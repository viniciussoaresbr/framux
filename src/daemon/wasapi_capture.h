// Captura WASAPI comum ao áudio do jogo (loopback do processo) e ao
// microfone: o cliente de áudio já inicializado em float 48 kHz estéreo é
// lido numa thread própria e entregue ao Sink com o instante de cada bloco.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

struct IAudioClient;

namespace framux::daemon::wasapi {

using Sink = std::function<void(const float *samples, uint32_t frames, uint64_t time_ns)>;

inline constexpr uint32_t kRate = 48000;
inline constexpr uint32_t kChannels = 2;

class Capture {
 public:
  ~Capture();
  // Inicializa `client` (modo compartilhado, evento, float 48 kHz estéreo;
  // `loopback` para o loopback de processo) e começa a ler. Assume a
  // referência de `client`.
  bool start(IAudioClient *client, bool loopback, Sink sink, std::string *err);
  void stop();

 private:
  void main();
  IAudioClient *client_ = nullptr;
  void *event_ = nullptr;
  Sink sink_;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

}  // namespace framux::daemon::wasapi
