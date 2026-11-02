// Contador para jogos Direct3D/OpenGL no Windows, desenhado de fora do jogo:
// uma janela em camadas, sempre no topo, transparente a cliques e fora das
// capturas, posicionada sobre a área cliente do jogo. Usa os mesmos
// retângulos da fonte de segmentos (overlay.h). Não aparece em tela cheia
// exclusiva (só em janela e janela sem bordas).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "common/overlay.h"

namespace framux::daemon {

class OverlayWindow {
 public:
  ~OverlayWindow();
  bool start();
  void stop();

  // Desenha `draw` sobre a área cliente da janela `target` (HWND). Chamado
  // pela thread principal; a janela é atualizada na thread dela.
  void show(void *target, const OverlayDraw &draw);
  void hide();

 private:
  void main();
  void render();

  std::mutex mu_;
  std::condition_variable started_;
  bool ready_ = false;
  void *hwnd_ = nullptr;  // janela do overlay
  void *target_ = nullptr;
  OverlayDraw draw_;
  bool visible_ = false;
  std::thread thread_;
  // Só na thread da janela: o que está na tela. Cada UpdateLayeredWindow faz
  // o DWM recompor a tela sobre o jogo; sem mudança, nada é enviado.
  OverlayDraw shown_draw_;
  long shown_x_ = 0, shown_y_ = 0;
  bool shown_ = false;
  unsigned long long topmost_at_ = 0;  // GetTickCount64 do último reforço do topo
};

}  // namespace framux::daemon
