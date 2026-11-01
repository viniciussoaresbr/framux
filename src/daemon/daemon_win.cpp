// Loop do framuxd no Windows: o equivalente ao epoll de daemon.cpp.
//
// Thread principal: espera a fila de callbacks (main_queue_), o pedido de
// parada, a mudança na pasta da config e o tick de 50 ms. Conexões dos jogos
// chegam pela thread do PipeListener; cada jogo tem uma thread que lê as
// mensagens e as entrega à thread principal, onde roda a mesma lógica do Linux.
#include <windows.h>

#include <cinttypes>
#include <cstring>

#include "common/log.h"
#include "common/paths.h"
#include "common/shm.h"
#include "common/win_setup.h"
#include "common/win_util.h"
#include "daemon/daemon.h"
#include "framux/stats.h"
#include "framux_build_config.h"

namespace framux::daemon {

namespace {
constexpr uint64_t kTickNs = 50 * kNsPerMs;
constexpr uint64_t kReloadDebounceNs = 200 * kNsPerMs;
}  // namespace

Daemon::~Daemon() {
  request_stop();
  listener_.stop();
  // O ETW posta na main_queue_ e escreve nos rings das sessões externas.
  tracer_.reset();
  overlay_win_.reset();
  for (auto &t : readers_)
    if (t.joinable()) t.join();
  for (auto &[id, s] : sessions_) {
    shm::unmap(s->ring);
    ipc::pipe_close(s->fd);
  }
  shm::unmap(stats_);
  if (stats_map_) CloseHandle(stats_map_);
  if (stop_event_) CloseHandle(stop_event_);
}

void Daemon::request_stop() {
  stopping_ = true;
  if (stop_event_) SetEvent(stop_event_);
}

// Thread principal: novo jogo conectado.
void Daemon::add_client(int sock) {
  auto s = std::make_unique<Session>();
  s->id = next_session_id_++;
  s->fd = sock;
  s->peer_pid = int(ipc::pipe_peer_pid(sock));
  fd_session_[sock] = s->id;
  const uint64_t id = s->id;
  sessions_[id] = std::move(s);
  readers_.emplace_back([this, id, sock] { client_reader(id, sock); });
}

void Daemon::client_reader(uint64_t id, int sock) {
  SetThreadDescription(GetCurrentThread(), L"framuxd-client");
  while (!stopping_) {
    auto m = std::make_shared<ipc::Message>();
    const auto r = ipc::recv_msg_wait(sock, *m, stop_event_, INFINITE);
    if (r == ipc::RecvResult::WouldBlock) continue;
    if (stopping_) return;
    if (r != ipc::RecvResult::Ok) {
      const char *why = r == ipc::RecvResult::Closed ? "desconectou" : "mensagem inválida";
      main_queue_.post([this, id, why] { close_session(id, why); });
      return;
    }
    main_queue_.post([this, id, m] { dispatch(id, *m); });
  }
}

int Daemon::run(int) {
  std::vector<std::string> warnings;
  global_cfg_ = load_config("", &warnings);
  for (const auto &w : warnings) FRAMUX_LOG("config: %s", w.c_str());

  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  const std::string dir = runtime_dir();
  make_dirs(dir);
  DeleteFileW(win::widen(stopped_path()).c_str());
  socket_path_ = socket_path();

  std::string err;
  if (!listener_.start(
          socket_path_, [this](int sock) { main_queue_.post([this, sock] { add_client(sock); }); }, &err)) {
    FRAMUX_LOG("%s", err.c_str());
    return 1;
  }

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
  h.quit = [this] { request_stop(); };
  dbus_ = DbusService::open(std::move(h), &already, main_queue_);
  if (already) {
    FRAMUX_LOG("outro framuxd já está rodando");
    return 1;
  }

  const std::string cfg_dir = config_dir();
  make_dirs(cfg_dir);
  make_dirs(cfg_dir + "/games");
  HANDLE watch = FindFirstChangeNotificationW(win::widen(cfg_dir).c_str(), TRUE,
                                              FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);

  resolve_hotkey_backend();
  start_encoder_probe();

  // Jogos Direct3D/OpenGL: presents pelo ETW e contador numa janela própria.
  tracer_ = std::make_unique<PresentTracer>();
  if (!tracer_->start([this](uint32_t pid, PresentSource src, uint64_t sc,
                             uint64_t t) { on_external_present(pid, src, sc, t); },
                      &err)) {
    FRAMUX_LOG("jogos DirectX/OpenGL sem medição: %s", err.c_str());
    tracer_.reset();
  }
  overlay_win_ = std::make_unique<OverlayWindow>();
  if (!overlay_win_->start()) overlay_win_.reset();

  if (global_cfg_.stats_export.enabled) {
    stats_map_ = CreateFileMappingW(INVALID_HANDLE_VALUE,
                                    static_cast<SECURITY_ATTRIBUTES *>(ipc::user_only_security(nullptr)),
                                    PAGE_READWRITE, 0, sizeof(framux_stats), win::widen(stats_path()).c_str());
    if (stats_map_) {
      void *p = MapViewOfFile(stats_map_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(framux_stats));
      if (p) {
        stats_ = static_cast<framux_stats *>(p);
        std::memset(stats_, 0, sizeof *stats_);
        stats_->version = FRAMUX_STATS_VERSION;
        stats_->size = sizeof(framux_stats);
      }
    }
  }

  if (win::is_elevated())
    FRAMUX_LOG("aviso: framuxd rodando como administrador; jogos Vulkan não conseguem gravar nem tirar screenshot "
               "(o Windows impede o jogo de passar a imagem a um processo elevado). Feche e abra o Framux sem "
               "\"Executar como administrador\".");
  FRAMUX_LOG("framuxd %s escutando em %s (hotkeys: %s%s)", FRAMUX_VERSION, socket_path_.c_str(), backend_.c_str(),
             dbus_ ? "" : ", sem canal de controle");

  uint64_t next_tick = now_ns() + kTickNs;
  while (!stopping_) {
    HANDLE hs[3] = {static_cast<HANDLE>(main_queue_.event()), stop_event_, watch};
    const DWORD n = watch != INVALID_HANDLE_VALUE ? 3 : 2;
    const uint64_t now = now_ns();
    const DWORD timeout = now >= next_tick ? 0 : DWORD((next_tick - now) / kNsPerMs);
    const DWORD w = WaitForMultipleObjects(n, hs, FALSE, timeout);
    if (w == WAIT_OBJECT_0) main_queue_.run_pending();
    if (w == WAIT_OBJECT_0 + 2) {
      FindNextChangeNotification(watch);
      reload_at_ns_ = now_ns() + kReloadDebounceNs;
    }
    if (now_ns() >= next_tick) {
      tick();
      ext_tick();
      next_tick += kTickNs;
      if (next_tick < now_ns()) next_tick = now_ns() + kTickNs;
    }
  }

  FRAMUX_LOG("encerrando");
  for (auto &[id, s] : sessions_) {
    if (s->bench) bench_stop(*s, true);
    if (s->rec) video_stop(*s, true);
  }
  worker_.stop();
  main_queue_.run_pending();
  if (watch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(watch);
  dbus_.reset();
  return 0;
}

}  // namespace framux::daemon
