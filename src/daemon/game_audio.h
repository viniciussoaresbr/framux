// Áudio do jogo isolado (§B.8, fonte 1): um stream de captura do PipeWire
// ligado só às saídas de áudio do processo do jogo (application.process.id),
// incluindo as que ele abrir durante a gravação. Thread própria do PipeWire.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace framux::daemon {

class GameAudio {
 public:
  // Amostras float intercaladas, 2 canais, 48 kHz; `time_ns` é o
  // CLOCK_MONOTONIC da primeira amostra. Chamado na thread do PipeWire.
  using Sink = std::function<void(const float *samples, uint32_t frames, uint64_t time_ns)>;

  // `pid`: o informado pelo jogo (no Steam Flatpak, o do sandbox, que é o que
  // a libpulse do jogo anuncia); `peer_pid`: o visto pelo daemon.
  static std::unique_ptr<GameAudio> start(int pid, int peer_pid, Sink sink, std::string *err);
  ~GameAudio();

 private:
  GameAudio() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace framux::daemon
