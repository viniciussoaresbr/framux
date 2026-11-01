#include "client/win_input.h"

#include <windows.h>

#include <algorithm>
#include <iterator>

#include "common/ipc_proto.h"
#include "common/keysyms.h"

namespace framux::client {

int vk_for_keysym(uint32_t k) {
  if (k > kMouseKeysymBase && k <= kMouseKeysymBase + 9) {
    // Numeração X11: 1 esquerdo, 2 meio, 3 direito, 8/9 laterais.
    switch (k - kMouseKeysymBase) {
      case 1: return VK_LBUTTON;
      case 2: return VK_MBUTTON;
      case 3: return VK_RBUTTON;
      case 8: return VK_XBUTTON1;
      case 9: return VK_XBUTTON2;
      default: return 0;
    }
  }
  if (k >= XK_a && k <= XK_z) return 'A' + int(k - XK_a);
  if (k >= XK_0 && k <= XK_9) return '0' + int(k - XK_0);
  if (k >= XK_F1 && k <= XK_F24) return VK_F1 + int(k - XK_F1);
  if (k >= XK_KP_0 && k <= XK_KP_9) return VK_NUMPAD0 + int(k - XK_KP_0);
  switch (k) {
    case XK_Escape: return VK_ESCAPE;
    case XK_Tab: return VK_TAB;
    case XK_space: return VK_SPACE;
    case XK_Return:
    case XK_KP_Enter: return VK_RETURN;
    case XK_BackSpace: return VK_BACK;
    case XK_Insert: return VK_INSERT;
    case XK_Delete: return VK_DELETE;
    case XK_Home: return VK_HOME;
    case XK_End: return VK_END;
    case XK_Prior: return VK_PRIOR;
    case XK_Next: return VK_NEXT;
    case XK_Left: return VK_LEFT;
    case XK_Right: return VK_RIGHT;
    case XK_Up: return VK_UP;
    case XK_Down: return VK_DOWN;
    case XK_Print: return VK_SNAPSHOT;
    case XK_Scroll_Lock: return VK_SCROLL;
    case XK_Pause: return VK_PAUSE;
    case XK_Menu: return VK_APPS;
    case XK_grave: return VK_OEM_3;
    case XK_minus: return VK_OEM_MINUS;
    case XK_equal: return VK_OEM_PLUS;
    case XK_bracketleft: return VK_OEM_4;
    case XK_bracketright: return VK_OEM_6;
    case XK_backslash: return VK_OEM_5;
    case XK_semicolon: return VK_OEM_1;
    case XK_apostrophe: return VK_OEM_7;
    case XK_comma: return VK_OEM_COMMA;
    case XK_period: return VK_OEM_PERIOD;
    case XK_slash: return VK_OEM_2;
    case XK_KP_Divide: return VK_DIVIDE;
    case XK_KP_Multiply: return VK_MULTIPLY;
    case XK_KP_Subtract: return VK_SUBTRACT;
    case XK_KP_Add: return VK_ADD;
    case XK_KP_Decimal: return VK_DECIMAL;
    default: return 0;
  }
}

std::unique_ptr<WinInput> WinInput::open() { return std::unique_ptr<WinInput>(new WinInput()); }

bool WinInput::snapshot() {
  std::copy(std::begin(keys_), std::end(keys_), std::begin(prev_));
  for (int vk = 1; vk < 256; ++vk) keys_[vk] = (GetAsyncKeyState(vk) & 0x8000) != 0;
  return true;
}

uint32_t WinInput::mods() const {
  uint32_t m = 0;
  if (down(VK_CONTROL)) m |= ipc::kModCtrl;
  if (down(VK_SHIFT)) m |= ipc::kModShift;
  if (down(VK_MENU)) m |= ipc::kModAlt;
  if (down(VK_LWIN) || down(VK_RWIN)) m |= ipc::kModSuper;
  return m;
}

bool WinInput::pressed(const Hotkey &h) const {
  if (!h.enabled()) return false;
  return down(vk_for_keysym(h.keysym)) && mods() == h.mods;
}

bool WinInput::newly_down(const Hotkey &h) const {
  const int vk = vk_for_keysym(h.keysym);
  return down(vk) && !was_down(vk);
}

int WinInput::focus_state() {
  HWND fg = GetForegroundWindow();
  if (!fg) return 0;
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId() ? 1 : 0;
}

}  // namespace framux::client
