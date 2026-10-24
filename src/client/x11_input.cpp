#include "client/x11_input.h"

#include <X11/keysym.h>
#include <dlfcn.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include <xcb/xinput.h>

#include <cstdlib>
#include <cstring>

#include "common/ipc_proto.h"

namespace framux::client {

struct X11Input::Fns {
  void *lib = nullptr;
  void *xi_lib = nullptr;
#define FN(name) decltype(&::name) name = nullptr
  FN(xcb_connect);
  FN(xcb_disconnect);
  FN(xcb_connection_has_error);
  FN(xcb_get_setup);
  FN(xcb_setup_roots_iterator);
  FN(xcb_query_keymap);
  FN(xcb_query_keymap_reply);
  FN(xcb_get_keyboard_mapping);
  FN(xcb_get_keyboard_mapping_reply);
  FN(xcb_get_keyboard_mapping_keysyms);
  FN(xcb_intern_atom);
  FN(xcb_intern_atom_reply);
  FN(xcb_get_property);
  FN(xcb_get_property_reply);
  FN(xcb_get_property_value);
  FN(xcb_get_property_value_length);
  FN(xcb_query_tree);
  FN(xcb_query_tree_reply);
  FN(xcb_query_pointer);
  FN(xcb_query_pointer_reply);
  FN(xcb_input_xi_query_pointer);
  FN(xcb_input_xi_query_pointer_reply);
  FN(xcb_input_xi_query_pointer_buttons);
  FN(xcb_input_xi_query_pointer_buttons_length);
#undef FN
  ~Fns() {
    if (xi_lib) dlclose(xi_lib);
    if (lib) dlclose(lib);
  }
};

namespace {
template <class T>
bool sym(void *lib, const char *name, T &out) {
  out = reinterpret_cast<T>(dlsym(lib, name));
  return out != nullptr;
}
}  // namespace

X11Input::~X11Input() {
  if (conn_ && f_) f_->xcb_disconnect(static_cast<xcb_connection_t *>(conn_));
}

std::unique_ptr<X11Input> X11Input::open() {
  const char *display = std::getenv("DISPLAY");
  if (!display || !*display) return nullptr;
  std::unique_ptr<X11Input> in(new X11Input());
  if (!in->load()) return nullptr;
  return in;
}

bool X11Input::load() {
  f_ = std::make_unique<Fns>();
  f_->lib = dlopen("libxcb.so.1", RTLD_LAZY | RTLD_LOCAL);
  if (!f_->lib) return false;
  void *l = f_->lib;
  bool ok =
      sym(l, "xcb_connect", f_->xcb_connect) && sym(l, "xcb_disconnect", f_->xcb_disconnect) &&
      sym(l, "xcb_connection_has_error", f_->xcb_connection_has_error) && sym(l, "xcb_get_setup", f_->xcb_get_setup) &&
      sym(l, "xcb_setup_roots_iterator", f_->xcb_setup_roots_iterator) &&
      sym(l, "xcb_query_keymap", f_->xcb_query_keymap) &&
      sym(l, "xcb_query_keymap_reply", f_->xcb_query_keymap_reply) &&
      sym(l, "xcb_get_keyboard_mapping", f_->xcb_get_keyboard_mapping) &&
      sym(l, "xcb_get_keyboard_mapping_reply", f_->xcb_get_keyboard_mapping_reply) &&
      sym(l, "xcb_get_keyboard_mapping_keysyms", f_->xcb_get_keyboard_mapping_keysyms) &&
      sym(l, "xcb_intern_atom", f_->xcb_intern_atom) && sym(l, "xcb_intern_atom_reply", f_->xcb_intern_atom_reply) &&
      sym(l, "xcb_get_property", f_->xcb_get_property) &&
      sym(l, "xcb_get_property_reply", f_->xcb_get_property_reply) &&
      sym(l, "xcb_get_property_value", f_->xcb_get_property_value) &&
      sym(l, "xcb_get_property_value_length", f_->xcb_get_property_value_length) &&
      sym(l, "xcb_query_tree", f_->xcb_query_tree) && sym(l, "xcb_query_tree_reply", f_->xcb_query_tree_reply) &&
      sym(l, "xcb_query_pointer", f_->xcb_query_pointer) &&
      sym(l, "xcb_query_pointer_reply", f_->xcb_query_pointer_reply);
  if (!ok) return false;

  int screen = 0;
  auto *c = f_->xcb_connect(nullptr, &screen);
  if (!c || f_->xcb_connection_has_error(c)) {
    if (c) f_->xcb_disconnect(c);
    return false;
  }
  conn_ = c;
  const xcb_setup_t *setup = f_->xcb_get_setup(c);
  xcb_screen_iterator_t it = f_->xcb_setup_roots_iterator(setup);
  // Usa o primeiro screen; DISPLAY com screen ≠ 0 é raro em sessões de jogo.
  root_ = it.data->root;
  min_kc_ = setup->min_keycode;
  max_kc_ = setup->max_keycode;

  auto km_cookie = f_->xcb_get_keyboard_mapping(c, min_kc_, uint8_t(max_kc_ - min_kc_ + 1));
  if (auto *km = f_->xcb_get_keyboard_mapping_reply(c, km_cookie, nullptr)) {
    per_kc_ = km->keysyms_per_keycode;
    const xcb_keysym_t *ks = f_->xcb_get_keyboard_mapping_keysyms(km);
    keysyms_.assign(ks, ks + size_t(per_kc_) * size_t(max_kc_ - min_kc_ + 1));
    std::free(km);
  }
  const uint32_t mod_syms[8] = {XK_Control_L, XK_Control_R, XK_Shift_L, XK_Shift_R,
                                XK_Alt_L,     XK_Alt_R,     XK_Super_L, XK_Super_R};
  for (int i = 0; i < 8; ++i) mod_kc_[i] = keycode_for(mod_syms[i]);

  // XInput2 para os botões laterais (8/9), que o núcleo do protocolo não informa.
  f_->xi_lib = dlopen("libxcb-xinput.so.0", RTLD_LAZY | RTLD_LOCAL);
  if (f_->xi_lib) {
    have_xinput_ =
        sym(f_->xi_lib, "xcb_input_xi_query_pointer", f_->xcb_input_xi_query_pointer) &&
        sym(f_->xi_lib, "xcb_input_xi_query_pointer_reply", f_->xcb_input_xi_query_pointer_reply) &&
        sym(f_->xi_lib, "xcb_input_xi_query_pointer_buttons", f_->xcb_input_xi_query_pointer_buttons) &&
        sym(f_->xi_lib, "xcb_input_xi_query_pointer_buttons_length", f_->xcb_input_xi_query_pointer_buttons_length);
  }
  net_active_ = atom("_NET_ACTIVE_WINDOW");
  net_wm_pid_ = atom("_NET_WM_PID");
  return true;
}

uint32_t X11Input::keycode_for(uint32_t keysym) const {
  if (per_kc_ <= 0) return 0;
  for (size_t kc = 0; kc * size_t(per_kc_) < keysyms_.size(); ++kc) {
    // Só as duas primeiras colunas (sem/com Shift) do grupo 1.
    for (int col = 0; col < per_kc_ && col < 2; ++col)
      if (keysyms_[kc * size_t(per_kc_) + size_t(col)] == keysym) return uint32_t(kc + min_kc_);
  }
  return 0;
}

uint32_t X11Input::atom(const char *name) {
  auto *c = static_cast<xcb_connection_t *>(conn_);
  auto cookie = f_->xcb_intern_atom(c, 1, uint16_t(std::strlen(name)), name);
  auto *r = f_->xcb_intern_atom_reply(c, cookie, nullptr);
  if (!r) return 0;
  const uint32_t a = r->atom;
  std::free(r);
  return a;
}

bool X11Input::snapshot() {
  auto *c = static_cast<xcb_connection_t *>(conn_);
  if (f_->xcb_connection_has_error(c)) return false;
  auto kc = f_->xcb_query_keymap(c);
  auto pc = f_->xcb_query_pointer(c, root_);
  xcb_input_xi_query_pointer_cookie_t xc{};
  if (have_xinput_) xc = f_->xcb_input_xi_query_pointer(c, root_, 2 /* ponteiro mestre */);

  bool ok = false;
  std::memcpy(prev_keys_, keys_, sizeof keys_);
  prev_buttons_ = buttons_;
  if (auto *r = f_->xcb_query_keymap_reply(c, kc, nullptr)) {
    std::memcpy(keys_, r->keys, sizeof keys_);
    std::free(r);
    ok = true;
  }
  buttons_ = 0;
  if (auto *r = f_->xcb_query_pointer_reply(c, pc, nullptr)) {
    for (int b = 1; b <= 5; ++b)
      if (r->mask & (XCB_KEY_BUT_MASK_BUTTON_1 << (b - 1))) buttons_ |= 1u << b;
    std::free(r);
  }
  if (have_xinput_) {
    if (auto *r = f_->xcb_input_xi_query_pointer_reply(c, xc, nullptr)) {
      const int n = f_->xcb_input_xi_query_pointer_buttons_length(r);
      const uint32_t *mask = f_->xcb_input_xi_query_pointer_buttons(r);
      if (n > 0) buttons_ |= mask[0] & 0x3fe;  // botões 1..9
      std::free(r);
    }
  }
  auto down = [&](uint32_t code) { return code && (keys_[code / 8] & (1u << (code % 8))); };
  mods_ = 0;
  if (down(mod_kc_[0]) || down(mod_kc_[1])) mods_ |= ipc::kModCtrl;
  if (down(mod_kc_[2]) || down(mod_kc_[3])) mods_ |= ipc::kModShift;
  if (down(mod_kc_[4]) || down(mod_kc_[5])) mods_ |= ipc::kModAlt;
  if (down(mod_kc_[6]) || down(mod_kc_[7])) mods_ |= ipc::kModSuper;
  return ok;
}

namespace {

// Numeração do Windows/FRAPS → botões X: 1 esq., 2 dir., 3 meio, 4/5 laterais.
int x_button(const Hotkey &h) {
  static const int map[10] = {0, 1, 3, 2, 8, 9, 0, 0, 0, 0};
  return map[h.mouse_button()];
}

}  // namespace

bool X11Input::pressed(const Hotkey &h) const {
  if (!h.enabled() || h.mods != mods_) return false;
  if (h.is_mouse()) {
    const int xb = x_button(h);
    return xb && (buttons_ & (1u << xb));
  }
  const uint32_t code = keycode_for(h.keysym);
  return code && (keys_[code / 8] & (1u << (code % 8)));
}

bool X11Input::newly_down(const Hotkey &h) const {
  if (!h.enabled()) return false;
  if (h.is_mouse()) {
    const int xb = x_button(h);
    return xb && !(prev_buttons_ & (1u << xb));
  }
  const uint32_t code = keycode_for(h.keysym);
  return code && !(prev_keys_[code / 8] & (1u << (code % 8)));
}

void X11Input::refresh_ancestors() {
  ancestors_.clear();
  ancestors_valid_ = true;
  auto *c = static_cast<xcb_connection_t *>(conn_);
  uint32_t w = window_;
  for (int depth = 0; w && w != root_ && depth < 16; ++depth) {
    ancestors_.push_back(w);
    auto *r = f_->xcb_query_tree_reply(c, f_->xcb_query_tree(c, w), nullptr);
    if (!r) break;  // janela inexistente: erro isolado nesta requisição
    w = r->parent;
    std::free(r);
  }
}

int X11Input::focus_state() {
  auto *c = static_cast<xcb_connection_t *>(conn_);
  if (!net_active_ || f_->xcb_connection_has_error(c)) return 2;
  auto *r =
      f_->xcb_get_property_reply(c, f_->xcb_get_property(c, 0, root_, net_active_, XCB_ATOM_WINDOW, 0, 1), nullptr);
  if (!r) return 2;
  // Sem a propriedade (servidor sem window manager EWMH) não há como saber.
  const bool has_prop = r->type != XCB_NONE;
  uint32_t active = 0;
  if (f_->xcb_get_property_value_length(r) >= 4) std::memcpy(&active, f_->xcb_get_property_value(r), 4);
  std::free(r);
  if (!has_prop) return 2;
  if (!active) return 0;  // nenhuma janela X ativa (ex.: janela Wayland nativa em foco)

  if (window_) {
    if (!ancestors_valid_) refresh_ancestors();
    for (uint32_t a : ancestors_)
      if (a == active) return 1;
  }
  // Drawables GLX e janelas do Wine: compara o _NET_WM_PID da janela ativa.
  if (net_wm_pid_) {
    auto *p = f_->xcb_get_property_reply(c, f_->xcb_get_property(c, 0, active, net_wm_pid_, XCB_ATOM_CARDINAL, 0, 1),
                                         nullptr);
    if (p) {
      uint32_t pid = 0;
      if (f_->xcb_get_property_value_length(p) >= 4) std::memcpy(&pid, f_->xcb_get_property_value(p), 4);
      std::free(p);
      if (pid) return pid == uint32_t(getpid()) ? 1 : 0;
    }
  }
  return window_ ? 0 : 2;
}

}  // namespace framux::client
