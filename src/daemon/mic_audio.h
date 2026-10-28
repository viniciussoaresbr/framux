// Microfone (§B.8, fonte 2): um stream de captura do PipeWire ligado pelo
// WirePlumber à fonte escolhida (node.name) ou à padrão do sistema. Microfone
// mono chega duplicado nos dois canais. Thread própria do PipeWire.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace framux::daemon {

class MicAudio {
 public:
  // Amostras float intercaladas, 2 canais, 48 kHz; `time_ns` é o
  // CLOCK_MONOTONIC da primeira amostra. Chamado na thread do PipeWire.
  using Sink = std::function<void(const float *samples, uint32_t frames, uint64_t time_ns)>;

  // `device`: node.name da fonte; "default" ou vazio = padrão do sistema.
  static std::unique_ptr<MicAudio> start(const std::string &device, Sink sink, std::string *err);
  ~MicAudio();

 private:
  MicAudio() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace framux::daemon
