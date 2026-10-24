#include "daemon/x11_hotkeys.h"

#include <X11/XKBlib.h>
#include <X11/Xlib.h>

#include "common/ipc_proto.h"
#include "common/log.h"

namespace framux::daemon {

struct X11Hotkeys::Impl {
  Display *dpy = nullptr;
  Window root = 0;
  Callback cb;
  std::vector<Hotkey> keys;
};

namespace {

// Grab de uma tecla já capturada por outro cliente gera BadAccess; isso é
// só um aviso, não motivo para encerrar o daemon.
int on_x_error(Display *, XErrorEvent *e) {
  FRAMUX_LOG("X11: erro %d na requisição %d (hotkey já usada por outro programa?)", e->error_code, e->request_code);
  return 0;
}

unsigned x_mods(uint32_t mods) {
  unsigned m = 0;
  if (mods & ipc::kModCtrl) m |= ControlMask;
  if (mods & ipc::kModShift) m |= ShiftMask;
  if (mods & ipc::kModAlt) m |= Mod1Mask;
  if (mods & ipc::kModSuper) m |= Mod4Mask;
  return m;
}

uint32_t our_mods(unsigned state) {
  uint32_t m = 0;
  if (state & ControlMask) m |= ipc::kModCtrl;
  if (state & ShiftMask) m |= ipc::kModShift;
  if (state & Mod1Mask) m |= ipc::kModAlt;
  if (state & Mod4Mask) m |= ipc::kModSuper;
  return m;
}

// Botões no estilo Windows/FRAPS → botões X.
unsigned x_button(int mouse) {
  static const unsigned map[10] = {0, 1, 3, 2, 8, 9, 0, 0, 0, 0};
  return mouse > 0 && mouse < 10 ? map[mouse] : 0;
}

// NumLock (Mod2) e CapsLock não devem impedir a hotkey.
const unsigned kIgnored[] = {0, LockMask, Mod2Mask, LockMask | Mod2Mask};

}  // namespace

X11Hotkeys::~X11Hotkeys() {
  if (impl_ && impl_->dpy) {
    ungrab_all();
    XCloseDisplay(impl_->dpy);
  }
}

std::unique_ptr<X11Hotkeys> X11Hotkeys::open(Callback cb) {
  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy) return nullptr;
  XSetErrorHandler(on_x_error);
  std::unique_ptr<X11Hotkeys> h(new X11Hotkeys());
  h->impl_ = std::make_unique<Impl>();
  h->impl_->dpy = dpy;
  h->impl_->root = DefaultRootWindow(dpy);
  h->impl_->cb = std::move(cb);
  // Sem auto-repeat sintético: KeyRelease só quando a tecla é solta.
  Bool supported = False;
  XkbSetDetectableAutoRepeat(dpy, True, &supported);
  XSelectInput(dpy, h->impl_->root, KeyPressMask | KeyReleaseMask);
  return h;
}

int X11Hotkeys::fd() const { return ConnectionNumber(impl_->dpy); }

void X11Hotkeys::ungrab_all() {
  Impl &d = *impl_;
  for (const Hotkey &k : d.keys) {
    for (unsigned ign : kIgnored) {
      if (k.is_mouse()) {
        if (unsigned b = x_button(k.mouse_button())) XUngrabButton(d.dpy, b, x_mods(k.mods) | ign, d.root);
      } else if (KeyCode kc = XKeysymToKeycode(d.dpy, k.keysym)) {
        XUngrabKey(d.dpy, kc, x_mods(k.mods) | ign, d.root);
      }
    }
  }
  XFlush(d.dpy);
}

void X11Hotkeys::set_bindings(const std::vector<Hotkey> &keys) {
  ungrab_all();
  Impl &d = *impl_;
  d.keys.clear();
  for (const Hotkey &k : keys) {
    if (!k.enabled()) continue;
    d.keys.push_back(k);
    for (unsigned ign : kIgnored) {
      if (k.is_mouse()) {
        if (unsigned b = x_button(k.mouse_button()))
          XGrabButton(d.dpy, b, x_mods(k.mods) | ign, d.root, False, ButtonPressMask | ButtonReleaseMask, GrabModeAsync,
                      GrabModeAsync, None, None);
      } else if (KeyCode kc = XKeysymToKeycode(d.dpy, k.keysym)) {
        XGrabKey(d.dpy, kc, x_mods(k.mods) | ign, d.root, False, GrabModeAsync, GrabModeAsync);
      }
    }
  }
  XFlush(d.dpy);
}

void X11Hotkeys::dispatch() {
  Impl &d = *impl_;
  while (XPending(d.dpy)) {
    XEvent ev;
    XNextEvent(d.dpy, &ev);
    if (ev.type == KeyPress || ev.type == KeyRelease) {
      const KeySym sym = XkbKeycodeToKeysym(d.dpy, KeyCode(ev.xkey.keycode), 0, 0);
      d.cb(uint32_t(sym), our_mods(ev.xkey.state), ev.type == KeyPress);
    } else if (ev.type == ButtonPress || ev.type == ButtonRelease) {
      for (int m = 1; m < 10; ++m) {
        if (x_button(m) == ev.xbutton.button) {
          d.cb(kMouseKeysymBase + uint32_t(m), our_mods(ev.xbutton.state), ev.type == ButtonPress);
          break;
        }
      }
    }
  }
}

}  // namespace framux::daemon
