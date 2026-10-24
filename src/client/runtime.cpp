#include "client/runtime.h"

#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "client/x11_input.h"
#include "common/hotkey.h"
#include "common/ipc_socket.h"
#include "common/log.h"
#include "common/paths.h"

namespace framux::client {

namespace {

constexpr uint64_t kInputPollNs = 16 * kNsPerMs;
constexpr uint64_t kFocusPollNs = 500 * kNsPerMs;
constexpr uint64_t kStatsEveryFrames = 600;

enum Action { kActBenchmark = 0, kActOverlay, kActScreenshot, kActVideo, kActLoopSave, kActOverlayCorner, kActCount };
static_assert(kActCount == Runtime::kHotkeyActions);

class SpinGuard {
 public:
  explicit SpinGuard(std::atomic_flag &f) : f_(f) {
    while (f_.test_and_set(std::memory_order_acquire)) {
      // Contenção só acontece com presents simultâneos em várias threads.
    }
  }
  ~SpinGuard() { f_.clear(std::memory_order_release); }

 private:
  std::atomic_flag &f_;
};

void copy_str(char *dst, size_t n, const std::string &src) {
  std::strncpy(dst, src.c_str(), n - 1);
  dst[n - 1] = '\0';
}

}  // namespace

Runtime &Runtime::get() {
  // Nunca destruído: evita destrutores rodando no exit() do jogo com a
  // thread de IPC ainda viva. O kernel fecha o socket e o daemon trata como BYE.
  static Runtime *rt = new Runtime();
  return *rt;
}

Runtime::~Runtime() = default;

bool Runtime::init(ipc::Api api) {
  // Caminho rápido (todo present): sem mutex nem getpid().
  if (ready_.load(std::memory_order_acquire)) return enabled_;
  static std::mutex init_mu;
  std::lock_guard lk(init_mu);
  if (ready_.load(std::memory_order_relaxed)) return enabled_;
  static std::once_flag atfork;
  std::call_once(atfork, [] {
    // No filho de um fork() a thread de IPC não existe: reinicializa no
    // primeiro present do filho.
    pthread_atfork(nullptr, nullptr, [] { Runtime::get().ready_.store(false, std::memory_order_relaxed); });
  });
  const int pid = getpid();
  if (initialized_) {
    // Processo filho após fork(): a thread de IPC não existe aqui.
    thread_.detach();
    ring_ = nullptr;
    connected_ = false;
    swaps_.clear();
    x11_.reset();
    x11_tried_ = false;
  }
  initialized_ = true;
  init_pid_ = pid;
  api_ = api;
  id_ = current_identity();

  std::vector<std::string> warnings;
  Config cfg = load_config(id_.config_name, &warnings);
  for (const auto &w : warnings) FRAMUX_LOG("config: %s", w.c_str());
  if (skipped_by_auto_mode() || is_excluded(cfg, id_.exe_base, id_.game)) {
    FRAMUX_DEBUG("processo %s excluído por [filters]", id_.exe_base.c_str());
    enabled_ = false;
    ready_.store(true, std::memory_order_release);
    return false;
  }
  {
    SpinGuard g(present_lock_);
    apply_config_locked(cfg);
    overlay_.set_corner(cfg.overlay.corner);
  }

  {
    std::lock_guard pl(pending_mu_);
    hello_.proto_version = ipc::kProtoVersion;
    hello_.pid = pid;
    hello_.api = uint32_t(api);
    hello_.bits = sizeof(void *) == 8 ? 64 : 32;
    hello_.wine = id_.wine;
    copy_str(hello_.exe, sizeof hello_.exe, id_.exe);
    copy_str(hello_.comm, sizeof hello_.comm, id_.comm);
    copy_str(hello_.game, sizeof hello_.game, id_.game);
    copy_str(hello_.config_name, sizeof hello_.config_name, id_.config_name);
    if (!hello_.engine[0] && id_.wine && api != ipc::Api::Vulkan)
      copy_str(hello_.engine, sizeof hello_.engine, "wined3d");
  }

  if (wake_fd_ < 0) wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  stop_ = false;
  rejected_ = false;
  enabled_ = true;
  thread_ = std::thread([this] { ipc_thread_main(); });
  ready_.store(true, std::memory_order_release);
  FRAMUX_DEBUG("ativo em %s (jogo \"%s\", api %s)", id_.exe_base.c_str(), id_.game.c_str(), ipc::api_name(api));
  return true;
}

bool Runtime::skipped_by_auto_mode() {
  static const bool skip = [] {
    const char *a = std::getenv("FRAMUX_AUTO");
    if (!a || std::strcmp(a, "1") != 0) return false;
    auto set = [](const char *name) {
      const char *v = std::getenv(name);
      return v && *v && std::strcmp(v, "0") != 0;
    };
    const ProcessIdentity id = current_identity();
    const Config cfg = load_config(id.config_name, nullptr);
    const bool accept = auto_mode_accepts(cfg, id.exe_base, id.game, id.wine, set("SteamGameId") || set("SteamAppId"));
    if (!accept) FRAMUX_DEBUG("modo automático: %s não parece um jogo; camada inativa", id.exe_base.c_str());
    return !accept;
  }();
  return skip;
}

void Runtime::set_device_info(const std::string &engine, const std::string &driver, const std::string &gpu) {
  std::lock_guard pl(pending_mu_);
  if (!engine.empty()) copy_str(hello_.engine, sizeof hello_.engine, engine);
  if (!driver.empty()) copy_str(hello_.driver, sizeof hello_.driver, driver);
  if (!gpu.empty()) copy_str(hello_.gpu, sizeof hello_.gpu, gpu);
}

void Runtime::set_x11_window(uint32_t window) {
  SpinGuard g(present_lock_);
  x11_window_ = window;
  if (x11_) x11_->set_window(window);
}

void Runtime::forget_swapchain(uint32_t id) {
  SpinGuard g(present_lock_);
  swaps_.erase(id);
  if (primary_swapchain_ == id) primary_swapchain_ = 0;
}

void Runtime::apply_config_locked(const Config &cfg) {
  cfg_ = cfg;
  overlay_.set_config(cfg.overlay);
  for (auto &[id, s] : swaps_) s.fps.set_mode(cfg.overlay.update);
  const std::string *names[kActCount] = {&cfg.hotkeys.benchmark, &cfg.hotkeys.overlay,   &cfg.hotkeys.screenshot,
                                         &cfg.hotkeys.video,     &cfg.hotkeys.loop_save, &cfg.hotkeys.overlay_corner};
  for (int i = 0; i < kActCount; ++i) keys_[i] = parse_hotkey(*names[i]).value_or(Hotkey{});
  // O daemon resolve "auto" antes de enviar a config; sem daemon, "auto"
  // significa leitura in-game.
  ingame_hotkeys_ = cfg.hotkeys.backend == "ingame" || cfg.hotkeys.backend == "auto";
}

void Runtime::on_present(uint32_t swapchain_id, uint32_t width, uint32_t height, PresentResult &out) {
  const uint64_t now = now_ns();
  out = {};
  SpinGuard g(present_lock_);

  // Comandos pendentes da thread de IPC (sem bloquear se ela estiver ocupada).
  if (pending_.any && pending_mu_.try_lock()) {
    Pending p = std::move(pending_);
    pending_ = Pending{};
    pending_mu_.unlock();
    if (p.connected) connected_ = *p.connected;
    if (p.config_toml) {
      Config c;
      std::vector<std::string> warnings;
      if (apply_config_toml(c, *p.config_toml, "framuxd", &warnings)) apply_config_locked(c);
    }
    if (p.corner) overlay_.set_corner(*p.corner);
    // Comandos velhos (jogo pausado ou travado) são descartados aqui, no
    // present, e não na chegada: é o present que para quando o jogo pausa.
    auto fresh = [now](uint64_t issued) { return issued == 0 || now < issued || now - issued <= ipc::kCommandTtlNs; };
    for (const auto &ind : p.indicators) {
      uint32_t flags = ind.flags;
      if (!fresh(ind.issued_ns)) flags &= ~uint32_t(ipc::kIndFlash | ipc::kIndUnavailable);
      overlay_.on_indicators(flags, ind.bench_result, now);
    }
    for (auto &c : p.captures) {
      if (fresh(c.issued_ns))
        captures_.push_back(c);
      else
        FRAMUX_DEBUG("captura %llu descartada (comando com mais de 2 s)", (unsigned long long)c.request_id);
    }
  }

  SwapState &s = swaps_[swapchain_id];
  if (s.frame_id == 0) s.fps.set_mode(cfg_.overlay.update);
  s.fps.on_present(now);
  ++s.frame_id;
  if (ring_) ring_push(ring_, FrameRec{now, s.frame_id, swapchain_id, width, height, 0});

  // Swapchain "principal": o de mais apresentações recentes (§B.4).
  auto prim = swaps_.find(primary_swapchain_);
  if (prim == swaps_.end() || (swapchain_id != primary_swapchain_ && s.fps.fps() > prim->second.fps.fps()))
    primary_swapchain_ = swapchain_id;

  poll_input(now);

  const OverlayContent content = overlay_.compute(now, s.fps.display_fps(), s.fps.last_frametime_ns());
  build_overlay(content, width, height, s.draw);
  if (s.draw.visible) out.draw = &s.draw;

  if (swapchain_id == primary_swapchain_) {
    while (!captures_.empty()) {
      CaptureRequest c = captures_.front();
      captures_.pop_front();
      out.capture = c;
      break;
    }
  }

  overlay_cpu_ns_ += now_ns() - now;
  if (++overlay_frames_ >= kStatsEveryFrames && connected_) {
    ipc::StatsMsg st{ring_ ? ring_->dropped.load(std::memory_order_relaxed) : 0, overlay_cpu_ns_ / overlay_frames_};
    enqueue(ipc::MsgType::Stats, &st, sizeof st);
    overlay_cpu_ns_ = overlay_frames_ = 0;
  }
}

void Runtime::poll_input(uint64_t now) {
  if (now < next_input_poll_) return;
  next_input_poll_ = now + kInputPollNs;
  const bool want_focus = connected_ && now >= next_focus_check_;
  if (!ingame_hotkeys_ && !want_focus) return;

  if (!x11_ && !x11_tried_) {
    x11_tried_ = true;
    x11_ = X11Input::open();
    if (x11_ && x11_window_) x11_->set_window(x11_window_);
    FRAMUX_DEBUG("leitura de teclas in-game: %s", x11_ ? "X11" : "indisponível (sem DISPLAY)");
  }
  if (!x11_) return;

  if (want_focus) {
    next_focus_check_ = now + kFocusPollNs;
    const int f = x11_->focus_state();
    if (f != last_focus_) {
      last_focus_ = f;
      ipc::Focus msg{uint32_t(f), 0};
      enqueue(ipc::MsgType::Focus, &msg, sizeof msg);
    }
  }
  if (!ingame_hotkeys_ || !x11_->snapshot()) return;

  for (int i = 0; i < kActCount; ++i) {
    const bool p = x11_->pressed(keys_[i]);
    if (p == key_down_[i]) continue;
    key_down_[i] = p;
    // Combinação que passou a bater só porque um modificador foi solto.
    if (p && !x11_->newly_down(keys_[i])) continue;
    // No XWayland o estado do teclado reflete qualquer janela X em foco, e no
    // X11 é global: só aceita a tecla se a janela do jogo estiver em foco.
    if (p && x11_->focus_state() == 0) {
      key_down_[i] = false;
      continue;
    }
    on_hotkey(i, keys_[i], p, now);
  }
}

void Runtime::on_hotkey(int action, const Hotkey &hk, bool pressed, uint64_t now) {
  if (connected_) {
    ipc::KeyEvent ev{hk.keysym, hk.mods, pressed ? 1u : 0u, 0};
    enqueue(ipc::MsgType::KeyEvent, &ev, sizeof ev);
    return;
  }
  if (!pressed) return;
  // Sem daemon: o overlay continua funcionando; o resto mostra "-" (§C.3.1).
  if (action == kActOverlay)
    overlay_.toggle_visible();
  else if (action == kActOverlayCorner)
    overlay_.next_corner();
  else
    overlay_.show_unavailable(now);
}

void Runtime::enqueue(ipc::MsgType type, const void *body, size_t len, int fd) {
  Outgoing o{type, std::vector<uint8_t>(static_cast<const uint8_t *>(body), static_cast<const uint8_t *>(body) + len),
             fd};
  {
    std::lock_guard lk(out_mu_);
    if (out_.size() > 64) {  // daemon travado: descarta em vez de crescer
      if (fd >= 0) close(fd);
      return;
    }
    out_.push_back(std::move(o));
  }
  const uint64_t one = 1;
  if (wake_fd_ >= 0) (void)!write(wake_fd_, &one, sizeof one);
}

void Runtime::send_frame_image(const ipc::FrameImage &img, int memfd) {
  enqueue(ipc::MsgType::FrameImage, &img, sizeof img, memfd);
}

void Runtime::send_capture_error(uint64_t request_id, const char *msg) {
  ipc::ErrorMsg e{};
  e.request_id = request_id;
  std::strncpy(e.message, msg, sizeof e.message - 1);
  enqueue(ipc::MsgType::Error, &e, sizeof e);
}

void Runtime::shutdown() {
  stop_ = true;
  const uint64_t one = 1;
  if (wake_fd_ >= 0) (void)!write(wake_fd_, &one, sizeof one);
}

int Runtime::connect_once() {
  const std::string path = socket_path();
  if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return -1;
  int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (s < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strcpy(addr.sun_path, path.c_str());
  if (connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0) {
    close(s);
    return -1;
  }
  timeval tv{2, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

  ipc::Hello hello;
  {
    std::lock_guard pl(pending_mu_);
    hello = hello_;
  }
  ipc::Message m;
  if (!ipc::send_struct(s, ipc::MsgType::Hello, hello) || ipc::recv_msg(s, m) != ipc::RecvResult::Ok) {
    close(s);
    return -1;
  }
  if (m.type == ipc::MsgType::Reject) {
    const auto *r = m.as<ipc::Reject>();
    FRAMUX_LOG("framuxd recusou a conexão (protocolo %u, daemon %u): %.*s", ipc::kProtoVersion,
               r ? r->proto_version : 0, r ? int(sizeof r->reason) : 0, r ? r->reason : "");
    rejected_ = true;
    close(s);
    return -1;
  }
  const auto *w = m.as<ipc::Welcome>();
  if (m.type != ipc::MsgType::Welcome || !w || m.nfds < 1 || w->proto_version != ipc::kProtoVersion) {
    close(s);
    return -1;
  }
  const int ring_fd = m.take_fd(0);
  struct stat st;
  void *map = MAP_FAILED;
  if (fstat(ring_fd, &st) == 0 && size_t(st.st_size) >= sizeof(RingHeader))
    map = mmap(nullptr, size_t(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, ring_fd, 0);
  close(ring_fd);
  if (map == MAP_FAILED) {
    close(s);
    return -1;
  }
  auto *ring = static_cast<RingHeader *>(map);
  const uint32_t cap = ring->capacity;
  if (ring->magic != kRingMagic || cap == 0 || (cap & (cap - 1)) || ring_bytes(cap) > size_t(st.st_size)) {
    munmap(map, size_t(st.st_size));
    close(s);
    return -1;
  }
  const std::string toml = m.tail<ipc::Welcome>().substr(0, w->config_len);
  FRAMUX_DEBUG("conectado ao framuxd (sessão %llu)", (unsigned long long)w->session_id);

  {
    std::lock_guard pl(pending_mu_);
    pending_.config_toml = toml;
    pending_.connected = true;
    pending_.any = true;
  }
  {
    SpinGuard g(present_lock_);
    ring_ = ring;
    ring_size_ = size_t(st.st_size);
    connected_ = true;
    last_focus_ = -1;
  }
  timeval none{0, 0};
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof none);
  return s;
}

void Runtime::disconnect(int sock) {
  RingHeader *ring;
  size_t size;
  {
    SpinGuard g(present_lock_);
    ring = ring_;
    size = ring_size_;
    ring_ = nullptr;
    connected_ = false;
  }
  if (ring) munmap(ring, size);
  close(sock);
  std::lock_guard pl(pending_mu_);
  pending_.connected = false;
  pending_.any = true;
}

void Runtime::drop_outgoing() {
  std::lock_guard lk(out_mu_);
  for (auto &o : out_)
    if (o.fd >= 0) close(o.fd);
  out_.clear();
}

void Runtime::ipc_thread_main() {
  pthread_setname_np(pthread_self(), "framux-ipc");
  while (!stop_) {
    int sock = rejected_ ? -1 : connect_once();
    if (sock < 0) {
      // Daemon ausente: tenta de novo a cada segundo (§C.12 teste 10).
      drop_outgoing();
      pollfd pfd{wake_fd_, POLLIN, 0};
      poll(&pfd, 1, rejected_ ? 60000 : 1000);
      uint64_t v;
      (void)!read(wake_fd_, &v, sizeof v);
      continue;
    }
    bool alive = true;
    while (alive && !stop_) {
      pollfd pfds[2] = {{sock, POLLIN, 0}, {wake_fd_, POLLIN, 0}};
      if (poll(pfds, 2, 1000) < 0) continue;
      if (pfds[1].revents & POLLIN) {
        uint64_t v;
        (void)!read(wake_fd_, &v, sizeof v);
        std::deque<Outgoing> batch;
        {
          std::lock_guard lk(out_mu_);
          batch.swap(out_);
        }
        for (auto &o : batch) {
          const int nfds = o.fd >= 0 ? 1 : 0;
          if (!ipc::send_msg(sock, o.type, o.body.data(), o.body.size(), nullptr, 0, &o.fd, nfds)) alive = false;
          if (o.fd >= 0) close(o.fd);
        }
      }
      if (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
        ipc::Message m;
        const auto r = ipc::recv_msg(sock, m, MSG_DONTWAIT);
        if (r == ipc::RecvResult::Ok)
          handle_message(m);
        else if (r != ipc::RecvResult::WouldBlock)
          alive = false;
      }
    }
    if (!alive) FRAMUX_DEBUG("conexão com o framuxd perdida; tentando reconectar");
    disconnect(sock);
  }
}

void Runtime::handle_message(ipc::Message &m) {
  std::lock_guard pl(pending_mu_);
  switch (m.type) {
    case ipc::MsgType::SetOverlay:
      if (const auto *p = m.as<ipc::SetOverlay>(); p && p->corner <= uint32_t(ipc::Corner::Hidden))
        pending_.corner = ipc::Corner(p->corner);
      break;
    case ipc::MsgType::SetIndicators:
      if (const auto *p = m.as<ipc::SetIndicators>())
        pending_.indicators.push_back({p->flags, p->bench_result, p->issued_ns});
      break;
    case ipc::MsgType::CaptureFrame:
      if (const auto *p = m.as<ipc::CaptureFrame>())
        pending_.captures.push_back({p->request_id, p->include_overlay != 0, p->issued_ns});
      break;
    case ipc::MsgType::Config:
      if (const auto *p = m.as<ipc::ConfigMsg>())
        pending_.config_toml = m.tail<ipc::ConfigMsg>().substr(0, p->config_len);
      break;
    default: FRAMUX_DEBUG("mensagem %u ignorada", unsigned(m.type)); return;
  }
  pending_.any = true;
}

}  // namespace framux::client
