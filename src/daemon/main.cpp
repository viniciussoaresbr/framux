// framuxd: processo de usuário, iniciado pelo systemd --user (ativação por
// socket) ou sob demanda pelo `framux run`.

#include <systemd/sd-daemon.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/flatpak.h"
#include "common/log.h"
#include "daemon/daemon.h"
#include "daemon/recorder.h"
#include "framux_build_config.h"

int main(int argc, char **argv) {
  // O SVT-AV1 escreve o banner e a config no stderr a cada encoder aberto.
  setenv("SVT_LOG", "1", 0);
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--version")) {
      std::printf("framuxd %s\n", FRAMUX_VERSION);
      return 0;
    }
    if (!std::strcmp(argv[i], "--probe-encoders")) {
      // Usado pelo próprio daemon (processo filho): uma chave por linha.
      const framux::daemon::EncoderProbe p = framux::daemon::probe_encoders();
      for (const auto &k : p.available) std::printf("available %s\n", k.c_str());
      if (!p.missing.empty()) std::printf("missing %s\n", p.missing.c_str());
      return 0;
    }
    if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
      std::printf(
          "uso: framuxd [--version] [--probe-encoders]\n\n"
          "Daemon do Framux. Normalmente iniciado pelo systemd --user\n"
          "(framuxd.socket) ou automaticamente por `framux run`.\n\n"
          "--probe-encoders  lista os encoders de vídeo que funcionam nesta máquina\n");
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
