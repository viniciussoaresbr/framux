// GameAudio e MicAudio (§B.8): o próprio processo toca um seno de 440 Hz num
// stream do PipeWire (sem destino: nada sai nos alto-falantes) e o GameAudio
// tem que achá-lo pelo PID, ligar-se a ele e receber o som. Depois, uma
// fonte mono de 1 kHz faz o papel de microfone: a lista da janela
// (AudioDevices) tem que mostrá-la, o medidor tem que ver o sinal e o
// MicAudio tem que gravá-la em estéreo. Roda num PipeWire privado
// (audio_test.sh), nunca no da sessão do usuário.
#include <pipewire/impl.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "daemon/game_audio.h"
#include "daemon/mic_audio.h"
#include "gui/audio_devices.h"

namespace {

struct Tone {
  pw_stream *stream = nullptr;
  double freq = 440;
  uint32_t channels = 2;
  uint64_t phase = 0;
};

void on_tone(void *data) {
  auto *t = static_cast<Tone *>(data);
  pw_buffer *b = pw_stream_dequeue_buffer(t->stream);
  if (!b) return;
  spa_data &d = b->buffer->datas[0];
  auto *out = static_cast<float *>(d.data);
  const uint32_t stride = t->channels * sizeof(float);
  uint32_t frames = d.maxsize / stride;
  if (b->requested && b->requested < frames) frames = uint32_t(b->requested);
  for (uint32_t i = 0; i < frames; ++i, ++t->phase)
    for (uint32_t c = 0; c < t->channels; ++c)
      out[t->channels * i + c] = 0.5f * float(std::sin(2 * M_PI * t->freq * double(t->phase) / 48000.0));
  d.chunk->offset = 0;
  d.chunk->stride = int32_t(stride);
  d.chunk->size = frames * stride;
  pw_stream_queue_buffer(t->stream, b);
}

// Stream de saída sem autoconnect: não toca em lugar nenhum. Sem sink, o
// ritmo vem do driver de reserva do PipeWire (node.always-process).
void start_tone(pw_thread_loop *loop, Tone &tone, pw_properties *props) {
  tone.stream = pw_stream_new_simple(
      pw_thread_loop_get_loop(loop), "tone", props,
      [] {
        static pw_stream_events e{};
        e.version = PW_VERSION_STREAM_EVENTS;
        e.process = on_tone;
        return &e;
      }(),
      &tone);
  uint8_t buf[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = 48000;
  info.channels = tone.channels;
  if (tone.channels == 1) {
    info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  } else {
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
  }
  const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
  // Com target.object, o WirePlumber liga ao destino; sem, a lugar nenhum.
  const bool target = pw_stream_get_properties(tone.stream) &&
                      pw_properties_get(pw_stream_get_properties(tone.stream), PW_KEY_TARGET_OBJECT);
  pw_stream_connect(tone.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                    pw_stream_flags(PW_STREAM_FLAG_MAP_BUFFERS | (target ? PW_STREAM_FLAG_AUTOCONNECT : 0)), params, 1);
}

void stop_tone(pw_thread_loop *loop, Tone &tone) {
  pw_thread_loop_lock(loop);
  pw_stream_destroy(tone.stream);
  pw_thread_loop_unlock(loop);
}

// O que um Sink recebeu: amostras e o relógio do primeiro e último bloco.
struct Capture {
  std::mutex mu;
  std::vector<float> got;
  std::atomic<uint64_t> first_ns{0}, last_ns{0};

  void push(const float *s, uint32_t frames, uint64_t t) {
    std::lock_guard lk(mu);
    if (!first_ns) first_ns = t;
    last_ns = t;
    got.insert(got.end(), s, s + size_t(frames) * 2);
  }

  // Seno de amplitude 0,5 na frequência `freq`, igual nos dois canais.
  bool check(const char *what, double want_freq) {
    std::lock_guard lk(mu);
    const size_t frames = got.size() / 2;
    double energy = 0, diff = 0;
    size_t crossings = 0;
    for (size_t i = 0; i < frames; ++i) {
      energy += double(got[2 * i]) * got[2 * i];
      diff = std::max(diff, double(std::fabs(got[2 * i] - got[2 * i + 1])));
      if (i && (got[2 * (i - 1)] < 0) != (got[2 * i] < 0)) ++crossings;
    }
    const double rms = frames ? std::sqrt(energy / double(frames)) : 0;
    const double freq = frames ? crossings / 2.0 / (double(frames) / 48000.0) : 0;
    std::printf("%s: %zu quadros (%.2f s), rms %.3f, ~%.0f Hz, canais diferem %.3f, relógio %.2f s\n", what, frames,
                frames / 48000.0, rms, freq, diff, double(last_ns - first_ns) / 1e9);
    return frames > 48000 / 2 && rms > 0.2 && std::fabs(freq - want_freq) < want_freq / 16 && diff < 0.01 &&
           last_ns > first_ns;
  }
};

}  // namespace

int main() {
  pw_init(nullptr, nullptr);
  pw_thread_loop *loop = pw_thread_loop_new("tone", nullptr);
  pw_thread_loop_start(loop);
  bool ok = true;

  // Um "jogo" mínimo: stream de saída com media.class Stream/Output/Audio.
  {
    Tone tone;
    pw_thread_loop_lock(loop);
    start_tone(
        loop, tone,
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Game",
                          PW_KEY_NODE_NAME, "framux-test-tone", PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr));
    pw_thread_loop_unlock(loop);
    Capture cap;
    std::string err;
    auto audio = framux::daemon::GameAudio::start(
        getpid(), getpid(), [&](const float *s, uint32_t frames, uint64_t t) { cap.push(s, frames, t); }, &err);
    if (!audio) {
      std::fprintf(stderr, "GameAudio::start: %s\n", err.c_str());
      return 1;
    }
    sleep(2);
    audio.reset();
    stop_tone(loop, tone);
    ok &= cap.check("jogo", 440);
  }

  // Um "microfone" mono: um loopback (como os filtros de voz) cuja saída é
  // a fonte Audio/Source "framux-test-mic", alimentado por um seno de 1 kHz.
  {
    pw_thread_loop_lock(loop);
    pw_context *ctx = pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
    pw_impl_module *loopback = pw_context_load_module(ctx, "libpipewire-module-loopback",
                                                      "{ audio.position = [ MONO ]"
                                                      "  capture.props = { node.name = framux-test-mic-in"
                                                      "    media.class = Audio/Sink }"
                                                      "  playback.props = { node.name = framux-test-mic"
                                                      "    node.description = \"Microfone de teste\""
                                                      "    media.class = Audio/Source } }",
                                                      nullptr);
    pw_thread_loop_unlock(loop);
    if (!loopback) {
      std::fprintf(stderr, "libpipewire-module-loopback indisponível\n");
      return 1;
    }
    Tone mic;
    mic.freq = 1000;
    mic.channels = 1;
    pw_thread_loop_lock(loop);
    start_tone(loop, mic,
               pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_NODE_NAME,
                                 "framux-test-voice", PW_KEY_TARGET_OBJECT, "framux-test-mic-in",
                                 PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr));
    pw_thread_loop_unlock(loop);

    framux::gui::AudioDevices devices;
    bool listed = false;
    for (int i = 0; i < 50 && !listed; ++i) {
      for (const auto &s : devices.sources())
        listed |= s.name == "framux-test-mic" && s.description == "Microfone de teste";
      if (!listed) usleep(100'000);
    }
    std::printf("lista da janela: %s\n", listed ? "microfone de teste presente" : "FALTANDO");
    ok &= devices.available() && listed;
    devices.monitor("framux-test-mic");
    float peak = 0;
    for (int i = 0; i < 20; ++i) {
      usleep(50'000);
      peak = std::max(peak, devices.take_peak());
    }
    devices.monitor("");
    std::printf("medidor: pico %.3f\n", peak);
    ok &= peak > 0.4f && peak < 0.6f;

    Capture cap;
    std::string err;
    auto audio = framux::daemon::MicAudio::start(
        "framux-test-mic", [&](const float *s, uint32_t frames, uint64_t t) { cap.push(s, frames, t); }, &err);
    if (!audio) {
      std::fprintf(stderr, "MicAudio::start: %s\n", err.c_str());
      return 1;
    }
    sleep(2);
    audio.reset();
    stop_tone(loop, mic);
    ok &= cap.check("microfone", 1000);
    pw_thread_loop_lock(loop);
    pw_impl_module_destroy(loopback);
    pw_context_destroy(ctx);
    pw_thread_loop_unlock(loop);
  }

  pw_thread_loop_stop(loop);
  pw_thread_loop_destroy(loop);
  std::printf("%s\n", ok ? "ok" : "FALHOU");
  return ok ? 0 : 1;
}
