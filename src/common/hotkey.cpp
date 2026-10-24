#include "common/hotkey.h"

#include <X11/keysym.h>

#include <cctype>
#include <cstdio>

#include "common/ipc_proto.h"

namespace framux {

namespace {

struct NamedKey {
  const char *name;
  uint32_t keysym;
};

constexpr NamedKey kNamed[] = {
    {"Escape", XK_Escape},
    {"Esc", XK_Escape},
    {"Tab", XK_Tab},
    {"Space", XK_space},
    {"Return", XK_Return},
    {"Enter", XK_Return},
    {"BackSpace", XK_BackSpace},
    {"Insert", XK_Insert},
    {"Delete", XK_Delete},
    {"Home", XK_Home},
    {"End", XK_End},
    {"PageUp", XK_Prior},
    {"PageDown", XK_Next},
    {"Left", XK_Left},
    {"Right", XK_Right},
    {"Up", XK_Up},
    {"Down", XK_Down},
    {"Print", XK_Print},
    {"PrintScreen", XK_Print},
    {"ScrollLock", XK_Scroll_Lock},
    {"Pause", XK_Pause},
    {"Menu", XK_Menu},
    {"Grave", XK_grave},
    {"Minus", XK_minus},
    {"Equal", XK_equal},
    {"BracketLeft", XK_bracketleft},
    {"BracketRight", XK_bracketright},
    {"Backslash", XK_backslash},
    {"Semicolon", XK_semicolon},
    {"Apostrophe", XK_apostrophe},
    {"Comma", XK_comma},
    {"Period", XK_period},
    {"Slash", XK_slash},
    {"KP_Divide", XK_KP_Divide},
    {"KP_Multiply", XK_KP_Multiply},
    {"KP_Subtract", XK_KP_Subtract},
    {"KP_Add", XK_KP_Add},
    {"KP_Enter", XK_KP_Enter},
    {"KP_Decimal", XK_KP_Decimal},
};

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
  return true;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && std::isspace((unsigned char)s.front())) s.remove_prefix(1);
  while (!s.empty() && std::isspace((unsigned char)s.back())) s.remove_suffix(1);
  return s;
}

std::optional<uint32_t> parse_key(std::string_view k) {
  if (k.size() == 1) {
    const unsigned char c = (unsigned char)k[0];
    if (std::isalpha(c)) return XK_a + uint32_t(std::tolower(c) - 'a');
    if (std::isdigit(c)) return XK_0 + uint32_t(c - '0');
  }
  if ((k[0] == 'F' || k[0] == 'f') && k.size() <= 3) {
    int n = 0;
    for (size_t i = 1; i < k.size(); ++i) {
      if (!std::isdigit((unsigned char)k[i])) {
        n = -1;
        break;
      }
      n = n * 10 + (k[i] - '0');
    }
    if (n >= 1 && n <= 24) return XK_F1 + uint32_t(n - 1);
  }
  if (k.size() == 4 && iequals(k.substr(0, 3), "KP_") && std::isdigit((unsigned char)k[3]))
    return XK_KP_0 + uint32_t(k[3] - '0');
  if (k.size() == 6 && iequals(k.substr(0, 5), "Mouse") && k[5] >= '1' && k[5] <= '9')
    return kMouseKeysymBase + uint32_t(k[5] - '0');
  for (const auto &nk : kNamed)
    if (iequals(k, nk.name)) return nk.keysym;
  return std::nullopt;
}

}  // namespace

std::optional<Hotkey> parse_hotkey(std::string_view s) {
  s = trim(s);
  Hotkey h;
  if (s.empty()) return h;
  while (true) {
    const size_t plus = s.find('+', 1);  // "+" isolado não é suportado
    std::string_view part = trim(s.substr(0, plus));
    if (part.empty()) return std::nullopt;
    if (plus == std::string_view::npos) {
      auto key = parse_key(part);
      if (!key) return std::nullopt;
      h.keysym = *key;
      return h;
    }
    if (iequals(part, "Ctrl") || iequals(part, "Control"))
      h.mods |= ipc::kModCtrl;
    else if (iequals(part, "Shift"))
      h.mods |= ipc::kModShift;
    else if (iequals(part, "Alt"))
      h.mods |= ipc::kModAlt;
    else if (iequals(part, "Super") || iequals(part, "Meta") || iequals(part, "Win"))
      h.mods |= ipc::kModSuper;
    else
      return std::nullopt;
    s = s.substr(plus + 1);
  }
}

std::string format_hotkey(const Hotkey &h) {
  if (!h.enabled()) return "";
  std::string out;
  if (h.mods & ipc::kModCtrl) out += "Ctrl+";
  if (h.mods & ipc::kModShift) out += "Shift+";
  if (h.mods & ipc::kModAlt) out += "Alt+";
  if (h.mods & ipc::kModSuper) out += "Super+";
  const uint32_t k = h.keysym;
  char buf[16];
  if (h.is_mouse()) {
    std::snprintf(buf, sizeof buf, "Mouse%d", h.mouse_button());
    out += buf;
  } else if (k >= XK_F1 && k <= XK_F24) {
    std::snprintf(buf, sizeof buf, "F%u", k - XK_F1 + 1);
    out += buf;
  } else if (k >= XK_a && k <= XK_z) {
    out += char('A' + (k - XK_a));
  } else if (k >= XK_0 && k <= XK_9) {
    out += char('0' + (k - XK_0));
  } else if (k >= XK_KP_0 && k <= XK_KP_9) {
    out += "KP_";
    out += char('0' + (k - XK_KP_0));
  } else {
    const char *name = nullptr;
    for (const auto &nk : kNamed)
      if (nk.keysym == k) {
        name = nk.name;
        break;
      }
    if (name)
      out += name;
    else {
      std::snprintf(buf, sizeof buf, "0x%x", k);
      out += buf;
    }
  }
  return out;
}

}  // namespace framux
