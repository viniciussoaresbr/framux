// framuxd: processo de usuário, iniciado pelo systemd --user (ativação por
// socket) ou sob demanda pelo `framux run`.

#include <systemd/sd-daemon.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "common/flatpak.h"
#include "common/log.h"
#include "daemon/daemon.h"
#include "framux_build_config.h"

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--version")) {
      std::printf("framuxd %s\n", FRAMUX_VERSION);
      return 0;
    }
    if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
      std::printf(
          "uso: framuxd [--version]\n\n"
          "Daemon do Framux. Normalmente iniciado pelo systemd --user\n"
          "(framuxd.socket) ou automaticamente por `framux run`.\n");
      return 0;
    }
    std::fprintf(stderr, "framuxd: opção desconhecida: %s\n", argv[i]);
    return 2;
  }
  int listen_fd = -1;
  if (sd_listen_fds(1) == 1) listen_fd = SD_LISTEN_FDS_START;
  // Depois de atualizar o pacote, renova as cópias usadas pelo Steam Flatpak.
  if (std::string err; !framux::flatpak::sync(&err)) FRAMUX_LOG("Steam Flatpak: %s", err.c_str());
  framux::daemon::Daemon d;
  return d.run(listen_fd);
}
