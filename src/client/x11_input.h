// Leitura de teclado/mouse dentro do processo do jogo (mecanismo 3, §B.9) e
// detecção de foco, via libxcb carregada com dlopen.
//
// Usa xcb em vez de Xlib de propósito: erros do xcb voltam por requisição,
// enquanto a Xlib os entrega ao error handler global do processo (o do jogo),
// que por padrão encerra o programa. Nunca consome teclas: só consulta estado.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "common/hotkey.h"

namespace framux::client {

class X11Input {
 public:
  ~X11Input();
  // nullptr se não houver DISPLAY ou libxcb.
  static std::unique_ptr<X11Input> open();

  // Estado atual: a combinação está pressionada (modificadores exatos)?
  // `snapshot()` faz as consultas ao servidor; `pressed()` só lê o snapshot.
  bool snapshot();
  bool pressed(const Hotkey &h) const;
  // A tecla/botão principal de `h` desceu agora (estava solto no snapshot
  // anterior)? Soltar o Shift de Shift+F12 com o F12 ainda preso faz
  // `pressed(F12)` virar verdadeiro sem um F12 novo; isso não conta.
  bool newly_down(const Hotkey &h) const;

  // Janela X do jogo (da superfície Vulkan ou do drawable GLX).
  void set_window(uint32_t window) {
    window_ = window;
    ancestors_valid_ = false;
  }
  bool has_window() const { return window_ != 0; }

  // 1 = com foco, 0 = sem foco, 2 = desconhecido.
  int focus_state();

 private:
  X11Input() = default;
  bool load();
  uint32_t keycode_for(uint32_t keysym) const;
  uint32_t atom(const char *name);
  void refresh_ancestors();

  struct Fns;
  std::unique_ptr<Fns> f_;
  void *conn_ = nullptr;
  uint32_t root_ = 0;
  uint8_t min_kc_ = 8, max_kc_ = 255;
  std::vector<uint32_t> keysyms_;  // keysyms por keycode
  int per_kc_ = 0;
  uint8_t keys_[32] = {};
  uint8_t prev_keys_[32] = {};
  uint32_t buttons_ = 0;  // bit n = botão X n
  uint32_t prev_buttons_ = 0;
  uint32_t mods_ = 0;
  uint32_t mod_kc_[8] = {};  // Control_L/R, Shift_L/R, Alt_L/R, Super_L/R
  uint32_t window_ = 0;
  std::vector<uint32_t> ancestors_;
  bool ancestors_valid_ = false;
  uint32_t net_active_ = 0, net_wm_pid_ = 0;
  bool have_xinput_ = false;
};

}  // namespace framux::client
