// framuxd (§C.3.3): sessões por jogo, ring de timestamps, benchmark,
// screenshots, gravação, hotkeys, API D-Bus e exportação pública de
// estatísticas.
// Loop único com epoll; codificação e escrita em disco na thread Worker.
#pragma once

#include <cstdint>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/config.h"
#include "common/fps_counter.h"
#include "common/hotkey.h"
#include "common/ipc_proto.h"
#include "common/ipc_socket.h"
#include "common/ring.h"
#include "common/video_ring.h"
#include "core/arbiter.h"
#include "core/bench_files.h"
#include "core/bench_stats.h"
#include "daemon/dbus_service.h"
#include "daemon/game_audio.h"
#include "daemon/recorder.h"
#include "daemon/worker.h"

struct framux_stats;

namespace framux::daemon {

class X11Hotkeys;

struct Benchmark {
  uint64_t start_ns = 0;
  std::time_t start_wall = 0;
  uint32_t limit_s = 0;
  uint32_t swapchain = 0;  // 0 = escolher no primeiro quadro
  std::vector<BenchFrame> frames;
};

// Mapeamento do buffer de quadros de uma geração (video_ring.h). Os quadros
// na fila do Recorder seguram uma referência: o buffer só some depois deles.
struct VideoMap {
  VideoRingHeader *ring = nullptr;
  size_t size = 0;
  uint32_t generation = 0;
  ~VideoMap();
};

struct SwapStat {
  FpsCounter fps;
  uint64_t frames = 0;
  uint64_t last_present_ns = 0;
  uint64_t last_frametime_ns = 0;
  uint32_t width = 0, height = 0;
};

struct Session {
  uint64_t id = 0;
  int fd = -1;
  bool hello_done = false;
  ipc::Hello hello{};
  std::string game, config_name, exe;
  Config cfg;
  RingHeader *ring = nullptr;
  size_t ring_size = 0;
  std::map<uint32_t, SwapStat> swaps;
  uint64_t last_present_ns = 0;
  uint64_t total_frames = 0;
  uint64_t lost_records = 0;
  uint64_t dropped_records = 0;
  uint64_t overlay_cpu_ns = 0;
  int focus = 2;
  ipc::Corner corner = ipc::Corner::TopLeft;
  std::optional<Benchmark> bench;
  uint64_t next_repeat_shot_ns = 0;  // 0 = repetição desligada
  int peer_pid = 0;                  // SO_PEERCRED (no Flatpak difere de hello.pid)

  // Gravação (§B.7)
  std::shared_ptr<Recorder> rec;
  std::unique_ptr<GameAudio> audio;
  std::shared_ptr<VideoMap> video_map;
  uint64_t rec_start_ns = 0;
  // Medição: presents do swapchain principal (últimos 10 s; durante a
  // gravação, desde o início dela), FPS de antes, CPU do daemon e o custo
  // informado pela camada (VideoCost).
  std::deque<uint64_t> presents;
  BenchStats fps_before;
  uint64_t rec_cpu_start_ns = 0;
  ipc::VideoCost video_cost{};

  uint32_t primary_swapchain() const;
  const SwapStat *primary() const;
};

class Daemon {
 public:
  Daemon();
  ~Daemon();
  // listen_fd ≥ 0 quando ativado por socket do systemd.
  int run(int listen_fd);

 private:
  struct PendingShot {
    uint64_t session = 0;
    uint64_t issued_ns = 0;
    std::time_t wall = 0;
    int centis = 0;
    std::string game;
    Config::Screenshot cfg;
    bool via_dbus = false;
  };

  bool setup_socket(int listen_fd);
  void accept_clients();
  void on_client(int fd, uint32_t events);
  void close_session(uint64_t id, const char *why);
  void handle_hello(Session &s, const ipc::Message &m);
  void handle_message(Session &s, ipc::Message &m);
  void handle_frame_image(Session &s, ipc::Message &m);
  void handle_video_buffer(Session &s, ipc::Message &m);
  void handle_video_frame(Session &s, const ipc::Message &m);

  void tick();
  void drain(Session &s);
  void update_active();
  Session *active_session();
  std::vector<Hotkey> bindings(const Config &cfg) const;

  void on_key(Session *from, uint32_t keysym, uint32_t mods, bool pressed);
  ActionResult bench_action(Session *s, const std::string &action, uint32_t duration_s);
  void bench_start(Session &s, uint32_t duration_s);
  void bench_stop(Session &s, bool final_write_sync = false);
  ActionResult request_shot(Session *s, std::optional<bool> include_overlay, bool via_dbus);
  ActionResult overlay_action(Session *s, const std::string &action);
  ActionResult video_action(Session *s, const std::string &action);
  ActionResult video_start(Session &s);
  // Para a gravação; a finalização (flush, trailer, MP4) vai para o worker.
  void video_stop(Session &s, bool tell_layer);
  std::string recording_cost(Session &s);
  void start_encoder_probe();
  void send_indicators(Session &s, uint32_t event_flags, float bench_result = 0);
  void send_overlay(Session &s);

  void reload_config();
  void resolve_hotkey_backend();
  std::string session_config_toml(const Session &s) const;
  std::string status_text(bool json);
  void notify(const std::string &summary, const std::string &body, const std::string &icon);
  void update_stats_export();

  int epfd_ = -1;
  int listen_fd_ = -1;
  int signal_fd_ = -1;
  int timer_fd_ = -1;
  int inotify_fd_ = -1;
  bool running_ = true;
  bool owns_socket_file_ = false;
  std::string socket_path_;

  Config global_cfg_;
  std::string backend_;  // "x11" ou "ingame" (resolvido)
  uint64_t reload_at_ns_ = 0;

  std::map<uint64_t, std::unique_ptr<Session>> sessions_;
  std::map<int, uint64_t> fd_session_;
  uint64_t next_session_id_ = 1;
  uint64_t next_request_id_ = 1;
  Arbiter arbiter_;
  std::map<uint64_t, PendingShot> shots_;

  Worker worker_;
  MainQueue main_queue_;
  std::unique_ptr<X11Hotkeys> x11_;
  std::unique_ptr<DbusService> dbus_;

  // Encoders detectados por `framuxd --probe-encoders` (processo filho).
  bool encoders_probed_ = false;
  EncoderProbe encoders_;

  framux_stats *stats_ = nullptr;
  uint64_t last_stats_signal_ns_ = 0;
  uint64_t last_stats_write_ns_ = 0;
};

}  // namespace framux::daemon
