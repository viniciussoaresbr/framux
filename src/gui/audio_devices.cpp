#include "gui/audio_devices.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace framux::gui {

namespace {

const char *get(const spa_dict *d, const char *key) { return d ? spa_dict_lookup(d, key) : nullptr; }

}  // namespace

struct AudioDevices::Impl {
  pw_thread_loop *loop = nullptr;
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  pw_registry *registry = nullptr;
  spa_hook registry_listener{};
  pw_stream *stream = nullptr;  // medidor
  spa_hook stream_listener{};
  std::string monitored;

  mutable std::mutex mu;
  std::map<uint32_t, Source> sources;  // mu
  std::atomic<uint64_t> revision{0};
  std::atomic<float> peak{0};

  static void on_global(void *data, uint32_t id, uint32_t, const char *type, uint32_t, const spa_dict *props) {
    auto *self = static_cast<Impl *>(data);
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;
    // Audio/Source: microfones; Audio/Source/Virtual: filtros (redução de
    // ruído e afins). Monitores de saída não entram.
    const char *cls = get(props, PW_KEY_MEDIA_CLASS);
    if (!cls || (std::strcmp(cls, "Audio/Source") != 0 && std::strcmp(cls, "Audio/Source/Virtual") != 0)) return;
    const char *name = get(props, PW_KEY_NODE_NAME);
    if (!name) return;
    const char *desc = get(props, PW_KEY_NODE_DESCRIPTION);
    if (!desc) desc = get(props, PW_KEY_NODE_NICK);
    {
      std::lock_guard lk(self->mu);
      self->sources[id] = Source{name, desc ? desc : name};
    }
    ++self->revision;
  }

  static void on_global_remove(void *data, uint32_t id) {
    auto *self = static_cast<Impl *>(data);
    std::lock_guard lk(self->mu);
    if (self->sources.erase(id)) ++self->revision;
  }

  static void on_process(void *data) {
    auto *self = static_cast<Impl *>(data);
    pw_buffer *b = pw_stream_dequeue_buffer(self->stream);
    if (!b) return;
    const spa_data &d = b->buffer->datas[0];
    if (d.data && d.chunk) {
      const auto *s = reinterpret_cast<const float *>(static_cast<const uint8_t *>(d.data) + d.chunk->offset);
      float m = 0;
      for (uint32_t i = 0, n = d.chunk->size / sizeof(float); i < n; ++i) m = std::max(m, std::fabs(s[i]));
      float cur = self->peak.load();
      while (m > cur && !self->peak.compare_exchange_weak(cur, m)) {
      }
    }
    pw_stream_queue_buffer(self->stream, b);
  }

  // Com o loop travado.
  void stop_stream() {
    if (stream) pw_stream_destroy(stream);
    stream = nullptr;
    monitored.clear();
  }

  void start_stream(const std::string &device) {
    pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                                             PW_KEY_MEDIA_ROLE, "Production", PW_KEY_NODE_NAME, "framux-mic-level",
                                             PW_KEY_NODE_DESCRIPTION, "Framux (nível do microfone)", nullptr);
    if (device != "default") pw_properties_set(props, PW_KEY_TARGET_OBJECT, device.c_str());
    stream = pw_stream_new(core, "framux-mic-level", props);
    if (!stream) return;
    static pw_stream_events events = [] {
      pw_stream_events e{};
      e.version = PW_VERSION_STREAM_EVENTS;
      e.process = on_process;
      return e;
    }();
    pw_stream_add_listener(stream, &stream_listener, &events, this);
    uint8_t buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = 48000;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
    pw_stream_connect(stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                      pw_stream_flags(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
    monitored = device;
  }
};

AudioDevices::AudioDevices() : impl_(std::make_unique<Impl>()) {
  static std::once_flag once;
  std::call_once(once, [] { pw_init(nullptr, nullptr); });
  Impl &im = *impl_;
  im.loop = pw_thread_loop_new("framux-gui-audio", nullptr);
  if (!im.loop) return;
  im.context = pw_context_new(pw_thread_loop_get_loop(im.loop), nullptr, 0);
  if (!im.context) return;
  im.core = pw_context_connect(im.context, nullptr, 0);
  if (!im.core) return;
  static const pw_registry_events events = {PW_VERSION_REGISTRY_EVENTS, Impl::on_global, Impl::on_global_remove};
  im.registry = pw_core_get_registry(im.core, PW_VERSION_REGISTRY, 0);
  pw_registry_add_listener(im.registry, &im.registry_listener, &events, &im);
  pw_thread_loop_start(im.loop);
}

AudioDevices::~AudioDevices() {
  Impl &im = *impl_;
  if (im.loop) pw_thread_loop_stop(im.loop);
  im.stop_stream();
  if (im.registry) {
    spa_hook_remove(&im.registry_listener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(im.registry));
  }
  if (im.core) pw_core_disconnect(im.core);
  if (im.context) pw_context_destroy(im.context);
  if (im.loop) pw_thread_loop_destroy(im.loop);
}

bool AudioDevices::available() const { return impl_->core != nullptr; }

uint64_t AudioDevices::revision() const { return impl_->revision; }

std::vector<AudioDevices::Source> AudioDevices::sources() const {
  std::lock_guard lk(impl_->mu);
  std::vector<Source> out;
  for (const auto &[id, s] : impl_->sources) out.push_back(s);
  return out;
}

void AudioDevices::monitor(const std::string &device) {
  Impl &im = *impl_;
  if (!im.core || device == im.monitored) return;
  pw_thread_loop_lock(im.loop);
  im.stop_stream();
  if (!device.empty()) im.start_stream(device);
  pw_thread_loop_unlock(im.loop);
  im.peak = 0;
}

float AudioDevices::take_peak() { return impl_->peak.exchange(0); }

}  // namespace framux::gui
