// Captura de uma janela pelo Windows Graphics Capture (Windows 10 1903+),
// para gravar e fotografar jogos Direct3D/OpenGL de fora do processo deles.
// O compositor entrega cada quadro numa textura da GPU; aqui ele é copiado
// para a memória (BGRA) e entregue ao chamador numa thread própria.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace framux::daemon {

class WindowCapture {
 public:
  // Quadro BGRA 8 bits (`stride` bytes por linha) no instante `t_ns`
  // (relógio de now_ns()). O ponteiro vale só durante a chamada.
  using Sink = std::function<void(const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride, uint64_t t_ns)>;

  // Entrega pela GPU (gravação): o quadro fica em textura, sem a cópia lida
  // pela CPU. `open` é chamado no primeiro quadro, com o ID3D11Device da
  // captura e o tamanho do recorte; false = segue pelo Sink (memória).
  // `push` recebe a textura BGRA do Windows (válida só durante a chamada) e
  // o recorte da área cliente. Os dois rodam na thread da captura.
  struct GpuTarget {
    std::function<bool(void *device, uint32_t w, uint32_t h)> open;
    std::function<void(void *texture, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint64_t t_ns)> push;
  };

  WindowCapture();
  ~WindowCapture();
  // Começa a capturar `hwnd`. `max_fps` limita a entrega (0 = cada quadro).
  bool start(void *hwnd, bool cursor, uint32_t max_fps, Sink sink, std::string *err, GpuTarget gpu = {});
  void stop();

  // Um quadro só (screenshot): espera até `timeout_ms`.
  static bool grab(void *hwnd, std::vector<uint8_t> &bgra, uint32_t *w, uint32_t *h, std::string *err,
                   uint32_t timeout_ms = 1500);

  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

// O sistema tem Windows Graphics Capture?
bool window_capture_supported();

}  // namespace framux::daemon
