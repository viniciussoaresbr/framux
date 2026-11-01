// framux.exe: CLI no Windows. Os subcomandos de ação falam com o framuxd
// pelo canal de controle (common/control_proto.h), no lugar do D-Bus.
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cli/cli.h"
#include "common/config.h"
#include "common/control_client.h"
#include "common/win_setup.h"
#include "common/win_util.h"
#include "framux_build_config.h"

namespace framux::cli {

namespace {

void usage(FILE *f) {
  std::fprintf(f,
               "uso: framux <comando> [opções]\n\n"
               "  run [--] <jogo.exe> [args]    roda um jogo Vulkan com o Framux (Steam: framux run %%command%%)\n"
               "  status [--json]               sessões ativas\n"
               "  bench start|stop|toggle [--duration S]\n"
               "  shot [--overlay]              screenshot do jogo ativo\n"
               "  overlay toggle|next|hide|show mostra/oculta (toggle) ou troca o canto (next)\n"
               "  rec start|stop|toggle         grava o jogo ativo (ou F9 no jogo)\n"
               "  loop on|off|save              (loop recording: ainda não disponível)\n"
               "  stop                          encerra o framuxd (como \"Parar o Framux\")\n"
               "  doctor                        diagnóstico do ambiente\n"
               "  config default|path|check     config padrão, caminho do arquivo, validação\n"
               "  --version\n\n"
               "Sem argumentos, `framux` abre a janela de configuração (framux-gui).\n");
}

int report(const ctl::Reply &r) {
  if (r.unreachable) {
    std::fprintf(stderr, "framux: o framuxd não está rodando (abra o Framux ou rode um jogo com `framux run`)\n");
    return 3;
  }
  if (!r.ok) {
    std::fprintf(stderr, "framux: %s\n", r.text.c_str());
    return r.error_name == "io.github.framux.Error.NoSession" ? 4 : 1;
  }
  if (!r.text.empty()) {
    std::fputs(r.text.c_str(), stdout);
    if (r.text.back() != '\n') std::fputc('\n', stdout);
  }
  return 0;
}

// Espera o FileSaved/Error do pedido `id` (0 = qualquer um do tipo).
bool wait_file(ctl::Subscription &sub, uint64_t id, const std::string &kind, int seconds, std::string *result,
               bool *failed) {
  const ULONGLONG deadline = GetTickCount64() + ULONGLONG(seconds) * 1000;
  while (GetTickCount64() < deadline) {
    bool closed = false;
    const auto ev = sub.next(DWORD(deadline - GetTickCount64()), &closed);
    if (closed) return false;
    if (ev.size() >= 4 && ev[0] == "FileSaved" && ev[2] == kind && (!id || std::strtoull(ev[1].c_str(), nullptr, 10) == id)) {
      *result = ev[3];
      return true;
    }
    if (ev.size() >= 3 && ev[0] == "Error" && id && std::strtoull(ev[1].c_str(), nullptr, 10) == id) {
      *result = ev[2];
      *failed = true;
      return true;
    }
  }
  return false;
}

int cmd_shot(const std::vector<std::string> &args) {
  bool overlay = false;
  for (const auto &a : args) {
    if (a == "--overlay")
      overlay = true;
    else {
      std::fprintf(stderr, "uso: framux shot [--overlay]\n");
      return 2;
    }
  }
  ctl::Subscription sub;
  sub.open();
  const ctl::Reply r = ctl::call({"Screenshot", overlay ? "1" : "0"});
  if (!r.ok) return report(r);
  std::string path;
  bool failed = false;
  if (!wait_file(sub, std::strtoull(r.text.c_str(), nullptr, 10), "screenshot", 10, &path, &failed)) {
    std::fprintf(stderr, "framux: captura solicitada, mas o arquivo não foi confirmado em 10 s\n");
    return 1;
  }
  if (failed) {
    std::fprintf(stderr, "framux: screenshot falhou: %s\n", path.c_str());
    return 1;
  }
  std::printf("%s\n", path.c_str());
  return 0;
}

int cmd_bench(const std::vector<std::string> &args) {
  if (args.empty() || (args[0] != "start" && args[0] != "stop" && args[0] != "toggle")) {
    std::fprintf(stderr, "uso: framux bench start|stop|toggle [--duration S]\n");
    return 2;
  }
  uint32_t duration = 0;
  for (size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "--duration" && i + 1 < args.size())
      duration = uint32_t(std::strtoul(args[++i].c_str(), nullptr, 10));
    else {
      std::fprintf(stderr, "uso: framux bench start|stop|toggle [--duration S]\n");
      return 2;
    }
  }
  ctl::Subscription sub;
  sub.open();
  const ctl::Reply r = ctl::call({"Benchmark", args[0], std::to_string(duration)});
  const int rc = report(r);
  if (rc == 0 && r.text.find("encerrado") != std::string::npos) {
    std::string path;
    bool failed = false;
    if (wait_file(sub, 0, "benchmark", 15, &path, &failed) && !failed) std::printf("%s\n", path.c_str());
  }
  return rc;
}

int cmd_config(const std::vector<std::string> &args) {
  const std::string what = args.empty() ? "path" : args[0];
  if (what == "default") {
    std::fputs(config_to_toml(Config{}).c_str(), stdout);
    return 0;
  }
  if (what == "path") {
    std::printf("%s\n", main_config_path().c_str());
    return 0;
  }
  if (what == "check") {
    std::vector<std::string> warnings;
    load_config(args.size() > 1 ? args[1] : "", &warnings);
    for (const auto &w : warnings) std::fprintf(stderr, "%s\n", w.c_str());
    if (warnings.empty()) std::printf("ok\n");
    return warnings.empty() ? 0 : 1;
  }
  std::fprintf(stderr, "uso: framux config default|path|check [jogo]\n");
  return 2;
}

}  // namespace
}  // namespace framux::cli

int main(int argc, char **argv) {
  using namespace framux::cli;
  namespace ctl = framux::ctl;
  // Saída em UTF-8 no console (o manifesto já põe a página de código do processo em UTF-8).
  SetConsoleOutputCP(CP_UTF8);
  if (argc < 2) {
    // Sem argumentos abre a GUI, instalada ao lado da CLI.
    const std::wstring gui = framux::win::widen(framux::win::sibling("framux-gui.exe"));
    if (reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", gui.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32)
      return 0;
    usage(stdout);
    return 0;
  }
  const std::string cmd = argv[1];
  std::vector<std::string> args(argv + 2, argv + argc);

  if (cmd == "--version" || cmd == "version") {
    std::printf("framux %s\n", FRAMUX_VERSION);
    return 0;
  }
  if (cmd == "--help" || cmd == "-h" || cmd == "help") {
    usage(stdout);
    return 0;
  }
  if (cmd == "run") return cmd_run(args);
  if (cmd == "doctor") return cmd_doctor(args);
  if (cmd == "config") return cmd_config(args);
  if (cmd == "status") {
    const bool json = !args.empty() && args[0] == "--json";
    return report(ctl::call({json ? "StatusJson" : "Status"}));
  }
  if (cmd == "stop") return report(ctl::call({"Quit"}));
  if (cmd == "bench") return cmd_bench(args);
  if (cmd == "shot") return cmd_shot(args);
  if (cmd == "overlay") {
    if (args.size() != 1) {
      std::fprintf(stderr, "uso: framux overlay toggle|next|hide|show\n");
      return 2;
    }
    return report(ctl::call({"Overlay", args[0]}));
  }
  if (cmd == "rec" || cmd == "loop") {
    if (args.size() != 1) {
      std::fprintf(stderr, "uso: framux %s\n", cmd == "rec" ? "rec start|stop|toggle" : "loop on|off|save");
      return 2;
    }
    return report(ctl::call({cmd == "rec" ? "Record" : "Loop", args[0]}));
  }
  std::fprintf(stderr, "framux: comando desconhecido: %s\n\n", cmd.c_str());
  usage(stderr);
  return 2;
}
