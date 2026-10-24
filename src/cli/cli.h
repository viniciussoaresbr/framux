#pragma once

#include <string>
#include <vector>

namespace framux::cli {

// Socket do framuxd aceitando conexões?
bool daemon_reachable();
// Garante o daemon rodando (systemd --user ou processo avulso). Não fatal.
bool ensure_daemon(bool verbose);

int cmd_run(const std::vector<std::string> &args);
int cmd_doctor(const std::vector<std::string> &args);
int cmd_flatpak(const std::vector<std::string> &args);

// Caminho do shim GL para o LD_PRELOAD (pode conter o token $LIB).
std::string gl_preload_path();

}  // namespace framux::cli
