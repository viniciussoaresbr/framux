// Estado por processo compartilhado pela camada Vulkan e pelo shim OpenGL:
// config, contadores de FPS por swapchain, overlay, hotkeys in-game e a
// conexão com o framuxd (thread própria; o caminho do present nunca bloqueia
// em I/O).
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "common/config.h"
#include "common/fps_counter.h"
#include "common/hotkey.h"
#include "common/identity.h"
#include "common/ipc_proto.h"
#include "common/ipc_socket.h"
#include "common/overlay.h"
#include "common/ring.h"
#include "common/video_ring.h"

namespace framux::client {

class X11Input;

struct CaptureRequest {
  uint64_t request_id = 0;
  bool include_overlay = false;
  uint64_t issued_ns = 0;  // relógio do daemon (CLOCK_MONOTONIC é do sistema)
};

struct PresentResult {
  const OverlayDraw *draw = nullptr;  // nullptr = nada a desenhar
  std::optional<CaptureRequest> capture;
  bool video = false;  // copiar este quadro (antes do overlay) para a gravação
};

class Runtime {
 public:
  // Ações com atalho (benchmark, overlay, screenshot, vídeo, loop, canto).
  static constexpr int kHotkeyActions = 6;

  static Runtime &get();

  // Inicializa na primeira chamada (idempotente). Devolve false se o
  // processo estiver excluído pelos filtros; nesse caso nada mais é feito.
  bool init(ipc::Api api);
  bool enabled() const { return enabled_; }

  // Modo automático (FRAMUX_AUTO=1) e o processo não parece um jogo: a camada
  // fica totalmente transparente. Calculado uma vez por processo.
  static bool skipped_by_auto_mode();

  // Informações para o HELLO; chamar antes do primeiro present.
  void set_device_info(const std::string &engine, const std::string &driver, const std::string &gpu);
  void set_x11_window(uint32_t window);

  uint32_t new_swapchain_id() { return next_swapchain_id_.fetch_add(1) + 1; }
  void forget_swapchain(uint32_t id);

  // Caminho quente: chamado no início de cada present.
  void on_present(uint32_t swapchain_id, uint32_t width, uint32_t height, PresentResult &out);

  // Envio de um quadro capturado (memfd com os pixels). Assume o fd.
  void send_frame_image(const ipc::FrameImage &img, int memfd);
  void send_capture_error(uint64_t request_id, const char *msg);

  // Gravação: copia um quadro lido da GPU para um slot livre do buffer de
  // vídeo e avisa o daemon. `src` tem `height` linhas de `src_stride` bytes.
  // Sem slot livre (encoder atrasado), descarta. Devolve false se a gravação
  // não está ativa (o quadro chegou depois do VIDEO_STOP).
  bool video_write(const void *src, uint32_t width, uint32_t height, uint32_t src_stride, uint32_t vk_format,
                   uint32_t flags, uint64_t present_ns);
  bool video_active() const { return video_fps_.load(std::memory_order_relaxed) != 0; }
  uint32_t video_mode() const { return video_mode_.load(std::memory_order_relaxed); }  // ipc::VideoModeFlags
  // Tempo que a camada/shim gastou para entregar um quadro ao video_write
  // (mapear a cópia da GPU + copiar para o slot); vai ao daemon em lotes.
  void add_video_cost(uint64_t ns);

  void shutdown();

 private:
  Runtime() = default;
  ~Runtime();

  struct SwapState {
    FpsCounter fps;
    uint64_t frame_id = 0;
    OverlayDraw draw;
  };

  struct Outgoing {
    ipc::MsgType type;
    std::vector<uint8_t> body;
    int fd = -1;
  };

  void ipc_thread_main();
  int connect_once();
  void disconnect(int sock);
  void drop_outgoing();
  void handle_message(ipc::Message &m);
  bool enqueue(ipc::MsgType type, const void *body, size_t len, int fd = -1);
  void set_video_locked(uint32_t fps, uint64_t now);
  void release_video_ring();
  void apply_config_locked(const Config &cfg);
  void poll_input(uint64_t now);
  void on_hotkey(int action, const Hotkey &hk, bool pressed, uint64_t now);

  std::atomic<bool> enabled_{false};
  std::atomic<bool> ready_{false};  // init() concluído neste processo
  bool initialized_ = false;
  int init_pid_ = 0;
  ipc::Api api_ = ipc::Api::Unknown;
  ProcessIdentity id_;
  ipc::Hello hello_{};

  // Protegido por present_lock_ (spinlock; presents de várias threads).
  std::atomic_flag present_lock_ = ATOMIC_FLAG_INIT;
  Config cfg_;
  OverlayState overlay_;
  std::map<uint32_t, SwapState> swaps_;
  uint32_t primary_swapchain_ = 0;
  RingHeader *ring_ = nullptr;
  size_t ring_size_ = 0;
  bool connected_ = false;
  std::deque<CaptureRequest> captures_;
  std::unique_ptr<X11Input> x11_;
  bool x11_tried_ = false;
  uint32_t x11_window_ = 0;
  Hotkey keys_[kHotkeyActions];
  bool key_down_[kHotkeyActions] = {};
  bool ingame_hotkeys_ = false;
  uint64_t next_input_poll_ = 0;
  uint64_t next_focus_check_ = 0;
  int last_focus_ = -1;
  uint64_t overlay_cpu_ns_ = 0, overlay_frames_ = 0;

  // Gravação: ritmo (present_lock_) e buffer compartilhado (video_mu_).
  std::atomic<uint32_t> video_fps_{0};
  std::atomic<uint32_t> video_mode_{0};
  uint64_t video_period_ns_ = 0;
  uint64_t video_due_ns_ = 0;
  std::mutex video_mu_;
  VideoRingHeader *video_ring_ = nullptr;
  size_t video_ring_size_ = 0;
  uint32_t video_generation_ = 0;
  uint32_t video_dropped_ = 0;
  ipc::VideoCost video_cost_{};  // video_mu_

  // Comandos recebidos pela thread de IPC, consumidos no present.
  std::mutex pending_mu_;
  struct Pending {
    std::optional<ipc::Corner> corner;
    struct Indicators {
      uint32_t flags;
      float bench_result;
      uint64_t issued_ns;
    };
    std::deque<Indicators> indicators;
    std::deque<CaptureRequest> captures;
    std::optional<std::string> config_toml;
    std::optional<bool> connected;
    std::optional<uint32_t> video_fps;  // 0 = parar
    bool any = false;
  } pending_;

  // Fila de saída para a thread de IPC.
  std::mutex out_mu_;
  std::condition_variable out_cv_;
  std::deque<Outgoing> out_;
  int wake_fd_ = -1;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::atomic<uint32_t> next_swapchain_id_{0};
  bool rejected_ = false;
};

}  // namespace framux::client
