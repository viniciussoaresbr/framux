// recorder.h sem gravação (meson -Drecording=disabled): o daemon compila sem
// FFmpeg, e F9 / `framux rec` respondem que a gravação não está disponível.
#include <unistd.h>

#include "daemon/recorder.h"

namespace framux::daemon {

namespace {
constexpr const char *kUnavailable = "gravação de vídeo indisponível nesta build do Framux";
}

EncoderProbe probe_encoders() { return {{}, kUnavailable}; }

struct Recorder::Impl {
  RecorderOptions opt;
};

Recorder::Recorder(RecorderOptions opt) : impl_(std::make_unique<Impl>()) {
  impl_->opt = std::move(opt);
  if (impl_->opt.fd >= 0) close(impl_->opt.fd);
  impl_->opt.fd = -1;
}

Recorder::~Recorder() = default;

void Recorder::push_video(const uint8_t *, uint32_t, uint32_t, uint32_t, uint32_t, bool, bool, uint64_t,
                          std::function<void()> release) {
  if (release) release();
}

#ifdef _WIN32
bool Recorder::gpu_open(void *, uint32_t, uint32_t) { return false; }
void Recorder::gpu_push(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t) {}
#endif

void Recorder::push_audio(AudioSource, const float *, uint32_t, uint64_t) {}
void Recorder::set_volume(AudioSource, float) {}
void Recorder::add_layer_drops(uint32_t) {}
bool Recorder::failed() const { return true; }
std::string Recorder::error() const { return kUnavailable; }

RecorderResult Recorder::finish() {
  RecorderResult r;
  r.path = impl_->opt.path;
  r.error = kUnavailable;
  if (!r.path.empty()) unlink(r.path.c_str());  // arquivo vazio criado pelo daemon
  return r;
}

}  // namespace framux::daemon
