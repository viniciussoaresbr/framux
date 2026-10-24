#pragma once

#include <string>
#include <vector>

namespace framux {

struct ProcessIdentity {
  int pid = 0;
  std::string exe;          // /proc/self/exe
  std::string exe_base;     // basename de exe
  std::string comm;         // /proc/self/comm
  std::string game;         // nome usado nos arquivos (§C.7.1)
  std::string config_name;  // nome do override por jogo (§C.6): "wine-<exe>" no Wine
  bool wine = false;
};

// Deriva a identidade a partir do executável e da linha de comando.
// No Wine, o jogo é o primeiro argumento terminado em ".exe".
ProcessIdentity identity_from(const std::string &exe, const std::string &comm, const std::vector<std::string> &argv);
ProcessIdentity current_identity();

}  // namespace framux
