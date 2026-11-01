// Leitura de teclado/mouse dentro do processo do jogo no Windows: estado
// assíncrono (GetAsyncKeyState), sem hooks e sem consumir teclas, como o
// X11Input faz no Linux. Foco = a janela em primeiro plano é deste processo.
#pragma once

#include <cstdint>
#include <memory>

#include "common/hotkey.h"

namespace framux::client {

class WinInput {
 public:
  static std::unique_ptr<WinInput> open();

  bool snapshot();
  bool pressed(const Hotkey &h) const;
  bool newly_down(const Hotkey &h) const;

  void set_window(uint32_t window) { window_ = window; }
  bool has_window() const { return window_ != 0; }

  // 1 = com foco, 0 = sem foco.
  int focus_state();

 private:
  WinInput() = default;
  uint32_t mods() const;
  bool down(int vk) const { return vk > 0 && vk < 256 && keys_[vk]; }
  bool was_down(int vk) const { return vk > 0 && vk < 256 && prev_[vk]; }

  bool keys_[256] = {};
  bool prev_[256] = {};
  uint32_t window_ = 0;
};

// Keysym (ou botão de mouse) da config → código virtual do Windows. 0 = sem.
int vk_for_keysym(uint32_t keysym);

}  // namespace framux::client
