// framux run [--] <jogo.exe> [args] no Windows: garante o daemon, põe
// FRAMUX=1 no ambiente do jogo (ativa a camada Vulkan registrada) e espera
// o jogo terminar, devolvendo o código de saída dele.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "cli/cli.h"
#include "common/control_client.h"
#include "common/ipc_socket.h"
#include "common/paths.h"
#include "common/win_setup.h"
#include "common/win_util.h"

namespace framux::cli {

bool daemon_reachable() {
  const int s = ipc::pipe_connect(socket_path(), 100);
  if (s < 0) return false;
  ipc::pipe_close(s);
  return true;
}

bool ensure_daemon(bool verbose) {
  if (daemon_reachable()) return true;
  if (const char *a = std::getenv("FRAMUX_AUTOSTART"); a && !std::strcmp(a, "0")) return false;
  if (!win::spawn_daemon()) return false;
  for (int waited = 0; waited < 3000; waited += 50) {
    if (daemon_reachable()) {
      if (verbose) std::fprintf(stderr, "framux: framuxd iniciado\n");
      return true;
    }
    Sleep(50);
  }
  return false;
}

std::string gl_preload_path() { return {}; }

namespace {

// Regras de aspas do CommandLineToArgvW.
std::wstring quote(const std::wstring &a) {
  if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
  std::wstring out = L"\"";
  size_t bs = 0;
  for (wchar_t c : a) {
    if (c == L'\\') {
      ++bs;
      continue;
    }
    out.append(c == L'"' ? bs * 2 + 1 : bs, L'\\');
    bs = 0;
    out += c;
  }
  out.append(bs * 2, L'\\');
  return out + L"\"";
}

}  // namespace

int cmd_run(const std::vector<std::string> &args_in) {
  std::vector<std::string> args = args_in;
  if (!args.empty() && args[0] == "--") args.erase(args.begin());
  if (args.empty()) {
    std::fprintf(stderr, "uso: framux run [--] <jogo.exe> [args]\n");
    return 2;
  }
  if (!ensure_daemon(true))
    std::fprintf(stderr, "framux: framuxd indisponível; o contador funciona, benchmark e capturas não\n");
  if (!win::layer_registered())
    std::fprintf(stderr, "framux: a camada Vulkan não está registrada (abra o Framux uma vez)\n");

  // Herdado pelo jogo: ativa a camada (enable_environment do manifesto).
  SetEnvironmentVariableW(L"FRAMUX", L"1");
  SetEnvironmentVariableW(L"DISABLE_FRAMUX", nullptr);
  // Rodar explicitamente vale mesmo para o que o modo automático ignoraria.
  SetEnvironmentVariableW(L"FRAMUX_AUTO", nullptr);
  std::wstring cmdline;
  for (const auto &a : args) {
    std::wstring w = win::widen(a);
    // O programa com "/" (vindo de um shell POSIX) não é achado pelo CreateProcess.
    if (cmdline.empty()) std::replace(w.begin(), w.end(), L'/', L'\\');
    cmdline += (cmdline.empty() ? L"" : L" ") + quote(w);
  }
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
    std::fprintf(stderr, "framux: não consegui executar %s: %s\n", args[0].c_str(), win::error_text().c_str());
    return 127;
  }
  CloseHandle(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  return int(code);
}

}  // namespace framux::cli
