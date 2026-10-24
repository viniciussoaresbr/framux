// Hotkeys globais em sessão X11 via XGrabKey/XGrabButton (mecanismo 2, §B.9).
// O grab é inerente ao mecanismo: a tecla capturada não chega ao jogo.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "common/hotkey.h"

namespace framux::daemon {

class X11Hotkeys {
 public:
  using Callback = std::function<void(uint32_t keysym, uint32_t mods, bool pressed)>;

  ~X11Hotkeys();
  static std::unique_ptr<X11Hotkeys> open(Callback cb);

  int fd() const;
  void set_bindings(const std::vector<Hotkey> &keys);
  void dispatch();  // chamar quando fd() estiver legível

 private:
  X11Hotkeys() = default;
  void ungrab_all();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace framux::daemon
