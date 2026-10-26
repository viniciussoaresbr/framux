// Protocolo entre a camada/shim (processo do jogo) e o framuxd (§C.4).
//
// Transporte: socket Unix SOCK_SEQPACKET em $XDG_RUNTIME_DIR/framux/ipc.sock.
// Cada mensagem é um datagrama: MsgHeader seguido do payload do tipo. Campos
// de tamanho fixo e layout idêntico em 32 e 64 bits (só tipos de largura
// explícita e alinhamento natural), porque jogos i686 falam com o daemon x86_64.
#pragma once

#include <cstddef>
#include <cstdint>

namespace framux::ipc {

// Incrementar a cada mudança incompatível de mensagens ou do ring.
inline constexpr uint32_t kProtoVersion = 2;  // 2: gravação de vídeo

// Comandos mais velhos que isso são descartados pela camada (§C.3.3).
inline constexpr uint64_t kCommandTtlNs = 2'000'000'000ull;

inline constexpr size_t kMaxMessage = 64 * 1024;

enum class MsgType : uint32_t {
  // camada → daemon
  Hello = 1,
  FrameImage = 2,
  KeyEvent = 3,
  Focus = 4,
  Stats = 5,
  Bye = 6,
  Error = 7,
  VideoBuffer = 8,
  VideoFrame = 9,
  VideoCost = 10,  // daemons anteriores ignoram (mensagem desconhecida)
  // daemon → camada
  Welcome = 100,
  Reject = 101,
  SetOverlay = 102,
  SetIndicators = 103,
  CaptureFrame = 104,
  Config = 105,
  VideoStart = 106,
  VideoStop = 107,
};

struct MsgHeader {
  uint32_t type;
  uint32_t size;  // payload em bytes, sem o cabeçalho
};

enum class Api : uint32_t { Unknown = 0, Vulkan = 1, GlGlx = 2, GlEgl = 3 };

inline const char *api_name(Api a) {
  switch (a) {
    case Api::Vulkan: return "vulkan";
    case Api::GlGlx: return "gl_glx";
    case Api::GlEgl: return "gl_egl";
    default: return "unknown";
  }
}

struct Hello {
  uint32_t proto_version;
  int32_t pid;
  uint32_t api;  // Api
  uint8_t bits;  // 32 | 64
  uint8_t wine;  // bool
  uint8_t _pad[2];
  char exe[256];  // caminho de /proc/self/exe
  char comm[64];
  char game[128];         // nome usado em arquivos e na config por jogo
  char config_name[128];  // ex.: "wine-eldenring" (§C.6)
  char engine[64];        // VkApplicationInfo::pEngineName (DXVK, vkd3d…) ou "wined3d"
  char driver[128];
  char gpu[128];
};

struct Welcome {
  uint32_t proto_version;
  uint32_t config_len;  // bytes de TOML que seguem o struct
  uint64_t session_id;
  // + fd (SCM_RIGHTS): memfd do ring de timestamps (ring.h)
};

struct Reject {
  uint32_t proto_version;
  char reason[252];
};

enum class Corner : uint32_t { TopLeft = 0, TopRight = 1, BottomRight = 2, BottomLeft = 3, Hidden = 4 };

struct SetOverlay {
  uint64_t issued_ns;
  uint32_t corner;  // Corner
};

enum IndicatorFlags : uint32_t {
  kIndRecording = 1u << 0,
  kIndLoop = 1u << 1,
  kIndBenchmark = 1u << 2,
  kIndFlash = 1u << 3,        // flash de screenshot (120 ms)
  kIndBenchResult = 1u << 4,  // benchmark terminou: mostrar bench_result por 1 s
  kIndUnavailable = 1u << 5,  // ação pedida não disponível: mostrar "-"
};

struct SetIndicators {
  uint64_t issued_ns;
  uint32_t flags;
  float bench_result;  // FPS médio do benchmark recém-terminado
};

struct CaptureFrame {
  uint64_t issued_ns;
  uint64_t request_id;
  uint32_t include_overlay;
  uint32_t _pad;
};

// Inicia a gravação: a camada passa a copiar quadros do swapchain principal
// a no máximo `fps` por segundo para o buffer de vídeo (video_ring.h).
// Como a camada pode entregar os quadros (0.4.2+; antes, sempre RGB inteiro).
enum VideoModeFlags : uint32_t {
  kVideoYuv = 1u << 0,   // NV12 BT.709 convertido na GPU (o encoder não é RGB)
  kVideoHalf = 1u << 1,  // e já na metade do tamanho
};

// VkFormat de um quadro NV12: plano Y (stride × altura) seguido do UV
// intercalado (stride × altura/2).
inline constexpr uint32_t kVkFormatNv12 = 1000156003;  // VK_FORMAT_G8_B8R8_2PLANE_420_UNORM

struct VideoStart {
  uint64_t issued_ns;
  uint32_t fps;
  uint32_t mode;  // VideoModeFlags
};

// Buffer de quadros criado (ou trocado, se o tamanho mudou) pela camada.
struct VideoBuffer {
  uint32_t generation;
  uint32_t _pad;
  // + fd (SCM_RIGHTS): memfd com VideoRingHeader e os slots
};

// Quadro pronto no slot `slot` do buffer `generation`. O daemon devolve o
// slot zerando o estado dele no cabeçalho compartilhado.
struct VideoFrame {
  uint32_t generation;
  uint32_t slot;
  uint64_t present_ns;
  uint32_t width;
  uint32_t height;
  uint32_t vk_format;
  uint32_t stride;
  uint32_t flags;    // FrameImageFlags
  uint32_t dropped;  // quadros descartados pela camada desde o anterior
};

// Custo da gravação na thread do jogo (ler a cópia da GPU e copiar para o
// slot), acumulado desde a última VideoCost.
struct VideoCost {
  uint64_t cpu_ns;  // soma
  uint64_t max_ns;  // pior quadro
  uint32_t frames;  // quadros entregues
  uint32_t _pad;
};

struct ConfigMsg {
  uint32_t config_len;  // TOML que segue
  uint32_t _pad;
};

enum FrameImageFlags : uint32_t {
  kImgFlipY = 1u << 0,   // linhas de baixo para cima (glReadPixels)
  kImgHalved = 1u << 1,  // gravação: já reduzido à metade na GPU
};

struct FrameImage {
  uint64_t request_id;
  uint64_t present_ns;
  uint32_t width;
  uint32_t height;
  uint32_t vk_format;  // VkFormat do conteúdo do memfd
  uint32_t stride;     // bytes por linha
  uint32_t offset;
  uint32_t flags;  // FrameImageFlags
  uint64_t size;   // bytes válidos no memfd a partir de offset
  // + fd (SCM_RIGHTS): memfd com os pixels
};

enum KeyMods : uint32_t { kModCtrl = 1, kModShift = 2, kModAlt = 4, kModSuper = 8 };

struct KeyEvent {
  uint32_t keysym;  // keysym X11; botões de mouse usam kMouseKeysymBase + n
  uint32_t mods;
  uint32_t pressed;
  uint32_t _pad;
};

struct Focus {
  uint32_t state;  // 0 = sem foco, 1 = com foco, 2 = desconhecido
  uint32_t _pad;
};

struct StatsMsg {
  uint64_t dropped_records;
  uint64_t overlay_cpu_ns;  // média do custo de CPU do overlay por quadro
};

struct ErrorMsg {
  uint64_t request_id;
  char message[248];
};

static_assert(sizeof(Hello) == 912);
static_assert(sizeof(FrameImage) == 48);
static_assert(sizeof(Welcome) == 16);
static_assert(sizeof(VideoFrame) == 40);
static_assert(sizeof(VideoCost) == 24);

}  // namespace framux::ipc
