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

#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
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

enum class Kind { Nvenc, Vaapi, Svt, X264, Utvideo };

struct EncDef {
  const char *key;     // [video] encoder
  const char *ffname;  // encoder do FFmpeg
  Kind kind;
};

// Ordem = preferência do modo automático: codificador da placa primeiro
// (quase nenhuma CPU), AV1 antes de H.264; sem placa, CPU. HEVC ficou de
// fora: o Fedora sem RPM Fusion não o decodifica. O teto de taxa
// (video_rate.h) deixa o H.264 leve para o openh264 do Fedora tocar.
constexpr EncDef kDefs[] = {
    {"av1_hw", "av1_nvenc", Kind::Nvenc},   {"av1_hw", "av1_vaapi", Kind::Vaapi},
    {"h264_hw", "h264_nvenc", Kind::Nvenc}, {"h264_hw", "h264_vaapi", Kind::Vaapi},
    {"av1_sw", "libsvtav1", Kind::Svt},     {"x264", "libx264", Kind::X264},
    {"utvideo", "utvideo", Kind::Utvideo},
};

bool on_cpu(Kind k) { return k == Kind::Svt || k == Kind::X264 || k == Kind::Utvideo; }

// Codificador no processador disputa os núcleos com o jogo. Com nice 10 a
// thread perde a disputa: a gravação descarta quadros em vez de o jogo travar.
// As threads criadas depois (do SVT-AV1, do Ut Video, do swscale) herdam.
void yield_to_game() { setpriority(PRIO_PROCESS, gettid(), 10); }

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
  const void *list = nullptr;
  int n = 0;
  if (avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &list, &n) < 0 || !list)
    return *want.begin();
  const auto *fmts = static_cast<const AVPixelFormat *>(list);
  for (AVPixelFormat w : want)
    for (int i = 0; i < n; ++i)
      if (fmts[i] == w) return w;
  return AV_PIX_FMT_NONE;
}

// Formato que o encoder recebe da conversão.
AVPixelFormat sw_format_for(const EncDef &def, const AVCodec *codec) {
  switch (def.kind) {
    case Kind::Nvenc: return pick_format(codec, {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P});
    case Kind::Vaapi: return AV_PIX_FMT_NV12;  // superfícies NV12
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
bool open_encoder(const EncDef &def, int w, int h, int fps, const std::string &quality, bool global_header,
                  OpenEncoder &out, std::string *why, bool capped = true) {
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
  int r = 0;
  switch (def.kind) {
    case Kind::Nvenc:
      c->pix_fmt = out.sw_format;
      av_opt_set(c->priv_data, "preset", "p4", 0);
      av_opt_set(c->priv_data, "tune", "hq", 0);
      av_opt_set(c->priv_data, "rc", "vbr", 0);
      av_opt_set_int(c->priv_data, "cq", av1 ? quality_value(quality, 26, 32, 38) : quality_value(quality, 21, 26, 31),
                     0);
      c->bit_rate = 0;
      set_cap();
      break;
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
      c->thread_count = int(std::max<long>(sysconf(_SC_NPROCESSORS_ONLN) / 2, 1));
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
    if (def.kind == Kind::Vaapi && cap) return open_encoder(def, w, h, fps, quality, global_header, out, why, false);
    return false;
  }
  out.ctx = c;
  out.def = &def;
  return true;
}

// AVIO sobre o fd criado com O_EXCL|O_NOFOLLOW (§C.11.5).
int fd_write(void *opaque, const uint8_t *buf, int n) {
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
  if (whence & AVSEEK_SIZE) {
    struct stat st;
    return fstat(fd, &st) == 0 ? int64_t(st.st_size) : AVERROR(errno);
  }
  const off_t r = lseek(fd, off_t(off), whence & ~AVSEEK_FORCE);
  return r < 0 ? AVERROR(errno) : int64_t(r);
}

uint64_t file_size(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? uint64_t(st.st_size) : 0;
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
        "H.264 por placa de vídeo indisponível. AMD/Intel: instale mesa-va-drivers-freeworld do RPM Fusion; "
        "NVIDIA: use o driver proprietário.";
  return p;
}

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
  RecorderResult result;  // dropped é da conversão; o resto, do encoder

  // Só a thread de conversão.
  SwsContext *sws = nullptr;
  AVFrame *src = nullptr;  // embrulha o slot, sem cópia
  AVPixelFormat planned = AV_PIX_FMT_NONE;
  bool planned_cpu = false;  // planned é de um encoder no processador
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
  SwsContext *resws = nullptr;  // encoder de reserva com outro formato
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
      const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
      sws = sws_alloc_context();
      av_opt_set_int(sws, "sws_flags", SWS_BILINEAR, 0);
      av_opt_set_int(sws, "threads", std::clamp<long>(cpus / 2, 1, 4), 0);
      src = av_frame_alloc();
    }
    const int64_t index = std::llround(double(v.present_ns - std::min(v.present_ns, conv_start_ns)) * opt.fps / 1e9);
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
    const int r = sws_scale_frame(sws, out, src);
    v.release();  // o slot volta para a camada o quanto antes
    if (r < 0) {
      av_frame_free(&out);
      fail("conversão de cor falhou: " + av_err(r));
      return;
    }
    conv_last_index = index;
    {
      std::unique_lock lk(mu);
      space_cv.wait(lk, [&] { return queued_frames < kMaxQueuedFrames || failed; });
      if (failed) {
        av_frame_free(&out);
        return;
      }
      encq.push_back(FrameItem{out, index, v.present_ns});
      ++queued_frames;
    }
    enc_cv.notify_one();
  }

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
      if (failed)
        v.release();
      else
        convert(v);
    }
    sws_free_context(&sws);
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
    int r = avcodec_send_frame(venc.ctx, f);
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
    for (const EncDef *d : order) {
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
        if (!resws) resws = sws_alloc_context();
        r = sws_scale_frame(resws, conv, frame);
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
    sws_free_context(&resws);
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
