// GameAudio (§B.8): o próprio processo toca um seno de 440 Hz num stream do
// PipeWire (sem destino: nada sai nos alto-falantes) e o GameAudio tem que
// achá-lo pelo PID, ligar-se a ele e receber o som. Roda num PipeWire
// privado (audio_test.sh), nunca no da sessão do usuário.
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

namespace {

struct Tone {
  pw_stream *stream = nullptr;
  spa_hook listener{};
  uint64_t phase = 0;
};

void on_tone(void *data) {
  auto *t = static_cast<Tone *>(data);
  pw_buffer *b = pw_stream_dequeue_buffer(t->stream);
  if (!b) return;
  spa_data &d = b->buffer->datas[0];
  auto *out = static_cast<float *>(d.data);
  uint32_t frames = d.maxsize / (2 * sizeof(float));
  if (b->requested && b->requested < frames) frames = uint32_t(b->requested);
  for (uint32_t i = 0; i < frames; ++i, ++t->phase)
    out[2 * i] = out[2 * i + 1] = 0.5f * float(std::sin(2 * M_PI * 440.0 * double(t->phase) / 48000.0));
  d.chunk->offset = 0;
  d.chunk->stride = 2 * sizeof(float);
  d.chunk->size = frames * 2 * sizeof(float);
  pw_stream_queue_buffer(t->stream, b);
}

}  // namespace

int main() {
  pw_init(nullptr, nullptr);
  pw_thread_loop *loop = pw_thread_loop_new("tone", nullptr);
  pw_thread_loop_lock(loop);
  pw_thread_loop_start(loop);
  Tone tone;
  // Um "jogo" mínimo: stream de saída com media.class Stream/Output/Audio.
  pw_properties *props =
      pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Game",
                        PW_KEY_NODE_NAME, "framux-test-tone", PW_KEY_NODE_ALWAYS_PROCESS, "true", nullptr);
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
  info.channels = 2;
  info.position[0] = SPA_AUDIO_CHANNEL_FL;
  info.position[1] = SPA_AUDIO_CHANNEL_FR;
  const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
  // Sem autoconnect: não toca em lugar nenhum. Sem sink, o ritmo vem do
  // driver de reserva do PipeWire (node.always-process).
  pw_stream_connect(tone.stream, PW_DIRECTION_OUTPUT, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
  pw_thread_loop_unlock(loop);

  std::mutex mu;
  std::vector<float> got;
  std::atomic<uint64_t> first_ns{0}, last_ns{0};
  std::string err;
  auto audio = framux::daemon::GameAudio::start(
      getpid(), getpid(),
      [&](const float *s, uint32_t frames, uint64_t t) {
        std::lock_guard lk(mu);
        if (!first_ns) first_ns = t;
        last_ns = t;
        got.insert(got.end(), s, s + size_t(frames) * 2);
      },
      &err);
  if (!audio) {
    std::fprintf(stderr, "GameAudio::start: %s\n", err.c_str());
    return 1;
  }
  sleep(2);
  audio.reset();

  pw_thread_loop_lock(loop);
  pw_stream_destroy(tone.stream);
  pw_thread_loop_unlock(loop);
  pw_thread_loop_stop(loop);
  pw_thread_loop_destroy(loop);

  std::lock_guard lk(mu);
  const size_t frames = got.size() / 2;
  double energy = 0;
  size_t crossings = 0;
  for (size_t i = 0; i < frames; ++i) {
    energy += double(got[2 * i]) * got[2 * i];
    if (i && (got[2 * (i - 1)] < 0) != (got[2 * i] < 0)) ++crossings;
  }
  const double rms = frames ? std::sqrt(energy / double(frames)) : 0;
  const double freq = frames ? crossings / 2.0 / (double(frames) / 48000.0) : 0;
  std::printf("capturado: %zu quadros (%.2f s), rms %.3f, ~%.0f Hz, relógio %.2f s\n", frames, frames / 48000.0, rms,
              freq, double(last_ns - first_ns) / 1e9);
  bool ok = frames > 48000 / 2 && rms > 0.2 && std::fabs(freq - 440) < 25 && last_ns > first_ns;
  std::printf("%s\n", ok ? "ok" : "FALHOU");
  return ok ? 0 : 1;
}
