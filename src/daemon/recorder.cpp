#include "daemon/recorder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}
#else
#include <sys/resource.h>
#endif
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <variant>

#include "common/log.h"
#include "common/video_rate.h"
#include "core/naming.h"
#include "framux_build_config.h"

namespace framux::daemon {

namespace {

#ifdef _WIN32
enum class Kind { Nvenc, Amf, Qsv, Svt, X264, Utvideo };
#else
enum class Kind { Nvenc, Vaapi, Svt, X264, Utvideo };
#endif

struct EncDef {
  const char *key;     // [video] encoder
  const char *ffname;  // encoder do FFmpeg
  Kind kind;
};

// Ordem = preferência do modo automático: codificador da placa primeiro
// (quase nenhuma CPU), AV1 antes de H.264; sem placa, CPU. HEVC ficou de
// fora: o Fedora sem RPM Fusion não o decodifica. O teto de taxa
// (video_rate.h) deixa o H.264 leve para o openh264 do Fedora tocar.
// No Windows, a placa é a da NVIDIA (NVENC), AMD (AMF) ou Intel (QSV).
constexpr EncDef kDefs[] = {
#ifdef _WIN32
    {"av1_hw", "av1_nvenc", Kind::Nvenc},   {"av1_hw", "av1_amf", Kind::Amf},   {"av1_hw", "av1_qsv", Kind::Qsv},
    {"h264_hw", "h264_nvenc", Kind::Nvenc}, {"h264_hw", "h264_amf", Kind::Amf}, {"h264_hw", "h264_qsv", Kind::Qsv},
#else
    {"av1_hw", "av1_nvenc", Kind::Nvenc},   {"av1_hw", "av1_vaapi", Kind::Vaapi},
    {"h264_hw", "h264_nvenc", Kind::Nvenc}, {"h264_hw", "h264_vaapi", Kind::Vaapi},
#endif
    {"av1_sw", "libsvtav1", Kind::Svt},     {"x264", "libx264", Kind::X264},
    {"utvideo", "utvideo", Kind::Utvideo},
};

bool on_cpu(Kind k) { return k == Kind::Svt || k == Kind::X264 || k == Kind::Utvideo; }

// Codificador no processador disputa os núcleos com o jogo. Com nice 10 a
// thread perde a disputa: a gravação descarta quadros em vez de o jogo travar.
// As threads criadas depois (do SVT-AV1, do Ut Video, do swscale) herdam.
#ifdef _WIN32
// No Windows a prioridade é por thread e não passa às threads criadas depois.
void yield_to_game() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST); }
#else
void yield_to_game() { setpriority(PRIO_PROCESS, gettid(), 10); }
#endif

// Quadros já convertidos esperando o encoder (~270 ms a 60 fps): cobrem a
// abertura do encoder e os picos do SVT-AV1 sem segurar os slots do jogo.
constexpr size_t kMaxQueuedFrames = 16;

std::string av_err(int e) {
  char buf[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(e, buf, sizeof buf);
  return buf;
}

AVPixelFormat src_format(uint32_t vk) {
  switch (vk) {
    case 37:  // R8G8B8A8_UNORM
    case 43:  // R8G8B8A8_SRGB
    case 51:  // A8B8G8R8_UNORM_PACK32
    case 57: return AV_PIX_FMT_RGB0;
    case 44:  // B8G8R8A8_UNORM
    case 50: return AV_PIX_FMT_BGR0;
    case 58: return AV_PIX_FMT_X2RGB10LE;     // A2R10G10B10_UNORM_PACK32
    case 64: return AV_PIX_FMT_X2BGR10LE;     // A2B10G10R10_UNORM_PACK32
    case 1000156003: return AV_PIX_FMT_NV12;  // convertido na GPU pela camada
    default: return AV_PIX_FMT_NONE;
  }
}

// Primeiro formato de `want` aceito pelo encoder.
AVPixelFormat pick_format(const AVCodec *codec, std::initializer_list<AVPixelFormat> want) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(61, 13, 100)
  const void *list = nullptr;
  int n = 0;
  if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &list, &n) < 0 || !list)
    return *want.begin();
  const auto *fmts = static_cast<const AVPixelFormat *>(list);
#else
  // Antes do FFmpeg 7.1: lista terminada em AV_PIX_FMT_NONE.
  const AVPixelFormat *fmts = codec->pix_fmts;
  if (!fmts) return *want.begin();
  int n = 0;
  while (fmts[n] != AV_PIX_FMT_NONE) ++n;
#endif
  for (AVPixelFormat w : want)
    for (int i = 0; i < n; ++i)
      if (fmts[i] == w) return w;
  return AV_PIX_FMT_NONE;
}

// Formato que o encoder recebe da conversão.
AVPixelFormat sw_format_for(const EncDef &def, const AVCodec *codec) {
  switch (def.kind) {
    case Kind::Nvenc: return pick_format(codec, {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P});
#ifdef _WIN32
    case Kind::Amf:
    case Kind::Qsv: return pick_format(codec, {AV_PIX_FMT_NV12});  // da memória do sistema
#else
    case Kind::Vaapi: return AV_PIX_FMT_NV12;  // superfícies NV12
#endif
    case Kind::Svt:
    case Kind::X264: return AV_PIX_FMT_YUV420P;
    case Kind::Utvideo: return pick_format(codec, {AV_PIX_FMT_GBRP});
  }
  return AV_PIX_FMT_NONE;
}

int quality_value(const std::string &q, int high, int medium, int low) {
  return q == "low" ? low : q == "medium" ? medium : high;
}

struct OpenEncoder {
  AVCodecContext *ctx = nullptr;
  AVBufferRef *hw_device = nullptr;
  AVBufferRef *hw_frames = nullptr;
  AVPixelFormat sw_format = AV_PIX_FMT_NONE;  // formato que o sws produz
  const EncDef *def = nullptr;

  void reset() {
    avcodec_free_context(&ctx);
    av_buffer_unref(&hw_frames);
    av_buffer_unref(&hw_device);
    sw_format = AV_PIX_FMT_NONE;
    def = nullptr;
  }
};

// Configura e abre `def` em w×h. Devolve false (com o motivo) se o encoder
// não existir ou o hardware recusar. `capped`: qualidade constante com teto
// de taxa; o VA-API sem modo VBR no driver tenta de novo sem teto (CQP).
// `gpu_frames`: quadros D3D11 NV12 (gravação pela GPU no Windows); só o
// NVENC e o AMF aceitam.
bool open_encoder(const EncDef &def, int w, int h, int fps, const std::string &quality, bool global_header,
                  OpenEncoder &out, std::string *why, bool capped = true, AVBufferRef *gpu_frames = nullptr) {
  out.reset();
  const AVCodec *codec = avcodec_find_encoder_by_name(def.ffname);
  if (!codec) {
    *why = std::string(def.ffname) + " não existe nesta build do FFmpeg";
    return false;
  }
  AVCodecContext *c = avcodec_alloc_context3(codec);
  c->width = w;
  c->height = h;
  c->time_base = {1, fps};
  c->framerate = {fps, 1};
  c->gop_size = fps * 2;  // keyframe a cada 2 s: busca rápida no player
  c->max_b_frames = 0;
  if (global_header) c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  c->color_range = AVCOL_RANGE_MPEG;
  c->colorspace = AVCOL_SPC_BT709;
  c->color_primaries = AVCOL_PRI_BT709;
  c->color_trc = AVCOL_TRC_BT709;
  const bool av1 = std::strncmp(def.ffname, "av1", 3) == 0 || def.kind == Kind::Svt;
  const int64_t cap = capped ? int64_t(video_max_bitrate(av1, quality, w, h, fps)) : 0;
  auto set_cap = [&] {
    c->rc_max_rate = cap;
    c->rc_buffer_size = int(std::min<int64_t>(cap * 2, INT32_MAX));  // ~2 s de folga para cenas pesadas
  };
  out.sw_format = sw_format_for(def, codec);
#ifdef _WIN32
  if (gpu_frames) {
    if (def.kind != Kind::Nvenc && def.kind != Kind::Amf) {
      *why = std::string(def.ffname) + " não recebe quadros da GPU";
      avcodec_free_context(&c);
      out.reset();
      return false;
    }
    // O quadro chega pronto (D3D11 NV12): sem conversão nem envio.
    out.sw_format = AV_PIX_FMT_D3D11;
    c->hw_frames_ctx = av_buffer_ref(gpu_frames);
  }
#endif
  int r = 0;
  switch (def.kind) {
    case Kind::Nvenc:
      c->pix_fmt = out.sw_format;
      av_opt_set(c->priv_data, "preset", "p4", 0);
      // Quadros da GPU: o NVENC espera a conversão de cada um terminar na GPU,
      // atrás do jogo. Com só ~3 em andamento (o padrão), essa espera limitava
      // a taxa; com 16, ela se sobrepõe.
      if (gpu_frames) av_opt_set_int(c->priv_data, "surfaces", 16, 0);
      av_opt_set(c->priv_data, "tune", "hq", 0);
      av_opt_set(c->priv_data, "rc", "vbr", 0);
      av_opt_set_int(c->priv_data, "cq", av1 ? quality_value(quality, 26, 32, 38) : quality_value(quality, 21, 26, 31),
                     0);
      c->bit_rate = 0;
      set_cap();
      break;
#ifdef _WIN32
    case Kind::Amf:
      // QVBR: qualidade constante com teto de taxa; quadros NV12 da memória do
      // sistema (ou D3D11, na gravação pela GPU).
      c->pix_fmt = out.sw_format;
      av_opt_set(c->priv_data, "quality", "balanced", 0);
      av_opt_set(c->priv_data, "rc", "qvbr", 0);
      av_opt_set_int(c->priv_data, "qvbr_quality_level",
                     av1 ? quality_value(quality, 26, 32, 38) : quality_value(quality, 21, 26, 31), 0);
      c->bit_rate = cap * 3 / 4;
      set_cap();
      break;
    case Kind::Qsv:
      // global_quality com rc_max_rate liga o QVBR (sem teto seria ICQ). Sem hw_frames: o
      // encoder abre a própria sessão e copia os quadros NV12 da memória do sistema.
      c->pix_fmt = out.sw_format;
      av_opt_set(c->priv_data, "preset", "faster", 0);
      c->global_quality = av1 ? quality_value(quality, 26, 32, 38) : quality_value(quality, 21, 26, 31);
      c->bit_rate = cap * 3 / 4;
      set_cap();
      break;
#else
    case Kind::Vaapi: {
      r = av_hwdevice_ctx_create(&out.hw_device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0);
      if (r < 0) {
        *why = "VA-API indisponível: " + av_err(r);
        avcodec_free_context(&c);
        return false;
      }
      out.hw_frames = av_hwframe_ctx_alloc(out.hw_device);
      auto *fc = reinterpret_cast<AVHWFramesContext *>(out.hw_frames->data);
      fc->format = AV_PIX_FMT_VAAPI;
      fc->sw_format = AV_PIX_FMT_NV12;
      fc->width = w;
      fc->height = h;
      fc->initial_pool_size = 8;
      if ((r = av_hwframe_ctx_init(out.hw_frames)) < 0) {
        *why = "VA-API sem superfícies NV12: " + av_err(r);
        avcodec_free_context(&c);
        out.reset();
        return false;
      }
      c->pix_fmt = AV_PIX_FMT_VAAPI;
      c->hw_frames_ctx = av_buffer_ref(out.hw_frames);
      if (cap) {
        av_opt_set(c->priv_data, "rc_mode", "VBR", 0);
        c->bit_rate = cap * 3 / 4;
        set_cap();
      } else {
        av_opt_set(c->priv_data, "rc_mode", "CQP", 0);
        // AV1 no VA-API usa o índice de quantização 0–255.
        c->global_quality = av1 ? quality_value(quality, 90, 130, 170) : quality_value(quality, 21, 26, 31);
      }
      break;
    }
#endif
    case Kind::Svt:
      c->pix_fmt = out.sw_format;
      av_opt_set(c->priv_data, "preset", "10", 0);  // tempo real em CPU de jogo
      av_opt_set_int(c->priv_data, "crf", quality_value(quality, 30, 36, 42), 0);
      set_cap();  // CRF com teto
      break;
    case Kind::X264:
      c->pix_fmt = out.sw_format;
      av_opt_set(c->priv_data, "preset", "veryfast", 0);
      av_opt_set_int(c->priv_data, "crf", quality_value(quality, 18, 23, 28), 0);
      set_cap();  // VBV
      break;
    case Kind::Utvideo:
      // Sem perda em RGB, para edição: compressão leve (~230 fps em 2560×1080 num i5 de 6 núcleos; o FFV1
      // não passava de 37) e arquivos grandes.
      c->pix_fmt = out.sw_format;
      c->colorspace = AVCOL_SPC_RGB;
      c->color_range = AVCOL_RANGE_JPEG;
      c->color_trc = AVCOL_TRC_IEC61966_2_1;
      // Um quadro por thread, em metade dos núcleos: com todos, o jogo travava.
#ifdef _WIN32
      c->thread_count = int(std::max<long>(long(std::thread::hardware_concurrency()) / 2, 1));
#else
      c->thread_count = int(std::max<long>(sysconf(_SC_NPROCESSORS_ONLN) / 2, 1));
#endif
      av_opt_set(c->priv_data, "pred", "median", 0);
      break;
  }
  if (out.sw_format == AV_PIX_FMT_NONE) {
    *why = std::string(def.ffname) + " sem formato de pixel compatível";
    avcodec_free_context(&c);
    out.reset();
    return false;
  }
  if ((r = avcodec_open2(c, codec, nullptr)) < 0) {
    *why = std::string(def.ffname) + ": " + av_err(r);
    avcodec_free_context(&c);
    out.reset();
#ifndef _WIN32
    if (def.kind == Kind::Vaapi && cap) return open_encoder(def, w, h, fps, quality, global_header, out, why, false);
#endif
    return false;
  }
  out.ctx = c;
  out.def = &def;
  return true;
}

// AVIO sobre o fd criado com O_EXCL|O_NOFOLLOW (§C.11.5). O buffer é const
// a partir do FFmpeg 7 (libavformat 61); o Ubuntu 24.04 tem o 6.1.
#if LIBAVFORMAT_VERSION_MAJOR >= 61
using AvioBuf = const uint8_t *;
#else
using AvioBuf = uint8_t *;
#endif
int fd_write(void *opaque, AvioBuf buf, int n) {
  const int fd = *static_cast<int *>(opaque);
  int done = 0;
  while (done < n) {
    const ssize_t w = write(fd, buf + done, size_t(n - done));
    if (w < 0) {
      if (errno == EINTR) continue;
      return AVERROR(errno);
    }
    done += int(w);
  }
  return n;
}

int64_t fd_seek(void *opaque, int64_t off, int whence) {
  const int fd = *static_cast<int *>(opaque);
#ifdef _WIN32
  // off_t e struct stat do CRT têm 32 bits: as variantes i64 passam dos 2 GB.
  if (whence & AVSEEK_SIZE) {
    struct _stati64 st;
    return _fstati64(fd, &st) == 0 ? int64_t(st.st_size) : AVERROR(errno);
  }
  const __int64 r = _lseeki64(fd, off, whence & ~AVSEEK_FORCE);
  return r < 0 ? AVERROR(errno) : int64_t(r);
#else
  if (whence & AVSEEK_SIZE) {
    struct stat st;
    return fstat(fd, &st) == 0 ? int64_t(st.st_size) : AVERROR(errno);
  }
  const off_t r = lseek(fd, off_t(off), whence & ~AVSEEK_FORCE);
  return r < 0 ? AVERROR(errno) : int64_t(r);
#endif
}

uint64_t file_size(const std::string &path) {
#ifdef _WIN32
  struct _stati64 st;
  return _stati64(path.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;
#else
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;
#endif
}

// MKV → MP4 sem recodificar. Devolve o caminho novo ou vazio (com `err`).
std::string remux_mp4(const std::string &mkv, std::string *err) {
  const size_t slash = mkv.rfind('/');
  const std::string dir = mkv.substr(0, slash);
  std::string stem = mkv.substr(slash + 1);
  if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, ".mkv") == 0) stem.resize(stem.size() - 4);
  std::string mp4;
  const int fd = create_unique(dir, stem, ".mp4", &mp4, err);
  if (fd < 0) return {};
  close(fd);

  AVFormatContext *in = nullptr, *out = nullptr;
  int r = avformat_open_input(&in, mkv.c_str(), nullptr, nullptr);
  if (r >= 0) r = avformat_find_stream_info(in, nullptr);
  if (r >= 0) r = avformat_alloc_output_context2(&out, nullptr, "mp4", mp4.c_str());
  for (unsigned i = 0; r >= 0 && i < in->nb_streams; ++i) {
    AVStream *os = avformat_new_stream(out, nullptr);
    r = avcodec_parameters_copy(os->codecpar, in->streams[i]->codecpar);
    os->codecpar->codec_tag = 0;
    // "hvc1" é o que players da Apple e navegadores aceitam para HEVC.
    if (os->codecpar->codec_id == AV_CODEC_ID_HEVC) os->codecpar->codec_tag = MKTAG('h', 'v', 'c', '1');
    os->time_base = in->streams[i]->time_base;
    os->disposition = in->streams[i]->disposition;  // só a trilha mixada toca por padrão
    av_dict_copy(&os->metadata, in->streams[i]->metadata, 0);
  }
  if (r >= 0) {
    av_dict_copy(&out->metadata, in->metadata, 0);
    r = avio_open(&out->pb, mp4.c_str(), AVIO_FLAG_WRITE);
  }
  AVDictionary *opts = nullptr;
  av_dict_set(&opts, "movflags", "+faststart", 0);
  if (r >= 0) r = avformat_write_header(out, &opts);
  av_dict_free(&opts);
  AVPacket *pkt = av_packet_alloc();
  while (r >= 0) {
    r = av_read_frame(in, pkt);
    if (r == AVERROR_EOF) {
      r = 0;
      break;
    }
    if (r < 0) break;
    av_packet_rescale_ts(pkt, in->streams[pkt->stream_index]->time_base, out->streams[pkt->stream_index]->time_base);
    pkt->pos = -1;
    r = av_interleaved_write_frame(out, pkt);
  }
  if (r >= 0) r = av_write_trailer(out);
  av_packet_free(&pkt);
  if (out && out->pb) avio_closep(&out->pb);
  avformat_free_context(out);
  avformat_close_input(&in);
  if (r < 0) {
    *err = "conversão para MP4 falhou: " + av_err(r);
    unlink(mp4.c_str());
    return {};
  }
  unlink(mkv.c_str());
  return mp4;
}

}  // namespace

// ---------------------------------------------------------------------------
// Detecção

EncoderProbe probe_encoders() {
  av_log_set_level(AV_LOG_QUIET);
  EncoderProbe p;
  for (const EncDef &d : kDefs) {
    bool known = false;
    for (const auto &k : p.available) known |= k == d.key;
    if (known) continue;
    OpenEncoder e;
    std::string why;
    if (open_encoder(d, 1280, 720, 60, "high", false, e, &why)) p.available.push_back(d.key);
    e.reset();
  }
  auto has = [&](const char *k) {
    for (const auto &a : p.available)
      if (a == k) return true;
    return false;
  };
  if (!has("h264_hw"))
    p.missing =
#ifdef _WIN32
        "H.264 por placa de vídeo indisponível. Atualize o driver da placa: NVIDIA (NVENC), AMD Adrenalin (AMF) "
        "ou Intel Arc/Iris (Quick Sync).";
#else
#if FRAMUX_DEBIAN
        "H.264 por placa de vídeo indisponível. AMD: instale mesa-va-drivers; Intel: intel-media-va-driver-non-free; "
#else
        "H.264 por placa de vídeo indisponível. AMD/Intel: instale mesa-va-drivers-freeworld do RPM Fusion; "
#endif
        "NVIDIA: use o driver proprietário.";
#endif
  return p;
}

// ---------------------------------------------------------------------------
// Conversão de cor

// No FFmpeg 8 (libswscale 9), o sws_scale_frame configura o contexto pelos
// próprios quadros. Antes dele (Ubuntu 24.04, Debian 13), o contexto é montado
// para cada tamanho e formato, com a matriz e a faixa de cor de cada quadro.
struct Scaler {
  int flags = 0;    // 0: padrão do libswscale
  int threads = 0;  // 0: padrão do libswscale
  SwsContext *ctx = nullptr;
#if LIBSWSCALE_VERSION_MAJOR < 9
  int key[6] = {};
#endif

  SwsContext *alloc() {
    ctx = sws_alloc_context();
    if (ctx && flags) av_opt_set_int(ctx, "sws_flags", flags, 0);
    if (ctx && threads) av_opt_set_int(ctx, "threads", threads, 0);
    return ctx;
  }

  int scale(AVFrame *dst, const AVFrame *src) {
#if LIBSWSCALE_VERSION_MAJOR >= 9
    if (!ctx && !alloc()) return AVERROR(ENOMEM);
    return sws_scale_frame(ctx, dst, src);
#else
    // Mesmo formato e tamanho (NV12 da GPU para encoder NV12): cópia direta.
    // O sws_scale antigo não copia o plano UV do NV12 nesse caso.
    if (src->format == dst->format && src->width == dst->width && src->height == dst->height)
      return av_frame_copy(dst, src);
    const int k[6] = {src->width, src->height, src->format, dst->width, dst->height, dst->format};
    if (!ctx || !std::equal(k, k + 6, key)) {
      reset();
      if (!alloc()) return AVERROR(ENOMEM);
      av_opt_set_int(ctx, "srcw", src->width, 0);
      av_opt_set_int(ctx, "srch", src->height, 0);
      av_opt_set_int(ctx, "src_format", src->format, 0);
      av_opt_set_int(ctx, "dstw", dst->width, 0);
      av_opt_set_int(ctx, "dsth", dst->height, 0);
      av_opt_set_int(ctx, "dst_format", dst->format, 0);
      if (const int r = sws_init_context(ctx, nullptr, nullptr); r < 0) {
        reset();
        return r;
      }
      std::copy(k, k + 6, key);
      auto coefs = [](const AVFrame *f) {
        return sws_getCoefficients(f->colorspace == AVCOL_SPC_RGB ? SWS_CS_DEFAULT : f->colorspace);
      };
      sws_setColorspaceDetails(ctx, coefs(src), src->color_range == AVCOL_RANGE_JPEG, coefs(dst),
                               dst->color_range == AVCOL_RANGE_JPEG, 0, 1 << 16, 1 << 16);
    }
    return sws_scale(ctx, src->data, src->linesize, 0, src->height, dst->data, dst->linesize);
#endif
  }

  void reset() {
#if LIBSWSCALE_VERSION_MAJOR >= 9
    sws_free_context(&ctx);
#else
    sws_freeContext(ctx);
    ctx = nullptr;
#endif
  }
};

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Conversão na GPU (gravação pela GPU no Windows)

// Recorte BGRA da captura → NV12 BT.709 limitado num quadro do pool D3D11 do
// encoder, pelo processador de vídeo do D3D11 (sem shaders). Usado só na
// thread da captura, dona do contexto imediato do dispositivo.
struct GpuConv {
  template <class T>
  static void drop(T *&p) {
    if (p) p->Release();
    p = nullptr;
  }

  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  ID3D11VideoDevice *vdev = nullptr;
  ID3D11VideoContext *vctx = nullptr;
  ID3D11VideoContext1 *vctx1 = nullptr;  // espaços de cor DXGI (Windows 10+)
  uint32_t out_w = 0, out_h = 0;
  // Direto: o processador de vídeo lê a textura do Windows (só o recorte),
  // sem cópia intermediária; com a GPU disputada, cada quadro custa menos e
  // o buffer do Windows volta antes ao pool dele. Se o driver não aceitar a
  // textura do Windows como entrada, copia o recorte para `src` antes.
  bool direct = true;
  // Dependem do tamanho da entrada (refeitos se a janela mudar de tamanho).
  uint32_t in_w = 0, in_h = 0;
  DXGI_FORMAT in_fmt = DXGI_FORMAT_UNKNOWN;
  ID3D11Texture2D *src = nullptr;  // cópia do recorte (sem `direct`)
  ID3D11VideoProcessorEnumerator *vpe = nullptr;
  ID3D11VideoProcessor *vp = nullptr;
  // Views por textura (o pool do Windows e o do encoder reaproveitam as
  // texturas; cada view segura a sua).
  std::vector<std::pair<ID3D11Texture2D *, ID3D11VideoProcessorInputView *>> in_views;
  std::vector<std::pair<ID3D11Texture2D *, ID3D11VideoProcessorOutputView *>> out_views;

  ~GpuConv() {
    reset_size();
    drop(vctx1);
    drop(vctx);
    drop(vdev);
    drop(ctx);
    drop(dev);
  }

  void reset_size() {
    for (auto &v : out_views) drop(v.second);
    out_views.clear();
    for (auto &v : in_views) drop(v.second);
    in_views.clear();
    drop(vp);
    drop(vpe);
    drop(src);
    in_w = in_h = 0;
  }

  bool init(ID3D11Device *d, uint32_t w, uint32_t h) {
    dev = d;
    dev->AddRef();
    dev->GetImmediateContext(&ctx);
    out_w = w;
    out_h = h;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11VideoDevice), reinterpret_cast<void **>(&vdev))) ||
        FAILED(ctx->QueryInterface(__uuidof(ID3D11VideoContext), reinterpret_cast<void **>(&vctx))))
      return false;
    ctx->QueryInterface(__uuidof(ID3D11VideoContext1), reinterpret_cast<void **>(&vctx1));
    return true;
  }

  // Entrada de w×h (a textura do Windows inteira, ou o recorte copiado).
  bool setup(uint32_t w, uint32_t h, DXGI_FORMAT fmt) {
    reset_size();
    if (!direct) {
      D3D11_TEXTURE2D_DESC td{};
      td.Width = w;
      td.Height = h;
      td.MipLevels = 1;
      td.ArraySize = 1;
      td.Format = fmt;
      td.SampleDesc.Count = 1;
      td.Usage = D3D11_USAGE_DEFAULT;
      td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
      if (FAILED(dev->CreateTexture2D(&td, nullptr, &src))) return false;
    }
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC cd{};
    cd.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    cd.InputFrameRate = {60, 1};
    cd.InputWidth = w;
    cd.InputHeight = h;
    cd.OutputFrameRate = {60, 1};
    cd.OutputWidth = out_w;
    cd.OutputHeight = out_h;
    cd.Usage = D3D11_VIDEO_USAGE_OPTIMAL_SPEED;
    if (FAILED(vdev->CreateVideoProcessorEnumerator(&cd, &vpe)) || FAILED(vdev->CreateVideoProcessor(vpe, 0, &vp)))
      return false;
    // RGB do jogo (faixa cheia) → YUV BT.709 de faixa limitada, o que os
    // players esperam (o mesmo da conversão no processador).
    if (vctx1) {
      vctx1->VideoProcessorSetStreamColorSpace1(vp, 0, DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
      vctx1->VideoProcessorSetOutputColorSpace1(vp, DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709);
    } else {
      D3D11_VIDEO_PROCESSOR_COLOR_SPACE in{}, out{};
      in.RGB_Range = 0;  // 0–255
      out.YCbCr_Matrix = 1;  // BT.709
      out.Nominal_Range = D3D11_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235;
      vctx->VideoProcessorSetStreamColorSpace(vp, 0, &in);
      vctx->VideoProcessorSetOutputColorSpace(vp, &out);
    }
    vctx->VideoProcessorSetStreamFrameFormat(vp, 0, D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    vctx->VideoProcessorSetStreamAutoProcessingMode(vp, 0, FALSE);
    const RECT out_rect{0, 0, LONG(out_w), LONG(out_h)};
    vctx->VideoProcessorSetStreamDestRect(vp, 0, TRUE, &out_rect);
    vctx->VideoProcessorSetOutputTargetRect(vp, TRUE, &out_rect);
    in_w = w;
    in_h = h;
    in_fmt = fmt;
    return true;
  }

  ID3D11VideoProcessorInputView *input_view(ID3D11Texture2D *t) {
    for (auto &v : in_views)
      if (v.first == t) return v.second;
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv{};
    iv.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    ID3D11VideoProcessorInputView *view = nullptr;
    if (FAILED(vdev->CreateVideoProcessorInputView(t, vpe, &iv, &view))) return nullptr;
    in_views.emplace_back(t, view);
    return view;
  }

  // Converte o recorte (x, y, w, h) de `tex` no quadro `out` (do pool D3D11).
  bool blit(ID3D11Texture2D *tex, uint32_t x, uint32_t y, uint32_t w, uint32_t h, AVFrame *out) {
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    const uint32_t need_w = direct ? d.Width : w, need_h = direct ? d.Height : h;
    if (need_w != in_w || need_h != in_h || d.Format != in_fmt || !vp) {
      if (!setup(need_w, need_h, d.Format)) {
        reset_size();
        return false;
      }
    }
    ID3D11VideoProcessorInputView *in = nullptr;
    if (direct && !(in = input_view(tex))) {
      FRAMUX_LOG("gravação pela GPU: o driver não lê a textura da captura direto; copiando o recorte antes");
      direct = false;
      return blit(tex, x, y, w, h, out);
    }
    auto *dst = reinterpret_cast<ID3D11Texture2D *>(out->data[0]);
    ID3D11VideoProcessorOutputView *view = nullptr;
    for (auto &v : out_views)
      if (v.first == dst) view = v.second;
    if (!view) {
      D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov{};
      ov.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
      if (FAILED(vdev->CreateVideoProcessorOutputView(dst, vpe, &ov, &view))) return false;
      out_views.emplace_back(dst, view);
    }
    RECT src_rect{LONG(x), LONG(y), LONG(x + w), LONG(y + h)};
    if (!direct) {
      const D3D11_BOX box{x, y, 0, x + w, y + h, 1};
      ctx->CopySubresourceRegion(src, 0, 0, 0, 0, tex, 0, &box);
      if (!(in = input_view(src))) return false;
      src_rect = RECT{0, 0, LONG(w), LONG(h)};
    }
    vctx->VideoProcessorSetStreamSourceRect(vp, 0, TRUE, &src_rect);
    D3D11_VIDEO_PROCESSOR_STREAM st{};
    st.Enable = TRUE;
    st.pInputSurface = in;
    if (FAILED(vctx->VideoProcessorBlt(vp, view, 0, 1, &st))) return false;
    ctx->Flush();
    return true;
  }
};

// Quadros da GPU esperando a conversão (que só lhes dá a posição): acima
// disso, o encoder está atrasado e a captura descarta em vez de acumular.
// O pool D3D11 é dinâmico (um array de NV12 como destino de renderização não
// existe em todo driver); o número de texturas fica limitado por esta fila,
// pela do encoder (kMaxQueuedFrames) e pelas superfícies do encoder.
constexpr size_t kGpuMaxPending = 8;
#endif

// ---------------------------------------------------------------------------
// Recorder

// Duas threads: "framux-rec" converte cada quadro (RGB do jogo → formato do
// encoder) e devolve o slot à camada na hora; "framux-enc" abre o encoder,
// mantém a taxa constante, codifica vídeo e áudio e escreve o arquivo. Entre
// elas, até kMaxQueuedFrames quadros convertidos.
struct Recorder::Impl {
  struct VideoItem {
    const uint8_t *data;
    uint32_t width, height, stride, vk_format;
    bool flip_y, halved;
    uint64_t present_ns;
    std::function<void()> release;
    AVFrame *gpu = nullptr;  // quadro D3D11 NV12 já pronto (gravação pela GPU)
  };
  struct AudioItem {
    std::vector<float> samples;
    uint64_t time_ns;
    AudioSource source;
  };
  struct FrameItem {  // convertido, com a posição na linha do tempo
    AVFrame *frame;
    int64_t index;
    uint64_t present_ns;
  };

  RecorderOptions opt;
  int fd;
  std::vector<const EncDef *> order;  // encoders a tentar
  std::thread conv_thread, enc_thread;
  std::mutex mu;
  std::condition_variable conv_cv;   // input ou stopping
  std::condition_variable enc_cv;    // encq ou conv_done
  std::condition_variable space_cv;  // vaga na encq (ou falha)
  std::deque<VideoItem> input;
  std::deque<std::variant<FrameItem, AudioItem>> encq;
  size_t queued_frames = 0;
  bool stopping = false;
  bool conv_done = false;
  std::atomic<bool> failed{false};
  std::string error;  // mu
  std::atomic<uint64_t> layer_drops{0};

  // Diagnóstico: tempo de cada etapa (soma, pior, vezes), registrado no fim.
  struct Timing {
    uint64_t sum = 0, max = 0, n = 0;
    void add(uint64_t ns) {
      sum += ns;
      max = std::max(max, ns);
      ++n;
    }
    double avg_ms() const { return n ? double(sum) / double(n) / 1e6 : 0.0; }
    double max_ms() const { return double(max) / 1e6; }
  };
  Timing t_sws, t_space, t_enc;  // conversão, espera por vaga na fila do encoder, avcodec_send_frame
  RecorderResult result;  // dropped é da conversão; o resto, do encoder

  // Só a thread de conversão.
  Scaler sws;
  AVFrame *src = nullptr;  // embrulha o slot, sem cópia
  AVPixelFormat planned = AV_PIX_FMT_NONE;
  bool planned_cpu = false;  // planned é de um encoder no processador
  const EncDef *planned_def = nullptr;
  int out_w = 0, out_h = 0;
  bool conv_started = false;
  bool gpu_yuv = false;  // a camada mandou NV12 (lido pelo encoder depois do 1º quadro)
  uint64_t conv_start_ns = 0;
  int64_t conv_last_index = -1;

  // Só a thread do encoder.
  AVFormatContext *fmt = nullptr;
  uint8_t *avio_buf = nullptr;
  OpenEncoder venc;
  AVStream *vst = nullptr;
  Scaler resws;                 // encoder de reserva com outro formato
  AVFrame *last = nullptr;      // último quadro enviado, para repetir em buracos
  AVPacket *pkt = nullptr;
  bool started = false;
  uint64_t start_ns = 0;
  int64_t last_index = -1;
  // Áudio: cada fonte fica alinhada na linha do tempo (amostras desde o
  // primeiro quadro); o trecho que todas já cobrem é mixado nas trilhas.
  struct AudioInput {
    bool on = false;
    AVAudioFifo *fifo = nullptr;  // amostras de `mixed` até `written`
    int64_t written = 0;
  };
  struct AudioTrack {
    unsigned sources = 0;  // bits de AudioSource
    AVCodecContext *enc = nullptr;
    AVStream *st = nullptr;
    AVAudioFifo *fifo = nullptr;  // à espera de completar um pacote
    int64_t pts = 0;
  };
  AudioInput inputs[2];
  std::vector<AudioTrack> tracks;
  int64_t mixed = 0;
  std::deque<AudioItem> early_audio;  // antes do primeiro quadro

  // Qualquer thread.
  std::atomic<float> volume[2] = {1.0f, 1.0f};

#ifdef _WIN32
  // Gravação pela GPU: dispositivo e pool D3D11 (criados em gpu_open, antes
  // do primeiro quadro; o encoder os usa) e o conversor (só a captura).
  AVBufferRef *gpu_dev = nullptr;
  AVBufferRef *gpu_frames = nullptr;
  std::unique_ptr<GpuConv> gconv;
  ~Impl() {
    gconv.reset();
    av_buffer_unref(&gpu_frames);
    av_buffer_unref(&gpu_dev);
  }
#endif

  void fail(const std::string &e) {
    {
      std::lock_guard lk(mu);
      if (error.empty()) error = e;
      failed = true;
    }
    space_cv.notify_all();
    FRAMUX_LOG("gravação: %s", e.c_str());
  }

  // Encoder pedido primeiro; se falhar (ou "auto"), os detectados na ordem.
  // A conversão já sai no formato do primeiro que existir.
  void plan() {
    const std::string want = opt.quality == "lossless" ? "utvideo" : opt.encoder;
    auto add_key = [&](const std::string &key) {
      for (const EncDef &d : kDefs)
        if (key == d.key) order.push_back(&d);
    };
    if (want != "auto") add_key(want);
    if (opt.available.empty()) {
      for (const EncDef &d : kDefs) order.push_back(&d);
    } else {
      for (const auto &k : opt.available) add_key(k);
    }
    for (const EncDef *d : order) {
      const AVCodec *codec = avcodec_find_encoder_by_name(d->ffname);
      if (codec && (planned = sw_format_for(*d, codec)) != AV_PIX_FMT_NONE) {
        planned_cpu = on_cpu(d->kind);
        planned_def = d;
        break;
      }
    }
    if (planned == AV_PIX_FMT_NONE) planned = AV_PIX_FMT_YUV420P;
  }

  static void set_color(AVFrame *f, bool rgb) {
    f->colorspace = rgb ? AVCOL_SPC_RGB : AVCOL_SPC_BT709;
    f->color_range = rgb ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
  }

  static bool is_rgb(AVPixelFormat f) { return f == AV_PIX_FMT_BGR0 || f == AV_PIX_FMT_BGRA || f == AV_PIX_FMT_GBRP; }

  // ------------------------------------------------------------------ conversão

  void convert(VideoItem &v) {
#ifdef _WIN32
    if (v.gpu) {
      convert_gpu(v);
      return;
    }
#endif
    const AVPixelFormat sf = src_format(v.vk_format);
    if (sf == AV_PIX_FMT_NONE) {
      v.release();
      FRAMUX_LOG_ONCE("gravação: formato de swapchain %u não suportado", v.vk_format);
      return;
    }
    if (!conv_started) {
      int w = int(v.width), h = int(v.height);
      if (opt.half && !v.halved) {
        w /= 2;
        h /= 2;
      }
      w &= ~1;  // 4:2:0 exige dimensões pares
      h &= ~1;
      if (w < 16 || h < 16) {
        v.release();
        fail("janela pequena demais para gravar");
        return;
      }
      out_w = w;
      out_h = h;
      gpu_yuv = sf == AV_PIX_FMT_NV12;
      conv_start_ns = v.present_ns;
      conv_started = true;
      // RGB→YUV de 2560×1080 leva ~7 ms num núcleo; em fatias, ~2 ms.
#ifdef _WIN32
      const long cpus = long(std::thread::hardware_concurrency());
#else
      const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
#endif
      sws.flags = SWS_BILINEAR;
      sws.threads = int(std::clamp<long>(cpus / 2, 1, 4));
      src = av_frame_alloc();
    }
    const double pos = double(v.present_ns - std::min(v.present_ns, conv_start_ns)) * opt.fps / 1e9;
    int64_t index = std::llround(pos);
    // Quadros com horário irregular (captura de fora do jogo com a GPU
    // disputada) caem às vezes no slot do anterior: em vez de descartar um e
    // repetir o seguinte, vai para o próximo slot (no máximo 1,5 quadro de
    // atraso; com o jogo perto da taxa do vídeo, quase todo quadro cai perto
    // da fronteira de um slot).
    if (index == conv_last_index) index = conv_last_index + 1;
    if (v.present_ns < conv_start_ns || index <= conv_last_index) {
      ++result.dropped;
      v.release();
      return;
    }

    AVFrame *out = av_frame_alloc();
    out->format = planned;
    out->width = out_w;
    out->height = out_h;
    // RGB do jogo → YUV BT.709 de faixa limitada (o que os players esperam).
    set_color(out, is_rgb(planned));
    if (av_frame_get_buffer(out, 0) < 0) {
      av_frame_free(&out);
      v.release();
      fail("sem memória para o quadro");
      return;
    }
    src->format = sf;
    src->width = int(v.width);
    src->height = int(v.height);
    set_color(src, sf != AV_PIX_FMT_NV12);
    src->data[0] = const_cast<uint8_t *>(v.data);
    src->linesize[0] = int(v.stride);
    src->data[1] = nullptr;
    src->linesize[1] = 0;
    if (sf == AV_PIX_FMT_NV12) {  // plano UV logo depois do Y
      src->data[1] = src->data[0] + size_t(v.stride) * v.height;
      src->linesize[1] = int(v.stride);
    }
    if (v.flip_y) {  // glReadPixels: de baixo para cima
      src->data[0] += size_t(v.height - 1) * v.stride;
      src->linesize[0] = -int(v.stride);
    }
    const uint64_t t_sws0 = now_ns();
    const int r = sws.scale(out, src);
    t_sws.add(now_ns() - t_sws0);
    v.release();  // o slot volta para a camada o quanto antes
    if (r < 0) {
      av_frame_free(&out);
      fail("conversão de cor falhou: " + av_err(r));
      return;
    }
    conv_last_index = index;
    {
      std::unique_lock lk(mu);
      const uint64_t t_space0 = now_ns();
      space_cv.wait(lk, [&] { return queued_frames < kMaxQueuedFrames || failed; });
      t_space.add(now_ns() - t_space0);
      if (failed) {
        av_frame_free(&out);
        return;
      }
      encq.push_back(FrameItem{out, index, v.present_ns});
      ++queued_frames;
    }
    enc_cv.notify_one();
  }

#ifdef _WIN32
  // Quadro da GPU: já está no formato e no tamanho do encoder; aqui só ganha
  // a posição na linha do tempo (a mesma regra dos quadros da memória).
  void convert_gpu(VideoItem &v) {
    AVFrame *f = v.gpu;
    v.gpu = nullptr;
    if (!conv_started) {
      out_w = f->width;
      out_h = f->height;
      gpu_yuv = true;
      conv_start_ns = v.present_ns;
      conv_started = true;
    }
    const double pos = double(v.present_ns - std::min(v.present_ns, conv_start_ns)) * opt.fps / 1e9;
    int64_t index = std::llround(pos);
    if (index == conv_last_index) index = conv_last_index + 1;
    if (v.present_ns < conv_start_ns || index <= conv_last_index) {
      ++result.dropped;
      av_frame_free(&f);
      return;
    }
    conv_last_index = index;
    {
      std::unique_lock lk(mu);
      const uint64_t t_space0 = now_ns();
      space_cv.wait(lk, [&] { return queued_frames < kMaxQueuedFrames || failed; });
      t_space.add(now_ns() - t_space0);
      if (failed) {
        av_frame_free(&f);
        return;
      }
      encq.push_back(FrameItem{f, index, v.present_ns});
      ++queued_frames;
    }
    enc_cv.notify_one();
  }
#endif

  void conv_main() {
    pthread_setname_np(pthread_self(), "framux-rec");
    if (planned_cpu) yield_to_game();
    for (;;) {
      VideoItem v;
      {
        std::unique_lock lk(mu);
        conv_cv.wait(lk, [&] { return stopping || !input.empty(); });
        if (input.empty()) break;
        v = std::move(input.front());
        input.pop_front();
      }
      if (failed) {
        v.release();
        av_frame_free(&v.gpu);
      } else {
        convert(v);
      }
    }
    sws.reset();
    av_frame_free(&src);
    {
      std::lock_guard lk(mu);
      conv_done = true;
    }
    enc_cv.notify_one();
  }

  // ------------------------------------------------------------------- encoder

  bool write_packets(AVCodecContext *enc, AVStream *st) {
    for (;;) {
      int r = avcodec_receive_packet(enc, pkt);
      if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) return true;
      if (r < 0) {
        fail("encoder falhou: " + av_err(r));
        return false;
      }
      av_packet_rescale_ts(pkt, enc->time_base, st->time_base);
      pkt->stream_index = st->index;
      if ((r = av_interleaved_write_frame(fmt, pkt)) < 0) {
        fail("falha ao escrever o vídeo: " + av_err(r));
        return false;
      }
    }
  }

  bool encode_video(AVFrame *f) {
    const uint64_t t_enc0 = now_ns();
    int r = avcodec_send_frame(venc.ctx, f);
    if (f) t_enc.add(now_ns() - t_enc0);
    if (r < 0 && r != AVERROR_EOF) {
      fail("encoder recusou o quadro: " + av_err(r));
      return false;
    }
    return write_packets(venc.ctx, vst);
  }

  bool open_audio_track(unsigned sources, const char *title) {
    const AVCodec *codec = avcodec_find_encoder_by_name("libopus");
    if (!codec) {
      FRAMUX_LOG("gravação sem áudio: libopus ausente no FFmpeg");
      return false;
    }
    AudioTrack t;
    t.sources = sources;
    t.enc = avcodec_alloc_context3(codec);
    t.enc->sample_rate = kAudioRate;
    t.enc->sample_fmt = AV_SAMPLE_FMT_FLT;
    av_channel_layout_default(&t.enc->ch_layout, 2);
    t.enc->bit_rate = 160000;
    t.enc->time_base = {1, kAudioRate};
    if (fmt->oformat->flags & AVFMT_GLOBALHEADER) t.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int r = avcodec_open2(t.enc, codec, nullptr);
    if (r < 0) {
      FRAMUX_LOG("gravação sem áudio: libopus: %s", av_err(r).c_str());
      avcodec_free_context(&t.enc);
      return false;
    }
    t.st = avformat_new_stream(fmt, nullptr);
    avcodec_parameters_from_context(t.st->codecpar, t.enc);
    t.st->time_base = t.enc->time_base;
    // A primeira trilha é a que os players tocam.
    t.st->disposition = tracks.empty() ? AV_DISPOSITION_DEFAULT : 0;
    av_dict_set(&t.st->metadata, "title", title, 0);
    av_dict_set(&t.st->metadata, "handler_name", title, 0);
    t.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, 2, kAudioRate);
    tracks.push_back(t);
    return true;
  }

  // Jogo ou microfone: uma trilha. Os dois: a mixada primeiro e, se pedido,
  // uma de cada para edição.
  void open_audio() {
    constexpr unsigned kGame = 1u << int(AudioSource::Game), kMic = 1u << int(AudioSource::Mic);
    if (opt.game_audio && opt.mic_audio) {
      if (open_audio_track(kGame | kMic, "Mix") && opt.separate_tracks) {
        open_audio_track(kGame, "Game");
        open_audio_track(kMic, "Microphone");
      }
    } else if (opt.game_audio) {
      open_audio_track(kGame, "Game");
    } else if (opt.mic_audio) {
      open_audio_track(kMic, "Microphone");
    }
    if (tracks.empty()) return;
    inputs[int(AudioSource::Game)].on = opt.game_audio;
    inputs[int(AudioSource::Mic)].on = opt.mic_audio;
    for (AudioInput &in : inputs)
      if (in.on) in.fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, 2, kAudioRate);
  }

  // Primeiro quadro: abre encoder e arquivo no tamanho dele.
  bool start(const FrameItem &f) {
    const int w = f.frame->width, h = f.frame->height;
    if (avformat_alloc_output_context2(&fmt, nullptr, "matroska", nullptr) < 0) {
      fail("muxer Matroska indisponível");
      return false;
    }
    const bool global = fmt->oformat->flags & AVFMT_GLOBALHEADER;
    std::string why, all_why;
    // Na gravação pela GPU o encoder já foi aberto em gpu_open.
    for (const EncDef *d : order) {
      if (venc.ctx) break;
      if (on_cpu(d->kind)) yield_to_game();
      if (open_encoder(*d, w, h, opt.fps, opt.quality, global, venc, &why)) break;
      FRAMUX_LOG("gravação: %s", why.c_str());
      all_why = why;
    }
    if (!venc.ctx) {
      fail("nenhum encoder de vídeo funcionou (" + all_why + ")");
      return false;
    }
    const std::string want = opt.quality == "lossless" ? "utvideo" : opt.encoder;
    if (want != "auto" && want != venc.def->key)
      FRAMUX_LOG("gravação: encoder \"%s\" indisponível; usando %s", want.c_str(), venc.def->ffname);
    result.encoder = venc.def->ffname;

    vst = avformat_new_stream(fmt, nullptr);
    avcodec_parameters_from_context(vst->codecpar, venc.ctx);
    vst->time_base = venc.ctx->time_base;
    vst->avg_frame_rate = venc.ctx->framerate;
    open_audio();

    av_dict_set(&fmt->metadata, "title", opt.game.c_str(), 0);
    av_dict_set(&fmt->metadata, "encoder", "framux " FRAMUX_VERSION, 0);
    av_dict_set(&fmt->metadata, "creation_time", "now", 0);
    constexpr int kAvioBuf = 1 << 16;
    avio_buf = static_cast<uint8_t *>(av_malloc(kAvioBuf));
    fmt->pb = avio_alloc_context(avio_buf, kAvioBuf, 1, &fd, nullptr, fd_write, fd_seek);
    int r = avformat_write_header(fmt, nullptr);
    if (r < 0) {
      fail("falha ao iniciar o arquivo: " + av_err(r));
      return false;
    }
    pkt = av_packet_alloc();
    start_ns = f.present_ns;
    started = true;
    char cap[48] = "";
    if (venc.ctx->rc_max_rate)
      std::snprintf(cap, sizeof cap, ", teto de %.1f Mbit/s", double(venc.ctx->rc_max_rate) / 1e6);
    FRAMUX_LOG("gravando %s: %dx%d a %d fps com %s%s, cor convertida %s%s", opt.path.c_str(), w, h, opt.fps,
               venc.def->ffname, cap, gpu_yuv ? "na GPU" : "no daemon", audio_desc().c_str());
    while (!early_audio.empty()) {
      process_audio(early_audio.front());
      early_audio.pop_front();
    }
    return true;
  }

  void encode_frame(FrameItem &f) {
    if (!started && !start(f)) {
      av_frame_free(&f.frame);
      return;
    }
    AVFrame *frame = f.frame;
    if (frame->format != venc.sw_format) {
      // O encoder planejado não abriu e o de reserva quer outro formato.
      AVFrame *conv = av_frame_alloc();
      conv->format = venc.sw_format;
      conv->width = frame->width;
      conv->height = frame->height;
      set_color(conv, is_rgb(venc.sw_format));
      int r = av_frame_get_buffer(conv, 0);
      if (r >= 0) {
        r = resws.scale(conv, frame);
      }
      av_frame_free(&frame);
      if (r < 0) {
        av_frame_free(&conv);
        fail("conversão de cor falhou: " + av_err(r));
        return;
      }
      frame = conv;
    }
    if (venc.hw_frames) {
      AVFrame *hw = av_frame_alloc();
      int r = av_hwframe_get_buffer(venc.hw_frames, hw, 0);
      if (r >= 0) r = av_hwframe_transfer_data(hw, frame, 0);
      av_frame_free(&frame);
      if (r < 0) {
        av_frame_free(&hw);
        fail("envio do quadro para a GPU falhou: " + av_err(r));
        return;
      }
      frame = hw;
    }

    // Taxa constante: buracos (jogo abaixo da taxa do vídeo, quadro
    // descartado) repetem o anterior, até 2 s; depois disso só pulam.
    if (last) {
      for (int64_t i = last_index + 1; i < f.index && i - last_index <= int64_t(opt.fps) * 2; ++i) {
        AVFrame *dup = av_frame_clone(last);
        dup->pts = i;
        const bool ok = encode_video(dup);
        av_frame_free(&dup);
        if (!ok) break;
        ++result.duplicated;
      }
      av_frame_free(&last);
    }
    frame->pts = f.index;
    encode_video(frame);
    ++result.frames;
    last_index = f.index;
    last = frame;
  }

  std::string audio_desc() const {
    if (tracks.empty()) return "";
    std::string d = " + Opus (";
    for (size_t i = 0; i < tracks.size(); ++i) {
      if (i) d += ", ";
      d += av_dict_get(tracks[i].st->metadata, "title", nullptr, 0)->value;
    }
    return d + ")";
  }

  bool encode_track(AudioTrack &t, bool flush) {
    const int fs = t.enc->frame_size > 0 ? t.enc->frame_size : 960;
    while (av_audio_fifo_size(t.fifo) >= fs || (flush && av_audio_fifo_size(t.fifo) > 0)) {
      AVFrame *f = av_frame_alloc();
      f->nb_samples = fs;
      f->format = AV_SAMPLE_FMT_FLT;
      av_channel_layout_copy(&f->ch_layout, &t.enc->ch_layout);
      f->sample_rate = kAudioRate;
      av_frame_get_buffer(f, 0);
      const int n = av_audio_fifo_read(t.fifo, reinterpret_cast<void **>(f->data), fs);
      if (n < fs)  // fim da gravação: completa o último pacote com silêncio
        std::memset(f->data[0] + size_t(n) * 2 * sizeof(float), 0, size_t(fs - n) * 2 * sizeof(float));
      f->pts = t.pts;
      t.pts += fs;
      const int r = avcodec_send_frame(t.enc, f);
      av_frame_free(&f);
      if (r < 0 || !write_packets(t.enc, t.st)) return false;
    }
    return true;
  }

  void write_input(AudioInput &in, float *samples, int64_t frames) {
    void *planes[1] = {samples};
    av_audio_fifo_write(in.fifo, planes, int(frames));
    in.written += frames;
  }

  void pad_input(AudioInput &in, int64_t frames) {
    std::vector<float> zeros(size_t(frames) * 2, 0.0f);
    write_input(in, zeros.data(), frames);
  }

  // Mixa o trecho que todas as fontes já cobrem e codifica nas trilhas.
  void mix_audio(bool flush) {
    int64_t upto = INT64_MAX;
    for (const AudioInput &in : inputs)
      if (in.on) upto = std::min(upto, in.written);
    if (upto != INT64_MAX && upto > mixed) {
      const int64_t n = upto - mixed;
      std::vector<float> src[2], out(size_t(n) * 2);
      for (int i = 0; i < 2; ++i) {
        if (!inputs[i].on) continue;
        src[i].resize(out.size());
        void *planes[1] = {src[i].data()};
        av_audio_fifo_read(inputs[i].fifo, planes, int(n));
      }
      for (AudioTrack &t : tracks) {
        std::fill(out.begin(), out.end(), 0.0f);
        for (int i = 0; i < 2; ++i)
          if (t.sources & (1u << i))
            for (size_t k = 0; k < out.size(); ++k) out[k] += src[i][k];
        if (t.sources & (t.sources - 1))  // mais de uma fonte: sem estourar
          for (float &v : out) v = std::clamp(v, -1.0f, 1.0f);
        void *planes[1] = {out.data()};
        av_audio_fifo_write(t.fifo, planes, int(n));
      }
      mixed = upto;
    }
    for (AudioTrack &t : tracks)
      if (!encode_track(t, flush)) return;
  }

  void process_audio(AudioItem &a) {
    if (!started) {
      // Áudio antes do primeiro quadro: guarda até 2 s de cada fonte.
      early_audio.push_back(std::move(a));
      while (early_audio.size() > 400) early_audio.pop_front();
      return;
    }
    AudioInput &in = inputs[int(a.source)];
    if (!in.on) return;
    const int64_t frames = int64_t(a.samples.size() / 2);
    const int64_t at = std::llround((double(a.time_ns) - double(start_ns)) * kAudioRate / 1e9);
    int64_t skip = 0;
    if (at > in.written + kAudioRate / 50) {
      // Buraco (fonte calada e o grafo parou de nos chamar): silêncio, até 10 s.
      pad_input(in, std::min<int64_t>(at - in.written, int64_t(kAudioRate) * 10));
    } else if (at < in.written - kAudioRate / 10) {
      // Sobreposição grande (antes do início ou relógio voltou): corta.
      skip = std::min(frames, in.written - at);
    }
    if (skip < frames) write_input(in, a.samples.data() + skip * 2, frames - skip);
    // Fonte atrasada (jogo calado, microfone desconectado): silêncio até
    // meio segundo atrás da mais adiantada, para a mixagem não esperar.
    constexpr int64_t kMaxLag = kAudioRate / 2;
    int64_t ahead = 0;
    for (const AudioInput &i : inputs)
      if (i.on) ahead = std::max(ahead, i.written);
    for (AudioInput &i : inputs)
      if (i.on && i.written < ahead - kMaxLag) pad_input(i, ahead - kMaxLag - i.written);
    mix_audio(false);
  }

  void finalize() {
    if (!started) {
      if (!failed) fail("nenhum quadro do jogo chegou");
    } else {
      encode_video(nullptr);
      if (!tracks.empty()) {
        // As fontes terminam juntas: a que parou antes completa com silêncio.
        int64_t end = 0;
        for (const AudioInput &in : inputs)
          if (in.on) end = std::max(end, in.written);
        for (AudioInput &in : inputs)
          if (in.on && in.written < end) pad_input(in, end - in.written);
        mix_audio(true);
        for (AudioTrack &t : tracks) {
          avcodec_send_frame(t.enc, nullptr);
          write_packets(t.enc, t.st);
        }
      }
      const int r = av_write_trailer(fmt);
      if (r < 0) fail("falha ao finalizar o arquivo: " + av_err(r));
      result.seconds = double(last_index + 1) / opt.fps;
    }
    av_frame_free(&last);
    av_packet_free(&pkt);
    resws.reset();
    for (AudioTrack &t : tracks) {
      av_audio_fifo_free(t.fifo);
      avcodec_free_context(&t.enc);
    }
    tracks.clear();
    for (AudioInput &in : inputs) {
      if (in.fifo) av_audio_fifo_free(in.fifo);
      in.fifo = nullptr;
    }
    venc.reset();
    if (fmt) {
      if (fmt->pb) {
        avio_flush(fmt->pb);
        av_freep(&fmt->pb->buffer);
        avio_context_free(&fmt->pb);
      }
      avformat_free_context(fmt);
      fmt = nullptr;
    }
    close(fd);
    fd = -1;
  }

  void enc_main() {
    pthread_setname_np(pthread_self(), "framux-enc");
    for (;;) {
      std::variant<FrameItem, AudioItem> item;
      {
        std::unique_lock lk(mu);
        enc_cv.wait(lk, [&] { return conv_done || !encq.empty(); });
        if (encq.empty()) break;
        item = std::move(encq.front());
        encq.pop_front();
        if (std::holds_alternative<FrameItem>(item)) --queued_frames;
      }
      space_cv.notify_one();
      if (auto *f = std::get_if<FrameItem>(&item)) {
        if (failed)
          av_frame_free(&f->frame);
        else
          encode_frame(*f);
      } else if (!failed) {
        process_audio(std::get<AudioItem>(item));
      }
    }
    finalize();
  }
};

Recorder::Recorder(RecorderOptions opt) : impl_(std::make_unique<Impl>()) {
  impl_->opt = std::move(opt);
  impl_->volume[int(AudioSource::Game)] = impl_->opt.game_volume;
  impl_->volume[int(AudioSource::Mic)] = impl_->opt.mic_volume;
  impl_->fd = impl_->opt.fd;
  impl_->plan();
  impl_->enc_thread = std::thread([this] { impl_->enc_main(); });
  impl_->conv_thread = std::thread([this] { impl_->conv_main(); });
}

Recorder::~Recorder() {
  if (impl_->conv_thread.joinable()) finish();
}

void Recorder::push_video(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride, uint32_t vk_format,
                          bool flip_y, bool halved, uint64_t present_ns, std::function<void()> release) {
  std::lock_guard lk(impl_->mu);
  if (impl_->stopping) {
    release();
    return;
  }
  impl_->input.push_back(
      Impl::VideoItem{data, width, height, stride, vk_format, flip_y, halved, present_ns, std::move(release)});
  impl_->conv_cv.notify_one();
}

#ifdef _WIN32
bool Recorder::gpu_open(void *device, uint32_t width, uint32_t height) {
  Impl &im = *impl_;
  if (const char *g = std::getenv("FRAMUX_GPU_RECORDING"); g && !std::strcmp(g, "0")) return false;
  const EncDef *d = im.planned_def;
  if (!d || (d->kind != Kind::Nvenc && d->kind != Kind::Amf)) return false;
  if (im.opt.quality == "lossless") return false;
  int w = int(width), h = int(height);
  if (im.opt.half) {
    w /= 2;
    h /= 2;
  }
  w &= ~1;
  h &= ~1;
  if (w < 16 || h < 16) return false;
  auto *dev = static_cast<ID3D11Device *>(device);
  // O encoder usa o dispositivo na thread dele; a captura, na dela.
  ID3D10Multithread *mt = nullptr;
  if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D10Multithread), reinterpret_cast<void **>(&mt)))) {
    mt->SetMultithreadProtected(TRUE);
    mt->Release();
  }
  AVBufferRef *dref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  if (!dref) return false;
  auto *hw = static_cast<AVD3D11VADeviceContext *>(reinterpret_cast<AVHWDeviceContext *>(dref->data)->hwctx);
  dev->AddRef();  // o FFmpeg solta ao liberar o contexto
  hw->device = dev;
  int r = av_hwdevice_ctx_init(dref);
  AVBufferRef *fref = r >= 0 ? av_hwframe_ctx_alloc(dref) : nullptr;
  if (fref) {
    auto *fc = reinterpret_cast<AVHWFramesContext *>(fref->data);
    fc->format = AV_PIX_FMT_D3D11;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = w;
    fc->height = h;
    fc->initial_pool_size = 0;  // texturas avulsas, criadas sob demanda e reaproveitadas
    static_cast<AVD3D11VAFramesContext *>(fc->hwctx)->BindFlags = D3D11_BIND_RENDER_TARGET;
    r = av_hwframe_ctx_init(fref);
  }
  auto conv = std::make_unique<GpuConv>();
  if (r < 0 || !fref || !conv->init(dev, uint32_t(w), uint32_t(h))) {
    FRAMUX_LOG("gravação pela GPU indisponível (%s); quadros pela memória",
               r < 0 ? av_err(r).c_str() : "sem processador de vídeo D3D11");
    av_buffer_unref(&fref);
    av_buffer_unref(&dref);
    return false;
  }
  // Abre aqui o encoder que a gravação usaria (o primeiro da ordem que
  // funcionar), já com os quadros da GPU. O FFmpeg traz o NVENC e o AMF mesmo
  // sem a placa deles: se o que funcionar for outro (QSV, processador), a
  // gravação segue pela memória com ele. As threads do Recorder só mexem no
  // encoder depois do primeiro quadro, que ainda não chegou.
  const AVOutputFormat *mkv = av_guess_format("matroska", nullptr, nullptr);
  const bool global = mkv && (mkv->flags & AVFMT_GLOBALHEADER);
  std::string why;
  for (const EncDef *e : im.order) {
    if (!avcodec_find_encoder_by_name(e->ffname)) continue;
    if (e->kind != Kind::Nvenc && e->kind != Kind::Amf) break;
    if (open_encoder(*e, w, h, im.opt.fps, im.opt.quality, global, im.venc, &why, true, fref)) break;
    FRAMUX_LOG("gravação pela GPU: %s", why.c_str());
  }
  if (!im.venc.ctx) {
    av_buffer_unref(&fref);
    av_buffer_unref(&dref);
    return false;
  }
  im.gpu_dev = dref;
  im.gpu_frames = fref;
  im.gconv = std::move(conv);
  return true;
}

void Recorder::gpu_push(void *texture, uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                        uint64_t present_ns) {
  Impl &im = *impl_;
  if (!im.gconv || im.failed) return;
  {
    std::lock_guard lk(im.mu);
    if (im.input.size() >= kGpuMaxPending) {
      ++im.layer_drops;
      return;
    }
  }
  AVFrame *f = av_frame_alloc();
  if (!f) return;
  // Pool cheio (encoder atrasado): descarta, sem esperar.
  if (av_hwframe_get_buffer(im.gpu_frames, f, 0) < 0) {
    av_frame_free(&f);
    ++im.layer_drops;
    return;
  }
  f->colorspace = AVCOL_SPC_BT709;
  f->color_range = AVCOL_RANGE_MPEG;
  f->color_primaries = AVCOL_PRI_BT709;
  f->color_trc = AVCOL_TRC_BT709;
  if (!im.gconv->blit(static_cast<ID3D11Texture2D *>(texture), x, y, width, height, f)) {
    av_frame_free(&f);
    ++im.layer_drops;
    FRAMUX_LOG_ONCE("gravação pela GPU: a conversão do quadro falhou");
    return;
  }
  {
    std::lock_guard lk(im.mu);
    if (!im.stopping) {
      im.input.push_back(Impl::VideoItem{nullptr, width, height, 0, 0, false, true, present_ns, [] {}, f});
      f = nullptr;
    }
  }
  av_frame_free(&f);
  im.conv_cv.notify_one();
}
#endif

void Recorder::push_audio(AudioSource src, const float *samples, uint32_t frames, uint64_t time_ns) {
  // O volume vale a partir de agora, não de quando o bloco for codificado.
  std::vector<float> v(samples, samples + size_t(frames) * 2);
  const float gain = impl_->volume[int(src)];
  if (gain != 1.0f)
    for (float &x : v) x *= gain;
  std::lock_guard lk(impl_->mu);
  if (impl_->stopping) return;
  impl_->encq.push_back(Impl::AudioItem{std::move(v), time_ns, src});
  impl_->enc_cv.notify_one();
}

void Recorder::set_volume(AudioSource src, float volume) { impl_->volume[int(src)] = std::clamp(volume, 0.0f, 1.0f); }

void Recorder::add_layer_drops(uint32_t n) { impl_->layer_drops += n; }

bool Recorder::failed() const { return impl_->failed; }

std::string Recorder::error() const {
  std::lock_guard lk(impl_->mu);
  return impl_->error;
}

RecorderResult Recorder::finish() {
  {
    std::lock_guard lk(impl_->mu);
    impl_->stopping = true;
  }
  impl_->conv_cv.notify_one();
  if (impl_->conv_thread.joinable()) impl_->conv_thread.join();
  if (impl_->enc_thread.joinable()) impl_->enc_thread.join();
  RecorderResult r = impl_->result;
  r.dropped += impl_->layer_drops;
  FRAMUX_LOG("gravação, etapas: conversão %.1f ms (pior %.0f ms); espera por vaga no encoder %.1f ms (pior %.0f ms); "
             "encoder %.1f ms por quadro (pior %.0f ms, %llu quadros)",
             impl_->t_sws.avg_ms(), impl_->t_sws.max_ms(), impl_->t_space.avg_ms(), impl_->t_space.max_ms(),
             impl_->t_enc.avg_ms(), impl_->t_enc.max_ms(), (unsigned long long)impl_->t_enc.n);
  r.path = impl_->opt.path;
  if (impl_->failed && !impl_->started) {
    // Nada gravado: não deixa um arquivo vazio na pasta.
    unlink(impl_->opt.path.c_str());
    r.error = error();
    return r;
  }
  r.ok = true;
  if (impl_->failed) r.error = error();  // gravou até a falha; o arquivo vale
  if (impl_->opt.to_mp4) {
    if (r.encoder == "utvideo") {
      FRAMUX_LOG("gravação: Ut Video não cabe em MP4; mantido em MKV");
    } else {
      std::string err;
      const std::string mp4 = remux_mp4(r.path, &err);
      if (mp4.empty())
        FRAMUX_LOG("gravação: %s; mantido em MKV", err.c_str());
      else
        r.path = mp4;
    }
  }
  r.bytes = file_size(r.path);
  return r;
}

}  // namespace framux::daemon
