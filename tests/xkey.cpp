// Injeta uma tecla via XTest (como um usuário apertando) para os testes de
// hotkeys in-game. uso: xkey <tecla> (ex.: F11, Shift+F12, Ctrl+Alt+F9)
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  Display *d = XOpenDisplay(nullptr);
  if (!d) return 77;
  // Modificadores antes do último "+", na sintaxe da config.
  std::vector<KeyCode> keys;
  std::string spec = argv[1];
  for (size_t plus; (plus = spec.find('+')) != std::string::npos; spec.erase(0, plus + 1)) {
    const std::string mod = spec.substr(0, plus);
    const char *sym = mod == "Shift" ? "Shift_L" : mod == "Ctrl" ? "Control_L" : mod == "Alt" ? "Alt_L" : "Super_L";
    keys.push_back(XKeysymToKeycode(d, XStringToKeysym(sym)));
  }
  keys.push_back(XKeysymToKeycode(d, XStringToKeysym(spec.c_str())));
  for (KeyCode kc : keys)
    if (!kc) return 2;
  for (KeyCode kc : keys) XTestFakeKeyEvent(d, kc, True, 0);
  XSync(d, False);
  usleep(80'000);  // a camada consulta o teclado a cada 16 ms
  for (auto it = keys.rbegin(); it != keys.rend(); ++it) XTestFakeKeyEvent(d, *it, False, 0);
  XSync(d, False);
  XCloseDisplay(d);
  return 0;
}
