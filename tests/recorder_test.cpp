// Recorder (§B.7) sem jogo nem PipeWire: quadros e áudio sintéticos com
// tempos conhecidos. Confere taxa constante (buraco vira repetição), trilha
// Opus, duração, o remux para MP4, as cores/orientação da conversão YUV e a
// mixagem do jogo com o microfone (volume, trilhas separadas).
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/samplefmt.h>
}

#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "daemon/recorder.h"

using namespace framux::daemon;

namespace {

int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FALHOU %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

struct Probe {
  std::string video, audio;
  int width = 0, height = 0, channels = 0, rate = 0;
  double duration = 0;
};

Probe probe(const std::string &path) {
  Probe p;
  AVFormatContext *f = nullptr;
  if (avformat_open_input(&f, path.c_str(), nullptr, nullptr) < 0) return p;
  avformat_find_stream_info(f, nullptr);
  for (unsigned i = 0; i < f->nb_streams; ++i) {
    const AVCodecParameters *c = f->streams[i]->codecpar;
    if (c->codec_type == AVMEDIA_TYPE_VIDEO) {
      p.video = avcodec_get_name(c->codec_id);
      p.width = c->width;
      p.height = c->height;
    } else if (c->codec_type == AVMEDIA_TYPE_AUDIO) {
      p.audio = avcodec_get_name(c->codec_id);
      p.channels = c->ch_layout.nb_channels;
      p.rate = c->sample_rate;
    }
  }
  p.duration = f->duration > 0 ? double(f->duration) / AV_TIME_BASE : 0;
  avformat_close_input(&f);
  return p;
}

// 2 s a 30 fps começando em t0, sem os quadros 20–24 (o jogo "travou"), e
// 2 s de seno de 440 Hz em blocos de 10 ms. Quadros BGRA 320×240 (VkFormat 44).
RecorderResult record(const std::string &dir, const std::string &name, const std::string &encoder, bool mp4,
                      bool half) {
  const std::string path = dir + "/" + name + ".mkv";
  RecorderOptions opt;
  opt.fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  opt.path = path;
  opt.game = "teste";
  opt.fps = 30;
  opt.half = half;
  opt.encoder = encoder;
  opt.quality = "high";
  opt.to_mp4 = mp4;
  opt.game_audio = true;
  opt.available = probe_encoders().available;
  Recorder rec(opt);

  constexpr uint32_t W = 320, H = 240;
  constexpr uint64_t t0 = 1'000'000'000'000ull, period = 1'000'000'000ull / 30;
  std::vector<uint8_t> frame(size_t(W) * H * 4);
  std::atomic<int> released = 0;
  for (int i = 0; i < 60; ++i) {
    if (i >= 20 && i < 25) continue;
    for (size_t p = 0; p < frame.size(); p += 4) {
      frame[p] = uint8_t(i * 4);  // B
      frame[p + 1] = 0x80;        // G
      frame[p + 2] = 0x20;        // R
      frame[p + 3] = 0xFF;
    }
    // Jitter de ±2 ms, como presents reais.
    const uint64_t t = t0 + uint64_t(i) * period + (i % 2 ? 2'000'000 : 0);
    rec.push_video(frame.data(), W, H, W * 4, 44, false, false, t, [&released] { ++released; });
    // A camada só reusa o slot depois do release; aqui, espera a conversão.
    while (released < i + 1 - (i >= 25 ? 5 : 0)) usleep(200);
  }
  std::vector<float> chunk(480 * 2);
  for (int c = 0; c < 200; ++c) {
    for (int s = 0; s < 480; ++s) {
      const float v = 0.5f * std::sin(2 * M_PI * 440 * (c * 480 + s) / 48000.0);
      chunk[size_t(s) * 2] = chunk[size_t(s) * 2 + 1] = v;
    }
    rec.push_audio(AudioSource::Game, chunk.data(), 480, t0 + uint64_t(c) * 10'000'000ull);
  }
  return rec.finish();
}

// Trilhas de áudio do arquivo: título, se é a padrão e o RMS do canal
// esquerdo em janelas de meio segundo (decodificadas por inteiro).
struct AudioTrack {
  std::string title;
  bool is_default = false;
  std::vector<double> rms;
};

std::vector<AudioTrack> audio_tracks(const std::string &path) {
  std::vector<AudioTrack> out;
  AVFormatContext *f = nullptr;
  if (avformat_open_input(&f, path.c_str(), nullptr, nullptr) < 0) return out;
  avformat_find_stream_info(f, nullptr);
  std::vector<AVCodecContext *> dec(f->nb_streams, nullptr);
  std::vector<int> track(f->nb_streams, -1);
  std::vector<std::vector<double>> sums;
  std::vector<int64_t> pos;
  for (unsigned i = 0; i < f->nb_streams; ++i) {
    const AVStream *st = f->streams[i];
    if (st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
    const AVCodec *codec = avcodec_find_decoder(st->codecpar->codec_id);
    dec[i] = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(dec[i], st->codecpar);
    avcodec_open2(dec[i], codec, nullptr);
    const AVDictionaryEntry *t = av_dict_get(st->metadata, "title", nullptr, 0);
    track[i] = int(out.size());
    out.push_back({t ? t->value : "", (st->disposition & AV_DISPOSITION_DEFAULT) != 0, {}});
    sums.emplace_back();
    pos.push_back(0);
  }
  AVPacket *pkt = av_packet_alloc();
  AVFrame *fr = av_frame_alloc();
  auto drain = [&](unsigned si) {
    while (avcodec_receive_frame(dec[si], fr) == 0) {
      const int ti = track[si];
      const bool planar = av_sample_fmt_is_planar(AVSampleFormat(fr->format));
      const auto *d = reinterpret_cast<const float *>(fr->data[0]);
      for (int n = 0; n < fr->nb_samples; ++n, ++pos[ti]) {
        const double v = d[planar ? n : n * fr->ch_layout.nb_channels];
        const size_t w = size_t(pos[ti] / 24000);
        if (sums[ti].size() <= w) sums[ti].resize(w + 1, 0);
        sums[ti][w] += v * v;
      }
    }
  };
  while (av_read_frame(f, pkt) >= 0) {
    const unsigned si = unsigned(pkt->stream_index);
    if (dec[si] && avcodec_send_packet(dec[si], pkt) >= 0) drain(si);
    av_packet_unref(pkt);
  }
  for (unsigned si = 0; si < f->nb_streams; ++si)
    if (dec[si]) {
      avcodec_send_packet(dec[si], nullptr);
      drain(si);
      avcodec_free_context(&dec[si]);
    }
  for (size_t t = 0; t < out.size(); ++t)
    for (double sum : sums[t]) out[t].rms.push_back(std::sqrt(sum / 24000));
  av_frame_free(&fr);
  av_packet_free(&pkt);
  avformat_close_input(&f);
  return out;
}

// Jogo (seno de 440 Hz, 2 s) e microfone (1 kHz a meio volume) até 1,5 s,
// depois "desconectado" (para de chegar).
std::vector<AudioTrack> record_mix(const std::string &dir, bool separate) {
  const std::string path = dir + "/mix" + (separate ? "-sep" : "") + ".mkv";
  RecorderOptions opt;
  opt.fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  opt.path = path;
  opt.fps = 30;
  opt.encoder = "utvideo";
  opt.game_audio = true;
  opt.mic_audio = true;
  opt.separate_tracks = separate;
  opt.mic_volume = 0.5f;
  Recorder rec(opt);

  constexpr uint32_t W = 64, H = 64;
  constexpr uint64_t t0 = 1'000'000'000'000ull, period = 1'000'000'000ull / 30;
  std::vector<uint8_t> frame(size_t(W) * H * 4, 0x80);
  std::atomic<int> released = 0;
  for (int i = 0; i < 60; ++i) {
    rec.push_video(frame.data(), W, H, W * 4, 44, false, false, t0 + uint64_t(i) * period, [&released] { ++released; });
    while (released < i + 1) usleep(200);
  }
  std::vector<float> game(480 * 2), mic(480 * 2);
  for (int c = 0; c < 200; ++c) {
    for (int s = 0; s < 480; ++s) {
      const double t = (c * 480 + s) / 48000.0;
      game[size_t(s) * 2] = game[size_t(s) * 2 + 1] = float(0.5 * std::sin(2 * M_PI * 440 * t));
      mic[size_t(s) * 2] = mic[size_t(s) * 2 + 1] = float(0.5 * std::sin(2 * M_PI * 1000 * t));
    }
    const uint64_t when = t0 + uint64_t(c) * 10'000'000ull;
    rec.push_audio(AudioSource::Game, game.data(), 480, when);
    if (c < 150) rec.push_audio(AudioSource::Mic, mic.data(), 480, when);
  }
  const RecorderResult r = rec.finish();
  CHECK(r.ok);
  return audio_tracks(r.path);
}

void check_mix(const std::string &dir) {
  // Seno de amplitude A: RMS A/√2. Jogo 0,354; microfone a meio volume 0,177.
  auto near = [](double v, double want) { return std::fabs(v - want) < 0.04; };
  auto print = [](const AudioTrack &t) {
    std::printf("  %s%s:", t.title.c_str(), t.is_default ? " (padrão)" : "");
    for (double v : t.rms) std::printf(" %.3f", v);
    std::printf("\n");
  };

  std::vector<AudioTrack> t = record_mix(dir, false);
  std::printf("jogo + microfone:\n");
  for (const auto &x : t) print(x);
  CHECK(t.size() == 1);
  if (t.size() == 1) {
    CHECK(t[0].title == "Mix" && t[0].is_default);
    CHECK(t[0].rms.size() >= 4);
    if (t[0].rms.size() >= 4) {
      CHECK(near(t[0].rms[0], std::hypot(0.354, 0.177)));  // os dois juntos
      CHECK(near(t[0].rms[3], 0.354));                     // só o jogo
    }
  }

  t = record_mix(dir, true);
  std::printf("jogo + microfone, trilhas separadas:\n");
  for (const auto &x : t) print(x);
  CHECK(t.size() == 3);
  if (t.size() == 3) {
    CHECK(t[0].title == "Mix" && t[0].is_default);
    CHECK(t[1].title == "Game" && !t[1].is_default);
    CHECK(t[2].title == "Microphone" && !t[2].is_default);
    for (const auto &x : t) CHECK(x.rms.size() >= 4);
    if (t[1].rms.size() >= 4 && t[2].rms.size() >= 4) {
      for (int w = 0; w < 4; ++w) CHECK(near(t[1].rms[w], 0.354));
      for (int w = 0; w < 3; ++w) CHECK(near(t[2].rms[w], 0.177));
      CHECK(t[2].rms[3] < 0.02);  // desconectado: silêncio, sem atrasar o jogo
    }
  }
}

// Y, U e V (ou G, B e R, se planar RGB) do pixel (x, y) do primeiro quadro.
bool first_pixel_yuv(const std::string &path, int x, int y, int out[3]) {
  AVFormatContext *f = nullptr;
  if (avformat_open_input(&f, path.c_str(), nullptr, nullptr) < 0) return false;
  avformat_find_stream_info(f, nullptr);
  const int si = av_find_best_stream(f, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  bool ok = false;
  if (si >= 0) {
    const AVCodec *codec = avcodec_find_decoder(f->streams[si]->codecpar->codec_id);
    AVCodecContext *c = codec ? avcodec_alloc_context3(codec) : nullptr;
    if (c && avcodec_parameters_to_context(c, f->streams[si]->codecpar) >= 0 && avcodec_open2(c, codec, nullptr) >= 0) {
      AVPacket *pkt = av_packet_alloc();
      AVFrame *fr = av_frame_alloc();
      bool eof = false;
      while (!ok) {
        int r = avcodec_receive_frame(c, fr);
        if (r == 0) {
          if (fr->format == AV_PIX_FMT_GBRP) {
            for (int pl = 0; pl < 3; ++pl) out[pl] = fr->data[pl][y * fr->linesize[pl] + x];
            ok = true;
          } else if (fr->format == AV_PIX_FMT_YUV420P || fr->format == AV_PIX_FMT_NV12) {
            out[0] = fr->data[0][y * fr->linesize[0] + x];
            if (fr->format == AV_PIX_FMT_NV12) {
              out[1] = fr->data[1][(y / 2) * fr->linesize[1] + (x / 2) * 2];
              out[2] = fr->data[1][(y / 2) * fr->linesize[1] + (x / 2) * 2 + 1];
            } else {
              out[1] = fr->data[1][(y / 2) * fr->linesize[1] + x / 2];
              out[2] = fr->data[2][(y / 2) * fr->linesize[2] + x / 2];
            }
            ok = true;
          }
          break;
        }
        if (r != AVERROR(EAGAIN) || eof) break;
        if (av_read_frame(f, pkt) < 0) {
          eof = true;
          avcodec_send_packet(c, nullptr);
        } else {
          if (pkt->stream_index == si) avcodec_send_packet(c, pkt);
          av_packet_unref(pkt);
        }
      }
      av_frame_free(&fr);
      av_packet_free(&pkt);
    }
    avcodec_free_context(&c);
  }
  avformat_close_input(&f);
  return ok;
}

// Metade de cima vermelha, de baixo branca, em AV1 (sempre no ffmpeg-free).
// Vermelho puro em BT.709 de faixa limitada: Y 63, U 102, V 240; branco: 235,
// 128, 128. Com flip_y (glReadPixels) as metades trocam de lugar na memória.
void check_colors(const std::string &dir, bool flip) {
  const std::string path = dir + (flip ? "/cor-flip.mkv" : "/cor.mkv");
  RecorderOptions opt;
  opt.fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  opt.path = path;
  opt.fps = 30;
  opt.encoder = "av1_sw";
  opt.quality = "high";
  opt.available = {"av1_sw"};
  Recorder rec(opt);
  constexpr uint32_t W = 320, H = 240;
  std::vector<uint8_t> frame(size_t(W) * H * 4);
  for (uint32_t y = 0; y < H; ++y) {
    const bool red = (y < H / 2) != flip;
    for (uint32_t x = 0; x < W; ++x) {
      uint8_t *p = &frame[(size_t(y) * W + x) * 4];
      p[0] = red ? 0 : 255;  // B
      p[1] = red ? 0 : 255;  // G
      p[2] = 255;            // R
      p[3] = 255;
    }
  }
  std::atomic<int> released = 0;
  for (int i = 0; i < 10; ++i) {
    rec.push_video(frame.data(), W, H, W * 4, 44, flip, false, 1'000'000'000ull + uint64_t(i) * 33'333'333ull,
                   [&released] { ++released; });
    while (released < i + 1) usleep(200);
  }
  const RecorderResult r = rec.finish();
  CHECK(r.ok && r.encoder == "libsvtav1");
  int top[3] = {}, bottom[3] = {};
  CHECK(first_pixel_yuv(r.path, W / 2, H / 4, top));
  CHECK(first_pixel_yuv(r.path, W / 2, H * 3 / 4, bottom));
  std::printf("cores%s: cima %d/%d/%d, baixo %d/%d/%d\n", flip ? " (flip)" : "", top[0], top[1], top[2], bottom[0],
              bottom[1], bottom[2]);
  auto near = [](const int *v, int y, int u, int w) {
    return std::abs(v[0] - y) <= 4 && std::abs(v[1] - u) <= 4 && std::abs(v[2] - w) <= 4;
  };
  CHECK(near(top, 63, 102, 240));
  CHECK(near(bottom, 235, 128, 128));
}

// NV12 já convertido e reduzido na GPU (camada 0.4.2+): com "metade" na
// config, o gravador não reduz de novo e preserva as cores.
void check_nv12(const std::string &dir) {
  const std::string path = dir + "/nv12.mkv";
  RecorderOptions opt;
  opt.fd = open(path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  opt.path = path;
  opt.fps = 30;
  opt.half = true;
  opt.encoder = "av1_sw";
  opt.quality = "high";
  opt.available = {"av1_sw"};
  Recorder rec(opt);
  constexpr uint32_t W = 320, H = 240, S = 324;  // stride com folga, como a camada
  std::vector<uint8_t> frame(size_t(S) * H * 3 / 2);
  for (uint32_t y = 0; y < H; ++y)
    for (uint32_t x = 0; x < S; ++x) frame[size_t(y) * S + x] = y < H / 2 ? 63 : 235;
  for (uint32_t y = 0; y < H / 2; ++y)
    for (uint32_t x = 0; x < S; x += 2) {
      uint8_t *uv = &frame[size_t(S) * H + size_t(y) * S + x];
      uv[0] = y < H / 4 ? 102 : 128;
      uv[1] = y < H / 4 ? 240 : 128;
    }
  std::atomic<int> released = 0;
  for (int i = 0; i < 10; ++i) {
    rec.push_video(frame.data(), W, H, S, 1000156003, false, true, 1'000'000'000ull + uint64_t(i) * 33'333'333ull,
                   [&released] { ++released; });
    while (released < i + 1) usleep(200);
  }
  const RecorderResult r = rec.finish();
  CHECK(r.ok);
  const Probe p = probe(r.path);
  CHECK(p.width == int(W) && p.height == int(H));
  int top[3] = {}, bottom[3] = {};
  CHECK(first_pixel_yuv(r.path, W / 2, H / 4, top));
  CHECK(first_pixel_yuv(r.path, W / 2, H * 3 / 4, bottom));
  std::printf("nv12: %dx%d, cima %d/%d/%d, baixo %d/%d/%d\n", p.width, p.height, top[0], top[1], top[2], bottom[0],
              bottom[1], bottom[2]);
  auto near = [](const int *v, int y, int u, int w) {
    return std::abs(v[0] - y) <= 4 && std::abs(v[1] - u) <= 4 && std::abs(v[2] - w) <= 4;
  };
  CHECK(near(top, 63, 102, 240));
  CHECK(near(bottom, 235, 128, 128));
}

}  // namespace

int main() {
  setenv("SVT_LOG", "1", 0);  // sem o banner do SVT-AV1 no log do teste
  char tmpl[] = "/tmp/framux-rec-XXXXXX";
  const std::string dir = mkdtemp(tmpl);

  // Ut Video (sempre disponível), metade do tamanho, MKV mesmo pedindo MP4.
  RecorderResult r = record(dir, "utvideo", "utvideo", true, true);
  CHECK(r.ok);
  CHECK(r.error.empty());
  CHECK(r.encoder == "utvideo");
  CHECK(r.frames == 55);
  CHECK(r.duplicated == 5);  // o buraco de 5 quadros virou repetição
  CHECK(r.path == dir + "/utvideo.mkv");
  Probe p = probe(r.path);
  std::printf("utvideo: %s %dx%d + %s %d ch %d Hz, %.2f s\n", p.video.c_str(), p.width, p.height, p.audio.c_str(),
              p.channels, p.rate, p.duration);
  CHECK(p.video == "utvideo");
  CHECK(p.width == 160 && p.height == 120);
  CHECK(p.audio == "opus" && p.channels == 2 && p.rate == 48000);
  CHECK(std::fabs(p.duration - 2.0) < 0.1);
  int gbr[3] = {};
  CHECK(first_pixel_yuv(r.path, 80, 60, gbr));
  CHECK(gbr[0] == 0x80 && gbr[1] == 0 && gbr[2] == 0x20);  // sem perda: o pixel exato do quadro 0

  // Encoder automático com remux para MP4 (o MKV some).
  r = record(dir, "auto", "auto", true, false);
  CHECK(r.ok);
  std::printf("auto: %s → %s\n", r.encoder.c_str(), r.path.c_str());
  if (r.encoder != "utvideo") {
    CHECK(r.path == dir + "/auto.mp4");
    CHECK(access((dir + "/auto.mkv").c_str(), F_OK) != 0);
  }
  p = probe(r.path);
  CHECK(!p.video.empty() && p.width == 320 && p.height == 240);
  CHECK(p.audio == "opus");
  CHECK(std::fabs(p.duration - 2.0) < 0.15);

  check_colors(dir, false);
  check_colors(dir, true);
  check_nv12(dir);
  check_mix(dir);

  // Sem nenhum quadro: erro e nenhum arquivo vazio na pasta.
  {
    RecorderOptions opt;
    opt.path = dir + "/vazio.mkv";
    opt.fd = open(opt.path.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    opt.encoder = "utvideo";
    Recorder rec(opt);
    const RecorderResult e = rec.finish();
    CHECK(!e.ok && !e.error.empty());
    CHECK(access(opt.path.c_str(), F_OK) != 0);
  }

  if (failures == 0) std::system(("rm -rf '" + dir + "'").c_str());
  std::printf("%s\n", failures ? "FALHOU" : "ok");
  return failures ? 1 : 0;
}
