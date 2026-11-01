// framuxd: processo de usuário, iniciado pelo systemd --user (ativação por
// socket) ou sob demanda pelo `framux run`.

#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#else
#include <systemd/sd-daemon.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#ifndef _WIN32
#include "common/flatpak.h"
#endif
#include "common/log.h"
#include "common/paths.h"
#include "daemon/daemon.h"
#include "daemon/recorder.h"
#include "framux_build_config.h"

#ifdef _WIN32
namespace {
framux::daemon::Daemon *g_daemon = nullptr;

// Ctrl+C no console e fim da sessão do Windows: encerra fechando os arquivos.
BOOL WINAPI on_console(DWORD) {
  if (g_daemon) g_daemon->request_stop();
  return TRUE;
}

// O processo roda como administrador (token elevado)?
bool is_elevated() {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION e{};
  DWORD len = 0;
  const bool ok = GetTokenInformation(token, TokenElevation, &e, sizeof e, &len) && e.TokenIsElevated;
  CloseHandle(token);
  return ok;
}
}  // namespace
#endif

int main(int argc, char **argv) {
  // O SVT-AV1 escreve o banner e a config no stderr a cada encoder aberto.
#ifdef _WIN32
  if (!std::getenv("SVT_LOG")) _putenv_s("SVT_LOG", "1");
#else
  setenv("SVT_LOG", "1", 0);
#endif
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
#ifdef _WIN32
  // Sem console (subsistema GUI): o log vai para %LOCALAPPDATA%/framux/framuxd.log.
  const std::string dir = framux::runtime_dir();
  framux::make_dirs(dir);
  framux::log_to_file((dir + "/framuxd.log").c_str());
  // COM na thread principal (WASAPI, microfone); MTA para os objetos de
  // áudio valerem em qualquer thread.
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  // O escalonador do Windows roda sempre a thread pronta de maior prioridade:
  // com um jogo ocupando todos os núcleos (threads dele acima do normal), as
  // do daemon só rodariam no reforço anti-inanição, a cada ~4 s, e a gravação
  // perderia quase todos os quadros. Acima do normal (como o OBS recomenda)
  // e fora do modo de eficiência (EcoQoS) do Windows 11; o daemon gasta
  // pouca CPU, então quase não tira nada do jogo.
  SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
  PROCESS_POWER_THROTTLING_STATE pt{PROCESS_POWER_THROTTLING_CURRENT_VERSION,
                                    PROCESS_POWER_THROTTLING_EXECUTION_SPEED, 0};
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
  // O mesmo na GPU: o Windows favorece o processo em primeiro plano (o jogo)
  // e o trabalho do encoder de vídeo do daemon esperaria atrás dele. Como o
  // OBS, sobe a prioridade de escalonamento da GPU deste processo.
  // FRAMUX_GPU_PRIORITY=0 desliga (para comparar).
  if (const char *gp = std::getenv("FRAMUX_GPU_PRIORITY"); !(gp && !std::strcmp(gp, "0"))) {
    using SetPrio = LONG(WINAPI *)(HANDLE, int);
    // D3DKMT_SCHEDULINGPRIORITYCLASS: 3 acima do normal, 4 alta, 5 tempo real (só administrador).
    // Com o jogo a 98 % da GPU, "alta" não basta: as cópias da captura
    // esperavam ~400 ms. Como administrador, pede tempo real (o que o OBS faz).
    if (auto set = reinterpret_cast<SetPrio>(
            reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"gdi32.dll"), "D3DKMTSetProcessSchedulingPriorityClass")))) {
      const bool admin = is_elevated();
      static const struct {
        int cls;
        const char *name;
      } levels[] = {{5, "tempo real"}, {4, "alta"}, {3, "acima do normal"}};
      LONG st = -1;
      const char *level = nullptr;
      for (const auto &l : levels) {
        if (l.cls == 5 && !admin) continue;
        if ((st = set(GetCurrentProcess(), l.cls)) >= 0) {
          level = l.name;
          break;
        }
      }
      if (!level)
        FRAMUX_LOG("prioridade de GPU: não consegui subir (0x%08lx)", (unsigned long)st);
      else
        FRAMUX_LOG("prioridade de GPU: %s (%s)", level,
                   admin ? "como administrador" : "sem administrador; \"tempo real\" exige administrador");
    }
  }
  framux::daemon::Daemon d;
  g_daemon = &d;
  SetConsoleCtrlHandler(on_console, TRUE);
  const int rc = d.run(-1);
  g_daemon = nullptr;
  CoUninitialize();
  return rc;
#else
  int listen_fd = -1;
  if (sd_listen_fds(1) == 1) listen_fd = SD_LISTEN_FDS_START;
  // Depois de atualizar o pacote, renova as cópias usadas pelo Steam Flatpak.
  if (std::string err; !framux::flatpak::sync(&err)) FRAMUX_LOG("Steam Flatpak: %s", err.c_str());
  framux::daemon::Daemon d;
  return d.run(listen_fd);
#endif
}
