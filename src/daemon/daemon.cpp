#include "daemon/daemon.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "common/ipc_socket.h"
#include "common/log.h"
#include "common/overlay.h"
#include "common/paths.h"
#include "core/image.h"
#include "core/naming.h"
#include "daemon/x11_hotkeys.h"
#include "framux/stats.h"
#include "framux_build_config.h"

namespace framux::daemon {

namespace {

constexpr uint64_t kTickNs = 50 * kNsPerMs;  // leitura dos rings a 20 Hz
constexpr uint64_t kShotTimeoutNs = 5 * kNsPerSec;
constexpr uint64_t kReloadDebounceNs = 200 * kNsPerMs;

enum Action { kActBenchmark = 0, kActOverlay, kActScreenshot, kActVideo, kActLoopSave, kActOverlayCorner, kActCount };

// Tags no epoll: fds fixos com valores especiais, clientes pelo próprio fd.
enum : uint64_t {
  kTagListen = 1ull << 40,
  kTagSignal,
  kTagTimer,
  kTagInotify,
  kTagQueue,
  kTagX11,
  kTagDbus,
};

void ep_add(int ep, int fd, uint64_t tag, uint32_t events = EPOLLIN) {
  epoll_event ev{};
  ev.events = events;
  ev.data.u64 = tag;
  epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

std::string json_str(const std::string &s) {
  std::string o = "\"";
  for (unsigned char c : s) {
    if (c == '"' || c == '\\')
      o += '\\', o += char(c);
    else if (c < 0x20) {
      char b[8];
      std::snprintf(b, sizeof b, "\\u%04x", c);
      o += b;
    } else
      o += char(c);
  }
  return o + "\"";
}

std::string cstr(const char *p, size_t n) { return std::string(p, strnlen(p, n)); }

ActionResult no_session() {
  return {false, "io.github.framux.Error.NoSession", "nenhuma sessão de jogo ativa (rode o jogo com `framux run`)"};
}

// CPU (usuário + sistema) de todas as threads do daemon, em ns.
uint64_t process_cpu_ns() {
  rusage ru{};
  getrusage(RUSAGE_SELF, &ru);
  auto ns = [](const timeval &t) { return uint64_t(t.tv_sec) * 1'000'000'000ull + uint64_t(t.tv_usec) * 1000ull; };
  return ns(ru.ru_utime) + ns(ru.ru_stime);
}

}  // namespace

VideoMap::~VideoMap() {
  if (ring) munmap(ring, size);
}

uint32_t Session::primary_swapchain() const {
  uint32_t best = 0, best_fps = 0;
  uint64_t best_last = 0;
  for (const auto &[id, s] : swaps) {
    if (s.fps.fps() > best_fps || (s.fps.fps() == best_fps && s.last_present_ns > best_last)) {
      best = id;
      best_fps = s.fps.fps();
      best_last = s.last_present_ns;
    }
  }
  return best;
}

const SwapStat *Session::primary() const {
  auto it = swaps.find(primary_swapchain());
  return it == swaps.end() ? nullptr : &it->second;
}

Daemon::Daemon() = default;

Daemon::~Daemon() {
  for (auto &[id, s] : sessions_) {
    if (s->ring) munmap(s->ring, s->ring_size);
    if (s->fd >= 0) close(s->fd);
  }
  if (stats_) munmap(stats_, sizeof(framux_stats));
  if (owns_socket_file_) unlink(socket_path_.c_str());
  for (int fd : {epfd_, listen_fd_, signal_fd_, timer_fd_, inotify_fd_})
    if (fd >= 0) close(fd);
}

// ---------------------------------------------------------------------------
// Inicialização

bool Daemon::setup_socket(int listen_fd) {
  const std::string dir = runtime_dir();
  if (dir.empty()) {
    FRAMUX_LOG("XDG_RUNTIME_DIR não definido");
    return false;
  }
  mkdir(dir.c_str(), 0700);
  struct stat st;
  if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid()) {
    FRAMUX_LOG("%s não é um diretório do usuário", dir.c_str());
    return false;
  }
  chmod(dir.c_str(), 0700);
  socket_path_ = socket_path();

  if (listen_fd >= 0) {
    listen_fd_ = listen_fd;
    ipc::set_cloexec_nonblock(listen_fd_, true);
    return true;
  }
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (socket_path_.size() >= sizeof addr.sun_path) return false;
  std::strcpy(addr.sun_path, socket_path_.c_str());

  // Socket existente: se alguém atende, outro framuxd já está rodando.
  int probe = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (connect(probe, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0) {
    close(probe);
    FRAMUX_LOG("outro framuxd já está rodando (%s)", socket_path_.c_str());
    return false;
  }
  close(probe);
  unlink(socket_path_.c_str());

  listen_fd_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  const mode_t old = umask(0077);
  const bool ok = bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0;
  umask(old);
  if (!ok || listen(listen_fd_, 16) != 0) {
    FRAMUX_LOG("não consegui escutar em %s: %s", socket_path_.c_str(), std::strerror(errno));
    return false;
  }
  chmod(socket_path_.c_str(), 0600);
  owns_socket_file_ = true;
  return true;
}

void Daemon::resolve_hotkey_backend() {
  const std::string want = global_cfg_.hotkeys.backend;
  const char *session = std::getenv("XDG_SESSION_TYPE");
  const bool x11_session = session && !std::strcmp(session, "x11") && std::getenv("DISPLAY");
  std::string resolved = "ingame";
  if (want == "x11" || (want == "auto" && x11_session)) resolved = "x11";
  if (want == "portal" || want == "evdev")
    FRAMUX_LOG("backend de hotkeys \"%s\" ainda não implementado; usando leitura in-game", want.c_str());

  if (resolved == "x11" && !x11_) {
    x11_ = X11Hotkeys::open([this](uint32_t ks, uint32_t mods, bool pressed) { on_key(nullptr, ks, mods, pressed); });
    if (x11_)
      ep_add(epfd_, x11_->fd(), kTagX11);
    else {
      FRAMUX_LOG("sem conexão X11 para hotkeys globais; usando leitura in-game");
      resolved = "ingame";
    }
  }
  if (resolved == "ingame" && x11_) {
    epoll_ctl(epfd_, EPOLL_CTL_DEL, x11_->fd(), nullptr);
    x11_.reset();
  }
  if (x11_) x11_->set_bindings(bindings(global_cfg_));
  backend_ = resolved;
}

std::vector<Hotkey> Daemon::bindings(const Config &cfg) const {
  const std::string *names[kActCount] = {&cfg.hotkeys.benchmark, &cfg.hotkeys.overlay,   &cfg.hotkeys.screenshot,
                                         &cfg.hotkeys.video,     &cfg.hotkeys.loop_save, &cfg.hotkeys.overlay_corner};
  std::vector<Hotkey> out;
  for (const auto *n : names) out.push_back(parse_hotkey(*n).value_or(Hotkey{}));
  return out;
}

int Daemon::run(int listen_fd) {
  std::vector<std::string> warnings;
  global_cfg_ = load_config("", &warnings);
  for (const auto &w : warnings) FRAMUX_LOG("config: %s", w.c_str());

  epfd_ = epoll_create1(EPOLL_CLOEXEC);
  if (!setup_socket(listen_fd)) return 1;

  bool already = false;
  DbusHandlers h;
  h.status = [this](bool json) { return status_text(json); };
  h.benchmark = [this](const std::string &a, uint32_t d) { return bench_action(active_session(), a, d); };
  h.screenshot = [this](bool overlay) {
    return request_shot(active_session(), overlay ? std::optional<bool>(true) : std::nullopt, true);
  };
  h.overlay = [this](const std::string &a) { return overlay_action(active_session(), a); };
  h.video = [this](const std::string &what, const std::string &action) {
    if (what == "record") return video_action(active_session(), action);
    return ActionResult{false, "io.github.framux.Error.NotSupported",
                        "loop recording ainda não está disponível nesta versão"};
  };
  h.reload = [this] { reload_config(); };
  dbus_ = DbusService::open(std::move(h), &already);
  if (already) {
    FRAMUX_LOG("outro framuxd já está registrado no D-Bus");
    return 1;
  }

  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGINT);
  sigaddset(&mask, SIGTERM);
  sigaddset(&mask, SIGHUP);
  sigprocmask(SIG_BLOCK, &mask, nullptr);
  signal(SIGPIPE, SIG_IGN);
  signal_fd_ = signalfd(-1, &mask, SFD_CLOEXEC | SFD_NONBLOCK);

  timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  itimerspec its{{0, long(kTickNs)}, {0, long(kTickNs)}};
  timerfd_settime(timer_fd_, 0, &its, nullptr);

  inotify_fd_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  const std::string cfg_dir = config_dir();
  make_dirs(cfg_dir);
  make_dirs(cfg_dir + "/games");
  const uint32_t watch = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE;
  inotify_add_watch(inotify_fd_, cfg_dir.c_str(), watch);
  inotify_add_watch(inotify_fd_, (cfg_dir + "/games").c_str(), watch);
  const std::string main_cfg = main_config_path();
  const std::string main_dir = main_cfg.substr(0, main_cfg.rfind('/'));
  if (main_dir != cfg_dir) inotify_add_watch(inotify_fd_, main_dir.c_str(), watch);

  ep_add(epfd_, listen_fd_, kTagListen);
  ep_add(epfd_, signal_fd_, kTagSignal);
  ep_add(epfd_, timer_fd_, kTagTimer);
  ep_add(epfd_, inotify_fd_, kTagInotify);
  ep_add(epfd_, main_queue_.fd(), kTagQueue);
  if (dbus_) ep_add(epfd_, dbus_->fd(), kTagDbus, dbus_->events());

  resolve_hotkey_backend();
  start_encoder_probe();

  if (global_cfg_.stats_export.enabled) {
    const std::string path = stats_path();
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
    if (fd >= 0 && ftruncate(fd, sizeof(framux_stats)) == 0) {
      void *p = mmap(nullptr, sizeof(framux_stats), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
      if (p != MAP_FAILED) {
        stats_ = static_cast<framux_stats *>(p);
        std::memset(stats_, 0, sizeof *stats_);
        stats_->version = FRAMUX_STATS_VERSION;
        stats_->size = sizeof(framux_stats);
      }
    }
    if (fd >= 0) close(fd);
  }

  FRAMUX_LOG("framuxd %s escutando em %s (hotkeys: %s%s)", FRAMUX_VERSION, socket_path_.c_str(), backend_.c_str(),
             dbus_ ? "" : ", sem D-Bus");

  epoll_event evs[32];
  while (running_) {
    if (dbus_) {
      epoll_event ev{};
      ev.events = dbus_->events();
      ev.data.u64 = kTagDbus;
      epoll_ctl(epfd_, EPOLL_CTL_MOD, dbus_->fd(), &ev);
    }
    const int timeout = dbus_ ? dbus_->timeout_ms() : -1;
    const int n = epoll_wait(epfd_, evs, 32, timeout);
    if (n < 0 && errno != EINTR) break;
    for (int i = 0; i < n; ++i) {
      const uint64_t tag = evs[i].data.u64;
      switch (tag) {
        case kTagListen: accept_clients(); break;
        case kTagSignal: {
          signalfd_siginfo si;
          while (read(signal_fd_, &si, sizeof si) == sizeof si) {
            if (si.ssi_signo == SIGHUP)
              reload_config();
            else
              running_ = false;
          }
          break;
        }
        case kTagTimer: {
          uint64_t exp;
          (void)!read(timer_fd_, &exp, sizeof exp);
          tick();
          break;
        }
        case kTagInotify: {
          char buf[4096];
          while (read(inotify_fd_, buf, sizeof buf) > 0) {
          }
          reload_at_ns_ = now_ns() + kReloadDebounceNs;
          break;
        }
        case kTagQueue: main_queue_.run_pending(); break;
        case kTagX11:
          if (x11_) x11_->dispatch();
          break;
        case kTagDbus: break;  // processado abaixo
        default: on_client(int(tag), evs[i].events); break;
      }
    }
    if (dbus_) dbus_->process();
  }

  FRAMUX_LOG("encerrando");
  // Fecha benchmarks e gravações em andamento antes de sair (arquivos
  // completos; o worker termina os pendentes no stop()).
  for (auto &[id, s] : sessions_) {
    if (s->bench) bench_stop(*s, true);
    if (s->rec) video_stop(*s, true);
  }
  worker_.stop();
  main_queue_.run_pending();
  return 0;
}

// ---------------------------------------------------------------------------
// Clientes

void Daemon::accept_clients() {
  for (;;) {
    const int fd = accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (fd < 0) return;
    ucred cred{};
    socklen_t len = sizeof cred;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != getuid()) {
      FRAMUX_LOG("conexão recusada: UID %u diferente", unsigned(cred.uid));
      close(fd);
      continue;
    }
    auto s = std::make_unique<Session>();
    s->id = next_session_id_++;
    s->fd = fd;
    s->peer_pid = cred.pid;
    fd_session_[fd] = s->id;
    ep_add(epfd_, fd, uint64_t(fd));
    sessions_[s->id] = std::move(s);
  }
}

void Daemon::on_client(int fd, uint32_t events) {
  auto it = fd_session_.find(fd);
  if (it == fd_session_.end()) return;
  const uint64_t id = it->second;
  Session &s = *sessions_[id];
  for (int i = 0; i < 64; ++i) {
    ipc::Message m;
    const auto r = ipc::recv_msg(fd, m, MSG_DONTWAIT);
    if (r == ipc::RecvResult::WouldBlock) break;
    if (r != ipc::RecvResult::Ok) {
      close_session(id, r == ipc::RecvResult::Closed ? "desconectou" : "mensagem inválida");
      return;
    }
    if (!s.hello_done) {
      if (m.type != ipc::MsgType::Hello) {
        close_session(id, "não enviou HELLO");
        return;
      }
      handle_hello(s, m);
      if (!s.hello_done) {
        close_session(id, "HELLO recusado");
        return;
      }
      continue;
    }
    if (m.type == ipc::MsgType::Bye) {
      close_session(id, "BYE");
      return;
    }
    handle_message(s, m);
  }
  if (events & (EPOLLHUP | EPOLLERR)) close_session(id, "desconectou");
}

void Daemon::close_session(uint64_t id, const char *why) {
  auto it = sessions_.find(id);
  if (it == sessions_.end()) return;
  Session &s = *it->second;
  // A memória do ring continua válida aqui mesmo com o jogo morto: drena e
  // fecha o benchmark com tudo o que foi apresentado (§C.12 teste 5).
  if (s.bench) bench_stop(s);
  if (s.rec) video_stop(s, false);  // o que já chegou vira um arquivo válido
  if (s.hello_done) FRAMUX_LOG("sessão %" PRIu64 " (%s, pid %d) encerrada: %s", id, s.game.c_str(), s.hello.pid, why);
  epoll_ctl(epfd_, EPOLL_CTL_DEL, s.fd, nullptr);
  close(s.fd);
  fd_session_.erase(s.fd);
  if (s.ring) munmap(s.ring, s.ring_size);
  for (auto sh = shots_.begin(); sh != shots_.end();) {
    if (sh->second.session == id) {
      if (dbus_ && sh->second.via_dbus) dbus_->emit_error(sh->first, "o jogo encerrou antes da captura");
      sh = shots_.erase(sh);
    } else {
      ++sh;
    }
  }
  sessions_.erase(it);
  update_active();
}

void Daemon::handle_hello(Session &s, const ipc::Message &m) {
  const auto *h = m.as<ipc::Hello>();
  if (!h) return;
  if (h->proto_version != ipc::kProtoVersion) {
    ipc::Reject rj{};
    rj.proto_version = ipc::kProtoVersion;
    std::snprintf(rj.reason, sizeof rj.reason,
                  "versão de protocolo incompatível: camada %u, framuxd %u (atualize ambos os pacotes)",
                  h->proto_version, ipc::kProtoVersion);
    ipc::send_struct(s.fd, ipc::MsgType::Reject, rj, MSG_DONTWAIT);
    FRAMUX_LOG("%s", rj.reason);
    return;
  }
  s.hello = *h;
  s.game = cstr(h->game, sizeof h->game);
  s.config_name = cstr(h->config_name, sizeof h->config_name);
  s.exe = cstr(h->exe, sizeof h->exe);
  if (s.game.empty()) s.game = "jogo";

  std::vector<std::string> warnings;
  s.cfg = load_config(s.config_name, &warnings);
  for (const auto &w : warnings) FRAMUX_LOG("config: %s", w.c_str());
  s.corner = s.cfg.overlay.corner;

  const int ring_fd = memfd_create("framux-ring", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  const size_t bytes = ring_bytes(kRingDefaultCapacity);
  void *map = MAP_FAILED;
  if (ring_fd >= 0 && ftruncate(ring_fd, off_t(bytes)) == 0)
    map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, ring_fd, 0);
  if (map == MAP_FAILED) {
    if (ring_fd >= 0) close(ring_fd);
    FRAMUX_LOG("sem memória para o ring da sessão");
    return;
  }
  fcntl(ring_fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL);
  s.ring = static_cast<RingHeader *>(map);
  s.ring_size = bytes;
  ring_init(s.ring, kRingDefaultCapacity);

  const std::string toml = session_config_toml(s);
  ipc::Welcome w{ipc::kProtoVersion, uint32_t(toml.size()), s.id};
  const bool ok = ipc::send_msg(s.fd, ipc::MsgType::Welcome, &w, sizeof w, toml.data(), toml.size(), &ring_fd, 1);
  close(ring_fd);
  if (!ok) return;
  s.hello_done = true;
  FRAMUX_LOG("sessão %" PRIu64 ": %s (pid %d, %s, %u bits%s%s%s)", s.id, s.game.c_str(), h->pid,
             ipc::api_name(ipc::Api(h->api)), h->bits, h->wine ? ", wine" : "", h->engine[0] ? ", " : "",
             cstr(h->engine, sizeof h->engine).c_str());
  update_active();
}

std::string Daemon::session_config_toml(const Session &s) const {
  Config c = s.cfg;
  c.hotkeys.backend = backend_;  // a camada só lê teclas se for "ingame"
  return config_to_toml(c);
}

void Daemon::handle_message(Session &s, ipc::Message &m) {
  switch (m.type) {
    case ipc::MsgType::KeyEvent:
      if (const auto *k = m.as<ipc::KeyEvent>()) {
        // A camada só envia teclas com a janela do jogo em foco.
        if (k->pressed) s.focus = 1;
        on_key(&s, k->keysym, k->mods, k->pressed != 0);
      }
      break;
    case ipc::MsgType::Focus:
      if (const auto *f = m.as<ipc::Focus>(); f && f->state <= 2) {
        s.focus = int(f->state);
        update_active();
      }
      break;
    case ipc::MsgType::Stats:
      if (const auto *st = m.as<ipc::StatsMsg>()) {
        s.dropped_records = st->dropped_records;
        s.overlay_cpu_ns = st->overlay_cpu_ns;
      }
      break;
    case ipc::MsgType::FrameImage: handle_frame_image(s, m); break;
    case ipc::MsgType::VideoBuffer: handle_video_buffer(s, m); break;
    case ipc::MsgType::VideoFrame: handle_video_frame(s, m); break;
    case ipc::MsgType::VideoCost:
      if (const auto *c = m.as<ipc::VideoCost>(); c && s.rec) {
        s.video_cost.cpu_ns += c->cpu_ns;
        s.video_cost.max_ns = std::max(s.video_cost.max_ns, c->max_ns);
        s.video_cost.frames += c->frames;
      }
      break;
    case ipc::MsgType::Error:
      if (const auto *e = m.as<ipc::ErrorMsg>()) {
        const std::string msg = cstr(e->message, sizeof e->message);
        FRAMUX_LOG("%s: %s", s.game.c_str(), msg.c_str());
        auto sh = shots_.find(e->request_id);
        if (sh != shots_.end() && sh->second.session == s.id) {
          if (dbus_ && sh->second.via_dbus) dbus_->emit_error(e->request_id, msg);
          notify("Screenshot falhou", msg, "dialog-error");
          shots_.erase(sh);
        }
      }
      break;
    default: FRAMUX_DEBUG("mensagem %u ignorada", unsigned(m.type)); break;
  }
}

void Daemon::handle_frame_image(Session &s, ipc::Message &m) {
  const auto *img = m.as<ipc::FrameImage>();
  if (!img || m.nfds < 1) return;
  auto it = shots_.find(img->request_id);
  if (it == shots_.end() || it->second.session != s.id) return;
  PendingShot shot = it->second;
  shots_.erase(it);
  send_indicators(s, ipc::kIndFlash);

  // Validação do fd recebido (§C.11.2): arquivo regular com o tamanho
  // anunciado e dimensões coerentes com o formato.
  const int fd = m.take_fd(0);
  struct stat st;
  const uint32_t bpp = vk_format_bpp(img->vk_format);
  const uint64_t need = uint64_t(img->offset) + img->size;
  std::string err;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || uint64_t(st.st_size) < need)
    err = "fd de imagem inválido";
  else if (!bpp)
    err = std::string("formato de swapchain não suportado: ") + std::to_string(img->vk_format);
  else if (img->width == 0 || img->height == 0 || img->width > 16384 || img->height > 16384 ||
           img->stride < uint64_t(img->width) * bpp || img->size < uint64_t(img->stride) * img->height)
    err = "dimensões de imagem inválidas";
  if (!err.empty()) {
    close(fd);
    FRAMUX_LOG("screenshot de %s: %s", s.game.c_str(), err.c_str());
    if (dbus_ && shot.via_dbus) dbus_->emit_error(img->request_id, err);
    return;
  }

  const ipc::FrameImage info = *img;
  const uint64_t req = img->request_id;
  worker_.post([this, fd, info, shot, req] {
    std::string error, path;
    void *map = mmap(nullptr, size_t(info.offset + info.size), PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
      error = "mmap da imagem falhou";
    } else {
      RawImage raw;
      raw.data = static_cast<const uint8_t *>(map) + info.offset;
      raw.size = size_t(info.size);
      raw.width = info.width;
      raw.height = info.height;
      raw.stride = info.stride;
      raw.vk_format = info.vk_format;
      raw.flip_y = info.flags & ipc::kImgFlipY;
      RgbImage rgb;
      std::vector<uint8_t> bytes;
      const bool ok =
          convert_image(raw, rgb, &error) && encode_image(rgb, shot.cfg.format, shot.cfg.jpeg_quality, bytes, &error);
      munmap(map, size_t(info.offset + info.size));
      const std::string folder = expand_path(shot.cfg.folder);
      if (ok && make_dirs(folder, &error)) {
        const std::string stem = shot.game + " " + file_timestamp(shot.wall, shot.centis);
        const int out =
            create_unique(folder, stem, std::string(".") + image_format_ext(shot.cfg.format), &path, &error);
        if (out >= 0 && !write_all_close(out, std::string(bytes.begin(), bytes.end()), &error)) path.clear();
      }
    }
    main_queue_.post([this, req, path, error, via = shot.via_dbus] {
      if (!path.empty()) {
        FRAMUX_LOG("screenshot salvo: %s", path.c_str());
        if (dbus_) dbus_->emit_file_saved(req, "screenshot", path);
        notify("Screenshot salvo", path, "camera-photo");
      } else {
        FRAMUX_LOG("screenshot falhou: %s", error.c_str());
        if (dbus_ && via) dbus_->emit_error(req, error);
        notify("Screenshot falhou", error, "dialog-error");
      }
    });
  });
}

// Novo buffer de quadros da camada (início da gravação ou janela maior).
void Daemon::handle_video_buffer(Session &s, ipc::Message &m) {
  const auto *vb = m.as<ipc::VideoBuffer>();
  if (!vb || m.nfds < 1) return;
  const int fd = m.take_fd(0);
  struct stat st;
  void *map = MAP_FAILED;
  // Validação (§C.11.2): memfd regular, cabeçalho coerente com o tamanho.
  if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && size_t(st.st_size) > kVideoRingDataOffset &&
      size_t(st.st_size) <= (size_t(1) << 32))
    map = mmap(nullptr, size_t(st.st_size), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (map == MAP_FAILED) {
    FRAMUX_LOG("%s: buffer de vídeo inválido", s.game.c_str());
    return;
  }
  auto vm = std::make_shared<VideoMap>();
  vm->ring = static_cast<VideoRingHeader *>(map);
  vm->size = size_t(st.st_size);
  vm->generation = vb->generation;
  const VideoRingHeader &h = *vm->ring;
  if (h.magic != kVideoRingMagic || h.slot_count != kVideoSlots || h.slot_bytes == 0 ||
      video_ring_bytes(h.slot_bytes) > vm->size) {
    FRAMUX_LOG("%s: cabeçalho do buffer de vídeo inválido", s.game.c_str());
    return;
  }
  s.video_map = std::move(vm);
}

void Daemon::handle_video_frame(Session &s, const ipc::Message &m) {
  const auto *vf = m.as<ipc::VideoFrame>();
  std::shared_ptr<VideoMap> vm = s.video_map;
  if (!vf || !vm || vf->generation != vm->generation || vf->slot >= kVideoSlots) return;
  auto release = [vm, slot = vf->slot] { vm->ring->state[slot].store(kSlotFree, std::memory_order_release); };
  const bool nv12 = vf->vk_format == ipc::kVkFormatNv12;
  const uint64_t need = uint64_t(vf->stride) * vf->height * (nv12 ? 3 : 2) / 2;
  if (vf->width == 0 || vf->height == 0 || vf->width > 16384 || vf->height > 16384 ||
      vf->stride < uint64_t(vf->width) * (nv12 ? 1 : 4) || (nv12 && (vf->width | vf->height) & 1) ||
      need > vm->ring->slot_bytes || (!nv12 && vk_format_bpp(vf->vk_format) != 4)) {
    FRAMUX_LOG_ONCE("%s: quadro de vídeo inválido descartado", s.game.c_str());
    release();
    return;
  }
  if (!s.rec) {  // chegou depois do VIDEO_STOP
    release();
    return;
  }
  if (vf->dropped) s.rec->add_layer_drops(vf->dropped);
  s.rec->push_video(video_slot(vm->ring, vf->slot), vf->width, vf->height, vf->stride, vf->vk_format,
                    vf->flags & ipc::kImgFlipY, vf->flags & ipc::kImgHalved, vf->present_ns, std::move(release));
}

// ---------------------------------------------------------------------------
// Ring, estatísticas e sessão ativa

void Daemon::drain(Session &s) {
  if (!s.ring) return;
  uint32_t primary = s.primary_swapchain();  // 0 antes do primeiro quadro
  ring_drain(
      s.ring,
      [&](const FrameRec &r) {
        SwapStat &sw = s.swaps[r.swapchain_id];
        if (sw.last_present_ns && r.present_ns > sw.last_present_ns)
          sw.last_frametime_ns = r.present_ns - sw.last_present_ns;
        sw.fps.on_present(r.present_ns);
        ++sw.frames;
        sw.last_present_ns = r.present_ns;
        sw.width = r.width;
        sw.height = r.height;
        ++s.total_frames;
        if (r.present_ns > s.last_present_ns) s.last_present_ns = r.present_ns;
        if (!primary) primary = r.swapchain_id;
        if (r.swapchain_id == primary) {
          s.presents.push_back(r.present_ns);
          if (!s.rec)
            while (s.presents.front() + 10'000'000'000ull < r.present_ns) s.presents.pop_front();
        }
        if (s.bench && r.present_ns >= s.bench->start_ns) {
          Benchmark &b = *s.bench;
          if (!b.swapchain) b.swapchain = r.swapchain_id;
          if (r.swapchain_id == b.swapchain) b.frames.push_back({r.present_ns, r.width, r.height});
        }
      },
      &s.lost_records);
  s.dropped_records = std::max<uint64_t>(s.dropped_records, s.ring->dropped.load(std::memory_order_relaxed));
}

void Daemon::update_active() {
  std::vector<SessionView> views;
  const uint64_t now = now_ns();
  for (const auto &[id, s] : sessions_) {
    if (!s->hello_done) continue;
    views.push_back({id, s->focus, s->last_present_ns, s->bench.has_value() || s->rec});
  }
  arbiter_.update(views, now);
}

Session *Daemon::active_session() {
  update_active();
  auto it = sessions_.find(arbiter_.active());
  return it == sessions_.end() || !it->second->hello_done ? nullptr : it->second.get();
}

void Daemon::tick() {
  const uint64_t now = now_ns();
  for (auto &[id, s] : sessions_) {
    if (!s->hello_done) continue;
    drain(*s);
    if (s->bench && s->bench->limit_s && now - s->bench->start_ns >= uint64_t(s->bench->limit_s) * kNsPerSec)
      bench_stop(*s);
    if (s->rec && s->rec->failed()) video_stop(*s, true);
    if (s->next_repeat_shot_ns && now >= s->next_repeat_shot_ns) {
      s->next_repeat_shot_ns = now + uint64_t(s->cfg.screenshot.repeat_every_s * 1e9);
      request_shot(s.get(), std::nullopt, false);
    }
  }
  for (auto it = shots_.begin(); it != shots_.end();) {
    if (now - it->second.issued_ns > kShotTimeoutNs) {
      FRAMUX_LOG("screenshot %" PRIu64 " expirou (o jogo não apresentou quadros?)", it->first);
      if (dbus_ && it->second.via_dbus) dbus_->emit_error(it->first, "tempo esgotado esperando um quadro do jogo");
      it = shots_.erase(it);
    } else {
      ++it;
    }
  }
  if (reload_at_ns_ && now >= reload_at_ns_) {
    reload_at_ns_ = 0;
    reload_config();
  }
  update_active();
  update_stats_export();
}

void Daemon::update_stats_export() {
  const uint64_t now = now_ns();
  Session *s = nullptr;
  auto it = sessions_.find(arbiter_.active());
  if (it != sessions_.end() && it->second->hello_done) s = it->second.get();
  const SwapStat *p = s ? s->primary() : nullptr;
  // Como o FrapsSharedData: FPS zera se o último quadro tem mais de 999 ms.
  const bool live = p && now - p->last_present_ns < 1000 * kNsPerMs;
  const float fps = live ? float(p->fps.fps()) : 0.0f;
  uint32_t flags = 0;
  if (s && s->bench) flags |= FRAMUX_STATS_BENCHMARK;
  if (s && s->rec) flags |= FRAMUX_STATS_RECORDING;

  if (stats_ && now - last_stats_write_ns_ >= 100 * kNsPerMs) {
    last_stats_write_ns_ = now;
    auto *seq = reinterpret_cast<std::atomic<uint32_t> *>(&stats_->seq);
    seq->fetch_add(1, std::memory_order_acq_rel);
    std::atomic_thread_fence(std::memory_order_release);
    stats_->fps = fps;
    stats_->frame_count = s ? s->total_frames : 0;
    stats_->last_present_ms = p ? p->last_present_ns / kNsPerMs : 0;
    stats_->frametime_ms = p ? float(p->last_frametime_ns / 1e6) : 0.0f;
    stats_->api = s ? s->hello.api : 0;
    std::memset(stats_->game, 0, sizeof stats_->game);
    if (s) std::strncpy(stats_->game, s->game.c_str(), sizeof stats_->game - 1);
    stats_->flags = flags;
    stats_->bench_elapsed_s = s && s->bench ? float((now - s->bench->start_ns) / 1e9) : 0.0f;
    stats_->bench_limit_s = s && s->bench ? float(s->bench->limit_s) : 0.0f;
    stats_->pid = s ? s->hello.pid : 0;
    std::atomic_thread_fence(std::memory_order_release);
    seq->fetch_add(1, std::memory_order_acq_rel);
  }
  if (dbus_ && s && now - last_stats_signal_ns_ >= kNsPerSec) {
    last_stats_signal_ns_ = now;
    dbus_->emit_stats(fps, s->total_frames, s->game, flags);
  }
}

// ---------------------------------------------------------------------------
// Ações

void Daemon::send_indicators(Session &s, uint32_t event_flags, float bench_result) {
  const uint32_t state = (s.bench ? uint32_t(ipc::kIndBenchmark) : 0u) | (s.rec ? uint32_t(ipc::kIndRecording) : 0u);
  ipc::SetIndicators ind{now_ns(), event_flags | state, bench_result};
  ipc::send_struct(s.fd, ipc::MsgType::SetIndicators, ind, MSG_DONTWAIT);
}

void Daemon::send_overlay(Session &s) {
  ipc::SetOverlay o{now_ns(), uint32_t(s.corner)};
  ipc::send_struct(s.fd, ipc::MsgType::SetOverlay, o, MSG_DONTWAIT);
}

void Daemon::on_key(Session *from, uint32_t keysym, uint32_t mods, bool pressed) {
  update_active();
  Session *s = active_session();
  // Tecla lida dentro de um jogo vale para ele mesmo se a arbitragem ainda
  // não trocou (ex.: a ativa está ocupada com benchmark: aí vale a ativa).
  if (from && (!s || !s->bench)) s = from;
  const Config &cfg = s ? s->cfg : global_cfg_;
  const auto keys = bindings(cfg);
  const Hotkey pressed_key{keysym, mods};
  int action = -1;
  for (int i = 0; i < kActCount; ++i)
    if (keys[i].enabled() && keys[i] == pressed_key) action = i;
  if (action < 0 || !pressed) return;
  if (!s) {
    FRAMUX_LOG("hotkey %s ignorada: nenhuma sessão de jogo", format_hotkey(pressed_key).c_str());
    return;
  }
  switch (action) {
    case kActBenchmark: bench_action(s, "toggle", 0); break;
    case kActOverlay: overlay_action(s, "toggle"); break;
    case kActOverlayCorner: overlay_action(s, "next"); break;
    case kActScreenshot:
      if (s->cfg.screenshot.repeat_every_s > 0) {
        // Com repetição ligada, a hotkey liga/desliga a série de capturas.
        if (s->next_repeat_shot_ns) {
          s->next_repeat_shot_ns = 0;
          FRAMUX_LOG("%s: repetição de screenshots desligada", s->game.c_str());
        } else {
          s->next_repeat_shot_ns = now_ns() + uint64_t(s->cfg.screenshot.repeat_every_s * 1e9);
          request_shot(s, std::nullopt, false);
        }
      } else {
        request_shot(s, std::nullopt, false);
      }
      break;
    case kActVideo:
      if (const ActionResult r = video_action(s, "toggle"); !r.ok) {
        notify("Gravação falhou", r.message, "dialog-error");
        send_indicators(*s, ipc::kIndUnavailable);
      }
      break;
    case kActLoopSave:
      FRAMUX_LOG_ONCE("loop recording ainda não está disponível nesta versão");
      send_indicators(*s, ipc::kIndUnavailable);
      break;
  }
}

ActionResult Daemon::bench_action(Session *s, const std::string &action, uint32_t duration_s) {
  if (!s) return no_session();
  bool start;
  if (action == "start")
    start = true;
  else if (action == "stop")
    start = false;
  else if (action == "toggle")
    start = !s->bench;
  else
    return {false, "org.freedesktop.DBus.Error.InvalidArgs", "ação inválida: " + action};
  if (start) {
    if (s->bench) return {false, "io.github.framux.Error.Busy", "benchmark já em andamento em " + s->game};
    bench_start(*s, duration_s);
    return {true, {}, "benchmark iniciado em " + s->game};
  }
  if (!s->bench) return {false, "io.github.framux.Error.NotRunning", "nenhum benchmark em andamento"};
  bench_stop(*s);
  return {true, {}, "benchmark encerrado em " + s->game};
}

void Daemon::bench_start(Session &s, uint32_t duration_s) {
  drain(s);  // quadros anteriores ao início não entram
  Benchmark b;
  b.start_ns = now_ns();
  b.start_wall = std::time(nullptr);
  b.limit_s = duration_s ? duration_s : s.cfg.benchmark.stop_after_s;
  b.swapchain = s.primary_swapchain();
  b.frames.reserve(1 << 16);
  s.bench = std::move(b);
  send_indicators(s, 0);
  FRAMUX_LOG("%s: benchmark iniciado%s", s.game.c_str(),
             s.bench->limit_s ? (" (para em " + std::to_string(s.bench->limit_s) + " s)").c_str() : "");
}

void Daemon::bench_stop(Session &s, bool sync) {
  if (!s.bench) return;
  drain(s);
  Benchmark b = std::move(*s.bench);
  s.bench.reset();
  if (b.limit_s) {
    const uint64_t end = b.start_ns + uint64_t(b.limit_s) * kNsPerSec;
    while (!b.frames.empty() && b.frames.back().present_ns > end) b.frames.pop_back();
  }
  const size_t n = b.frames.size();
  const double dur = n > 1 ? double(b.frames.back().present_ns - b.frames.front().present_ns) / 1e9 : 0.0;
  const float avg = dur > 0 ? float(double(n) / dur) : 0.0f;
  send_indicators(s, ipc::kIndBenchResult, avg);

  BenchMeta meta;
  meta.game = s.game;
  meta.start_time = b.start_wall;
  meta.api = ipc::api_name(ipc::Api(s.hello.api));
  meta.engine = cstr(s.hello.engine, sizeof s.hello.engine);
  meta.gpu = cstr(s.hello.gpu, sizeof s.hello.gpu);
  meta.driver = cstr(s.hello.driver, sizeof s.hello.driver);
  meta.timing = "present";
  meta.dropped_records = s.dropped_records + s.lost_records;
  BenchOptions opt{s.cfg.benchmark.log_frametimes, s.cfg.benchmark.log_fps, s.cfg.benchmark.log_summary};
  const std::string folder = expand_path(s.cfg.benchmark.folder);
  FRAMUX_LOG("%s: benchmark encerrado (%zu quadros, %.3f fps)", s.game.c_str(), n, avg);

  auto job = [this, folder, meta, opt, frames = std::move(b.frames)] {
    BenchWriteResult r = write_benchmark(folder, meta, frames, opt);
    main_queue_.post([this, r] {
      if (!r.error.empty()) {
        FRAMUX_LOG("benchmark: erro ao gravar: %s", r.error.c_str());
        notify("Benchmark: erro ao gravar", r.error, "dialog-error");
        return;
      }
      for (const auto &f : r.files) FRAMUX_DEBUG("gravado %s", f.c_str());
      char body[256];
      std::snprintf(body, sizeof body, "%" PRIu64 " quadros · média %.1f fps · mín %u · máx %u · 1%% low %.1f",
                    r.stats.frames, r.stats.avg_fps, r.stats.min_fps, r.stats.max_fps, r.stats.low_1pct_fps);
      FRAMUX_LOG("benchmark salvo: %s", r.summary_path.empty() ? r.files.front().c_str() : r.summary_path.c_str());
      if (dbus_) dbus_->emit_file_saved(0, "benchmark", r.summary_path.empty() ? r.files.front() : r.summary_path);
      notify("Benchmark salvo", body, "utilities-system-monitor");
    });
  };
  if (sync)
    job();
  else
    worker_.post(std::move(job));
}

ActionResult Daemon::request_shot(Session *s, std::optional<bool> include_overlay, bool via_dbus) {
  if (!s) return no_session();
  const uint64_t id = next_request_id_++;
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  PendingShot p;
  p.session = s->id;
  p.issued_ns = now_ns();
  p.wall = ts.tv_sec;
  p.centis = int(ts.tv_nsec / 10'000'000);
  p.game = sanitize_filename(s->game);
  p.cfg = s->cfg.screenshot;
  p.via_dbus = via_dbus;
  shots_[id] = p;
  ipc::CaptureFrame c{p.issued_ns, id, include_overlay.value_or(s->cfg.screenshot.include_overlay) ? 1u : 0u, 0};
  if (!ipc::send_struct(s->fd, ipc::MsgType::CaptureFrame, c, MSG_DONTWAIT)) {
    shots_.erase(id);
    return {false, "io.github.framux.Error.Failed", "falha ao falar com o jogo"};
  }
  ActionResult r;
  r.request_id = id;
  r.message = "captura solicitada";
  return r;
}

ActionResult Daemon::overlay_action(Session *s, const std::string &action) {
  if (!s) return no_session();
  const ipc::Corner home = s->cfg.overlay.corner;
  if (action == "toggle")
    s->corner = toggled_corner(s->corner, home);
  else if (action == "next")
    s->corner = next_corner(s->corner, home);
  else if (action == "hide")
    s->corner = ipc::Corner::Hidden;
  else if (action == "show")
    s->corner = home_corner(home);
  else
    return {false, "org.freedesktop.DBus.Error.InvalidArgs", "ação inválida: " + action};
  send_overlay(*s);
  return {true, {}, std::string("overlay: ") + corner_name(s->corner)};
}

ActionResult Daemon::video_action(Session *s, const std::string &action) {
  if (!s) return no_session();
  bool start;
  if (action == "start")
    start = true;
  else if (action == "stop")
    start = false;
  else if (action == "toggle")
    start = !s->rec;
  else
    return {false, "org.freedesktop.DBus.Error.InvalidArgs", "ação inválida: " + action};
  if (start) {
    if (s->rec) return {false, "io.github.framux.Error.Busy", "já gravando " + s->game};
    return video_start(*s);
  }
  if (!s->rec) return {false, "io.github.framux.Error.NotRunning", "nenhuma gravação em andamento"};
  video_stop(*s, true);
  return {true, {}, "gravação encerrada em " + s->game};
}

ActionResult Daemon::video_start(Session &s) {
  const Config::Video &v = s.cfg.video;
  std::string err, path;
  const std::string folder = expand_path(v.folder);
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  const std::string stem = sanitize_filename(s.game) + " " + file_timestamp(ts.tv_sec, int(ts.tv_nsec / 10'000'000));
  const int fd = make_dirs(folder, &err) ? create_unique(folder, stem, ".mkv", &path, &err) : -1;
  if (fd < 0) {
    FRAMUX_LOG("gravação: %s", err.c_str());
    return {false, "io.github.framux.Error.Failed", "não consegui criar o arquivo: " + err};
  }
  RecorderOptions opt;
  opt.fd = fd;
  opt.path = path;
  opt.game = s.game;
  opt.fps = v.fps;
  opt.half = v.scale == "half";
  opt.encoder = v.encoder;
  opt.quality = v.quality;
  opt.to_mp4 = v.container == "mp4";
  opt.audio = s.cfg.audio.game;
  opt.available = encoders_.available;
  s.rec = std::make_shared<Recorder>(std::move(opt));
  s.rec_start_ns = now_ns();
  drain(s);
  s.fps_before = compute_stats({s.presents.begin(), s.presents.end()});
  s.presents.clear();
  s.rec_cpu_start_ns = process_cpu_ns();
  s.video_cost = {};
  if (s.cfg.audio.game) {
    std::weak_ptr<Recorder> weak = s.rec;
    s.audio = GameAudio::start(
        s.hello.pid, s.peer_pid,
        [weak](const float *samples, uint32_t frames, uint64_t t) {
          if (auto r = weak.lock()) r->push_audio(samples, frames, t);
        },
        &err);
    if (!s.audio) FRAMUX_LOG("gravação sem áudio: %s", err.c_str());
  }
  // A camada converte para NV12 (e reduz) na GPU, a não ser no sem perda,
  // que precisa do RGB. Camadas antigas ignoram e mandam RGB inteiro.
  const bool rgb = v.quality == "lossless" || v.encoder == "utvideo";
  const uint32_t mode = rgb ? 0 : ipc::kVideoYuv | (v.scale == "half" ? ipc::kVideoHalf : 0);
  ipc::VideoStart vs{s.rec_start_ns, uint32_t(v.fps), mode};
  ipc::send_struct(s.fd, ipc::MsgType::VideoStart, vs, MSG_DONTWAIT);
  send_indicators(s, 0);
  update_active();
  FRAMUX_LOG("%s: gravação iniciada → %s", s.game.c_str(), path.c_str());
  return {true, {}, path};
}

// Uma linha para o log ao parar: FPS do jogo antes × durante, o que a cópia
// custou na thread do jogo e quanto de CPU o daemon (conversão + encoder)
// usou. É o que diz se a gravação está pesando no jogo.
std::string Daemon::recording_cost(Session &s) {
  const uint64_t wall = now_ns() - s.rec_start_ns;
  const double cores = wall ? double(process_cpu_ns() - s.rec_cpu_start_ns) / double(wall) : 0;
  const long ncpu = std::max(1L, sysconf(_SC_NPROCESSORS_ONLN));
  const BenchStats during = compute_stats({s.presents.begin(), s.presents.end()});
  auto fps = [](const BenchStats &b, char *out, size_t n) {
    if (b.frames < 2)
      std::snprintf(out, n, "?");
    else
      std::snprintf(out, n, "%.0f fps (1%% low %.0f)", b.avg_fps, b.low_1pct_fps);
  };
  char before[48], now[48], copy[96], out[384];
  fps(s.fps_before, before, sizeof before);
  fps(during, now, sizeof now);
  const ipc::VideoCost &c = s.video_cost;
  if (c.frames)
    std::snprintf(copy, sizeof copy, "%.2f ms por quadro gravado (pior %.1f ms)",
                  double(c.cpu_ns) / double(c.frames) / 1e6, double(c.max_ns) / 1e6);
  else
    std::snprintf(copy, sizeof copy, "sem dados (camada anterior à 0.4.2)");
  std::snprintf(out, sizeof out,
                "jogo %s antes, %s gravando; cópia na thread do jogo %s; framuxd %.2f núcleos (%.0f%% da CPU)", before,
                now, copy, cores, 100.0 * cores / double(ncpu));
  return out;
}

void Daemon::video_stop(Session &s, bool tell_layer) {
  if (!s.rec) return;
  if (tell_layer) ipc::send_msg(s.fd, ipc::MsgType::VideoStop, nullptr, 0, nullptr, 0, nullptr, 0, MSG_DONTWAIT);
  s.audio.reset();  // para de alimentar o Recorder antes de finalizar
  drain(s);
  const std::string cost = recording_cost(s);
  std::shared_ptr<Recorder> rec = std::move(s.rec);
  if (tell_layer) send_indicators(s, 0);
  const std::string game = s.game;
  FRAMUX_LOG("%s: gravação encerrada; finalizando o arquivo", game.c_str());
  worker_.post([this, rec, game, cost] {
    const RecorderResult r = rec->finish();
    main_queue_.post([this, r, game, cost] {
      if (!r.ok) {
        FRAMUX_LOG("gravação de %s falhou: %s", game.c_str(), r.error.c_str());
        notify("Gravação falhou", r.error, "dialog-error");
        return;
      }
      const int secs = int(std::lround(r.seconds));
      char body[512];
      std::snprintf(body, sizeof body, "%s\n%d min %02d s · %.0f MB", r.path.c_str(), secs / 60, secs % 60,
                    double(r.bytes) / 1e6);
      FRAMUX_LOG("gravação salva: %s (%.1f s, %" PRIu64 " quadros, %" PRIu64 " repetidos, %" PRIu64 " descartados, %s)",
                 r.path.c_str(), r.seconds, r.frames, r.duplicated, r.dropped, r.encoder.c_str());
      FRAMUX_LOG("medição da gravação: %s", cost.c_str());
      if (dbus_) dbus_->emit_file_saved(0, "video", r.path);
      if (r.error.empty())
        notify("Gravação salva", body, "video-x-generic");
      else
        notify("Gravação interrompida", std::string(body) + "\n" + r.error, "dialog-warning");
    });
  });
}

// Detecta os encoders num processo filho: abrir NVENC/VA-API carrega CUDA e
// drivers que ficariam residentes no daemon ocioso (§B.10).
void Daemon::start_encoder_probe() {
  worker_.post([this] {
    EncoderProbe probe;
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC) != 0) return;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    char self[] = "/proc/self/exe";
    char arg[] = "--probe-encoders";
    char *argv[] = {self, arg, nullptr};
    pid_t pid = -1;
    const int r = posix_spawn(&pid, self, &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(pipefd[1]);
    std::string out;
    char buf[1024];
    ssize_t n;
    while (r == 0 && (n = read(pipefd[0], buf, sizeof buf)) > 0) out.append(buf, size_t(n));
    close(pipefd[0]);
    if (r == 0) waitpid(pid, nullptr, 0);
    std::istringstream in(out);
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("available ", 0) == 0)
        probe.available.push_back(line.substr(10));
      else if (line.rfind("missing ", 0) == 0)
        probe.missing = line.substr(8);
    }
    main_queue_.post([this, probe] {
      encoders_ = probe;
      encoders_probed_ = true;
      std::string list;
      for (const auto &e : probe.available) list += (list.empty() ? "" : ", ") + e;
      FRAMUX_LOG("encoders de vídeo: %s", list.empty() ? "nenhum" : list.c_str());
    });
  });
}

// ---------------------------------------------------------------------------
// Config, status e notificações

void Daemon::reload_config() {
  std::vector<std::string> warnings;
  global_cfg_ = load_config("", &warnings);
  for (const auto &w : warnings) FRAMUX_LOG("config: %s", w.c_str());
  resolve_hotkey_backend();
  for (auto &[id, s] : sessions_) {
    if (!s->hello_done) continue;
    const ipc::Corner old_default = s->cfg.overlay.corner;
    s->cfg = load_config(s->config_name, nullptr);
    if (s->cfg.overlay.corner != old_default) {
      s->corner = s->cfg.overlay.corner;
      send_overlay(*s);
    }
    const std::string toml = session_config_toml(*s);
    ipc::ConfigMsg c{uint32_t(toml.size()), 0};
    ipc::send_msg(s->fd, ipc::MsgType::Config, &c, sizeof c, toml.data(), toml.size(), nullptr, 0, MSG_DONTWAIT);
  }
  FRAMUX_LOG("configuração recarregada");
}

std::string Daemon::status_text(bool json) {
  update_active();
  const uint64_t now = now_ns();
  std::ostringstream o;
  if (json) {
    o << "{\"version\":" << json_str(FRAMUX_VERSION) << ",\"hotkey_backend\":" << json_str(backend_)
      << ",\"encoders\":[";
    for (size_t i = 0; i < encoders_.available.size(); ++i) o << (i ? "," : "") << json_str(encoders_.available[i]);
    o << "],\"encoders_missing\":" << json_str(encoders_.missing) << ",\"sessions\":[";
    bool first = true;
    for (const auto &[id, s] : sessions_) {
      if (!s->hello_done) continue;
      const SwapStat *p = s->primary();
      const bool live = p && now - p->last_present_ns < kNsPerSec;
      o << (first ? "" : ",") << "{\"id\":" << id << ",\"pid\":" << s->hello.pid << ",\"game\":" << json_str(s->game)
        << ",\"exe\":" << json_str(s->exe) << ",\"api\":" << json_str(ipc::api_name(ipc::Api(s->hello.api)))
        << ",\"bits\":" << int(s->hello.bits) << ",\"wine\":" << (s->hello.wine ? "true" : "false")
        << ",\"engine\":" << json_str(cstr(s->hello.engine, sizeof s->hello.engine))
        << ",\"gpu\":" << json_str(cstr(s->hello.gpu, sizeof s->hello.gpu))
        << ",\"active\":" << (id == arbiter_.active() ? "true" : "false") << ",\"focus\":" << s->focus
        << ",\"fps\":" << (live ? p->fps.fps() : 0) << ",\"frames\":" << s->total_frames
        << ",\"resolution\":" << json_str(p ? std::to_string(p->width) + "x" + std::to_string(p->height) : "")
        << ",\"overlay\":" << json_str(corner_name(s->corner)) << ",\"benchmark\":" << (s->bench ? "true" : "false")
        << ",\"recording\":" << (s->rec ? "true" : "false")
        << ",\"recording_s\":" << (s->rec ? (now - s->rec_start_ns) / kNsPerSec : 0)
        << ",\"dropped_records\":" << s->dropped_records + s->lost_records << "}";
      first = false;
    }
    o << "]}";
    return o.str();
  }
  o << "framuxd " << FRAMUX_VERSION << " · hotkeys: " << backend_ << (dbus_ ? "" : " · sem D-Bus") << "\n";
  bool any = false;
  for (const auto &[id, s] : sessions_) {
    if (!s->hello_done) continue;
    any = true;
    const SwapStat *p = s->primary();
    const bool live = p && now - p->last_present_ns < kNsPerSec;
    o << (id == arbiter_.active() ? "* " : "  ") << "[" << id << "] " << s->game << " (pid " << s->hello.pid << ", "
      << ipc::api_name(ipc::Api(s->hello.api)) << ", " << int(s->hello.bits) << " bits";
    if (s->hello.wine) o << ", wine";
    if (s->hello.engine[0]) o << ", " << cstr(s->hello.engine, sizeof s->hello.engine);
    o << ")\n      " << (live ? p->fps.fps() : 0) << " fps";
    if (p) o << " · " << p->width << "x" << p->height;
    o << " · " << s->total_frames << " quadros · overlay " << corner_name(s->corner);
    if (s->bench) o << " · benchmark " << (now - s->bench->start_ns) / kNsPerSec << " s";
    if (s->rec) o << " · gravando " << (now - s->rec_start_ns) / kNsPerSec << " s";
    if (s->hello.gpu[0]) o << "\n      " << cstr(s->hello.gpu, sizeof s->hello.gpu);
    o << "\n";
  }
  if (!any) o << "nenhuma sessão de jogo\n";
  return o.str();
}

void Daemon::notify(const std::string &summary, const std::string &body, const std::string &icon) {
  if (dbus_ && global_cfg_.general.notifications) dbus_->notify(summary, body, icon);
}

}  // namespace framux::daemon
