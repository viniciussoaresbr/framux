#include "daemon/mic_audio.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <time.h>

#include <mutex>
#include <utility>

#include "common/log.h"

namespace framux::daemon {

namespace {

constexpr uint32_t kRate = 48000;

uint64_t monotonic_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

}  // namespace

struct MicAudio::Impl {
  Sink sink;
  pw_thread_loop *loop = nullptr;
  pw_stream *stream = nullptr;
  spa_hook stream_listener{};
  bool logged = false;

  static void on_state(void *data, pw_stream_state, pw_stream_state state, const char *error) {
    auto *self = static_cast<Impl *>(data);
    if (state == PW_STREAM_STATE_ERROR) FRAMUX_LOG("gravação: microfone com erro: %s", error ? error : "?");
    if (state == PW_STREAM_STATE_STREAMING && !self->logged) {
      self->logged = true;
      FRAMUX_LOG("gravação: capturando o microfone (nó %u do PipeWire)", pw_stream_get_node_id(self->stream));
    }
  }

  static void on_process(void *data) {
    auto *self = static_cast<Impl *>(data);
    pw_buffer *b = pw_stream_dequeue_buffer(self->stream);
    if (!b) return;
    const spa_data &d = b->buffer->datas[0];
    if (d.data && d.chunk) {
      const uint32_t frames = d.chunk->size / (2 * sizeof(float));
      pw_time t{};
      uint64_t when = monotonic_ns();
      if (pw_stream_get_time_n(self->stream, &t, sizeof t) == 0 && t.now > 0) {
        when = uint64_t(t.now);
        if (t.rate.denom && t.delay > 0) when -= uint64_t(t.delay) * 1'000'000'000ull * t.rate.num / t.rate.denom;
      }
      const auto *samples = reinterpret_cast<const float *>(static_cast<const uint8_t *>(d.data) + d.chunk->offset);
      if (frames) self->sink(samples, frames, when);
    }
    pw_stream_queue_buffer(self->stream, b);
  }

  void teardown() {
    if (loop) pw_thread_loop_stop(loop);
    if (stream) pw_stream_destroy(stream);
    stream = nullptr;
    if (loop) pw_thread_loop_destroy(loop);
    loop = nullptr;
  }
};

std::unique_ptr<MicAudio> MicAudio::start(const std::string &device, Sink sink, std::string *err) {
  static std::once_flag once;
  std::call_once(once, [] { pw_init(nullptr, nullptr); });

  std::unique_ptr<MicAudio> mic(new MicAudio);
  mic->impl_ = std::make_unique<Impl>();
  Impl &im = *mic->impl_;
  im.sink = std::move(sink);
  im.loop = pw_thread_loop_new("framux-mic", nullptr);
  if (!im.loop) {
    *err = "PipeWire indisponível";
    return nullptr;
  }
  pw_properties *props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Production", PW_KEY_NODE_NAME,
      "framux-microphone", PW_KEY_NODE_DESCRIPTION, "Framux (microfone)", PW_KEY_APP_NAME, "Framux", nullptr);
  // Fonte sumida: o WirePlumber cai na padrão em vez de gravar silêncio.
  if (!device.empty() && device != "default") pw_properties_set(props, PW_KEY_TARGET_OBJECT, device.c_str());
  static pw_stream_events stream_events = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = Impl::on_state;
    e.process = Impl::on_process;
    return e;
  }();
  pw_thread_loop_lock(im.loop);
  im.stream = pw_stream_new_simple(pw_thread_loop_get_loop(im.loop), "framux-microphone", props, &stream_events, &im);
  if (!im.stream) {
    pw_thread_loop_unlock(im.loop);
    *err = "não consegui conectar ao PipeWire";
    im.teardown();
    return nullptr;
  }

  uint8_t buf[1024];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
  spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = kRate;
  info.channels = 2;
  info.position[0] = SPA_AUDIO_CHANNEL_FL;
  info.position[1] = SPA_AUDIO_CHANNEL_FR;
  const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
  const int r = pw_stream_connect(im.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                                  pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
  pw_thread_loop_unlock(im.loop);
  if (r < 0 || pw_thread_loop_start(im.loop) < 0) {
    *err = "não consegui criar o stream do microfone no PipeWire";
    im.teardown();
    return nullptr;
  }
  return mic;
}

MicAudio::~MicAudio() {
  if (impl_) impl_->teardown();
}

}  // namespace framux::daemon
