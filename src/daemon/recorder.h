// Gravação de vídeo (§B.7): converte os quadros vindos do jogo, codifica com
// FFmpeg (encoder de hardware quando houver) e escreve um MKV com trilha de
// vídeo e, opcionalmente, o áudio do jogo em Opus. Tudo numa thread própria:
// o loop do daemon só enfileira.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace framux::daemon {

// Chaves de [video] encoder detectadas nesta máquina, na ordem de preferência
// do modo automático. `missing` explica o que falta (ex.: RPM Fusion).
struct EncoderProbe {
  std::vector<std::string> available;
  std::string missing;
};

// Abre de verdade cada encoder com um quadro pequeno. Lento (carrega CUDA/VA):
// o daemon roda isso num processo filho (`framuxd --probe-encoders`).
EncoderProbe probe_encoders();

struct RecorderOptions {
  int fd = -1;       // arquivo .mkv já criado (o Recorder assume o fd)
  std::string path;  // caminho dele, para remux e mensagens
  std::string game;  // tag title
  int fps = 60;
  bool half = false;
  std::string encoder;                 // chave de [video] encoder ("auto" = primeiro de `available`)
  std::string quality;                 // low | medium | high | lossless
  bool to_mp4 = false;                 // remux para MP4 ao terminar
  bool audio = false;                  // cria a trilha "Game" (Opus 48 kHz estéreo)
  std::vector<std::string> available;  // de probe_encoders(); vazio = tentar todos
};

struct RecorderResult {
  bool ok = false;
  std::string path;  // arquivo final (.mkv ou .mp4)
  std::string error;
  double seconds = 0;
  uint64_t bytes = 0;
  uint64_t frames = 0;
  uint64_t duplicated = 0;  // repetidos para manter a taxa constante
  uint64_t dropped = 0;     // chegaram adiantados demais e foram descartados
  std::string encoder;      // nome do encoder FFmpeg usado
};

class Recorder {
 public:
  explicit Recorder(RecorderOptions opt);
  ~Recorder();
  Recorder(const Recorder &) = delete;
  Recorder &operator=(const Recorder &) = delete;

  // Quadro do jogo. `data` fica válido até `release()` ser chamado (na thread
  // do Recorder, logo depois da conversão). Formatos: VkFormat RGB de 32 bits
  // ou NV12 (ipc::kVkFormatNv12, BT.709 limitado). `halved`: a camada já
  // reduziu à metade (não reduzir de novo).
  void push_video(const uint8_t *data, uint32_t width, uint32_t height, uint32_t stride, uint32_t vk_format,
                  bool flip_y, bool halved, uint64_t present_ns, std::function<void()> release);

  // Amostras float intercaladas, 2 canais, 48 kHz; `time_ns` é o
  // CLOCK_MONOTONIC da primeira amostra. Pode ser chamado de qualquer thread.
  void push_audio(const float *samples, uint32_t frames, uint64_t time_ns);

  // Descartes informados pela camada (sem slot livre).
  void add_layer_drops(uint32_t n);

  // Falhou de forma irrecuperável (encoder, disco cheio…): o daemon para.
  bool failed() const;
  std::string error() const;

  // Codifica o que falta, fecha o arquivo e faz o remux. Bloqueia.
  RecorderResult finish();

  static constexpr int kAudioRate = 48000;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace framux::daemon
