// Sintaxe de hotkeys da config: "F9", "Ctrl+Shift+F9", "Mouse4", "" (desativada).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace framux {

// Botões de mouse são representados como keysyms fora da faixa X11.
inline constexpr uint32_t kMouseKeysymBase = 0x7f000000;

struct Hotkey {
  uint32_t keysym = 0;  // 0 = desativada
  uint32_t mods = 0;    // ipc::KeyMods

  bool enabled() const { return keysym != 0; }
  bool is_mouse() const { return keysym > kMouseKeysymBase && keysym <= kMouseKeysymBase + 9; }
  int mouse_button() const { return is_mouse() ? int(keysym - kMouseKeysymBase) : 0; }
  bool operator==(const Hotkey &) const = default;
};

// nullopt = sintaxe inválida. String vazia = Hotkey desativada.
std::optional<Hotkey> parse_hotkey(std::string_view s);
std::string format_hotkey(const Hotkey &h);

}  // namespace framux
