// framux flatpak status|enable|disable: integração com o Steam Flatpak.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cli/cli.h"
#include "common/flatpak.h"

namespace framux::cli {

int cmd_flatpak(const std::vector<std::string> &args) {
  const std::string sub = args.empty() ? "status" : args[0];
  std::string err;
  if (sub == "enable") {
    if (!flatpak::steam_installed()) std::fprintf(stderr, "framux: aviso: Steam Flatpak não encontrado\n");
    // Segue o modo automático da sessão (environment.d → FRAMUX_AUTO=1).
    const char *a = std::getenv("FRAMUX_AUTO");
    const bool auto_mode = (args.size() > 1 && args[1] == "--auto") || (a && !std::strcmp(a, "1"));
    if (!flatpak::enable(auto_mode, &err)) {
      std::fprintf(stderr, "framux: %s\n", err.c_str());
      return 1;
    }
    std::printf("Integração feita. Feche e abra o Steam de novo para valer.\n");
    std::printf("Jogos OpenGL (ex.: Left 4 Dead 2 nativo): %s\n", flatpak::run_command().c_str());
    if (!flatpak::status().layer32)
      std::printf("Sem a camada de 32 bits: instale framux-layer.i686 e rode `framux flatpak enable` de novo.\n");
    return 0;
  }
  if (sub == "disable") {
    if (!flatpak::disable(&err)) {
      std::fprintf(stderr, "framux: %s\n", err.c_str());
      return 1;
    }
    std::printf("Integração desfeita. Feche e abra o Steam de novo.\n");
    return 0;
  }
  if (sub != "status") {
    std::fprintf(stderr, "uso: framux flatpak status|enable [--auto]|disable\n");
    return 2;
  }
  const auto s = flatpak::status(true);
  std::printf("Steam Flatpak: %s\n", s.steam ? "instalado" : "não encontrado");
  std::printf("integração: %s%s\n", s.enabled ? "feita" : "não feita", s.outdated ? " (cópias desatualizadas)" : "");
  if (s.enabled) {
    std::printf("camada 64 bits: %s · 32 bits: %s\n", s.layer64 ? "sim" : "não", s.layer32 ? "sim" : "não");
    std::printf("modo automático no sandbox: %s\n", s.auto_env ? "ligado" : "desligado");
    std::printf("jogos OpenGL: %s\n", flatpak::run_command().c_str());
  }
  return 0;
}

}  // namespace framux::cli
