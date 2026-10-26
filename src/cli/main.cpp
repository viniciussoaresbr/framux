// framux: CLI (§C.9). Os subcomandos de ação falam com o framuxd via D-Bus.

#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>

#include <cstdarg>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "cli/cli.h"
#include "common/config.h"
#include "core/bench_files.h"
#include "framux_build_config.h"

namespace framux::cli {

namespace {

constexpr const char *kBus = "io.github.framux.Daemon1";
constexpr const char *kPath = "/io/github/framux/Daemon1";
constexpr const char *kIface = "io.github.framux.Daemon1";

void usage(FILE *f) {
  std::fprintf(f,
               "uso: framux <comando> [opções]\n\n"
               "  run [--] <cmd> [args]         roda um jogo com o Framux (Steam: framux run %%command%%)\n"
               "  status [--json]               sessões ativas e backend de hotkeys\n"
               "  bench start|stop|toggle [--duration S]\n"
               "  shot [--overlay]              screenshot do jogo ativo\n"
               "  overlay toggle|next|hide|show mostra/oculta (toggle) ou troca o canto (next)\n"
               "  rec start|stop|toggle         grava o jogo ativo (ou F9 no jogo)\n"
               "  loop on|off|save              (loop recording: ainda não disponível)\n"
               "  doctor                        diagnóstico do ambiente\n"
               "  flatpak status|enable|disable integração com o Steam Flatpak\n"
               "  config default|path|check     config padrão, caminho do arquivo, validação\n"
               "  --version\n\n"
               "Sem argumentos, `framux` abre a janela de configuração (framux-gui).\n");
}

struct Bus {
  sd_bus *bus = nullptr;
  ~Bus() { sd_bus_flush_close_unref(bus); }
};

bool name_has_owner(sd_bus *bus) {
  sd_bus_message *reply = nullptr;
  int has = 0;
  if (sd_bus_call_method(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
                         nullptr, &reply, "s", kBus) >= 0)
    sd_bus_message_read(reply, "b", &has);
  sd_bus_message_unref(reply);
  return has != 0;
}

bool open_bus(Bus &b) {
  const int r = sd_bus_open_user(&b.bus);
  if (r < 0) {
    std::fprintf(stderr, "framux: D-Bus de sessão indisponível: %s\n", strerror(-r));
    return false;
  }
  // Com ativação por socket o framuxd só sobe quando alguém conecta ao
  // ipc.sock; a conexão de teste o acorda e esperamos o nome aparecer.
  if (!name_has_owner(b.bus) && daemon_reachable()) {
    for (int waited = 0; waited < 2000 && !name_has_owner(b.bus); waited += 50) {
      const timespec ts{0, 50'000'000};
      nanosleep(&ts, nullptr);
    }
  }
  return true;
}

int report_error(const sd_bus_error &e, int r) {
  if (sd_bus_error_has_name(&e, "org.freedesktop.DBus.Error.ServiceUnknown") ||
      sd_bus_error_has_name(&e, "org.freedesktop.DBus.Error.NameHasNoOwner")) {
    std::fprintf(stderr,
                 "framux: o framuxd não está rodando (inicie um jogo com `framux run` ou rode "
                 "`systemctl --user start framuxd.socket`)\n");
    return 3;
  }
  std::fprintf(stderr, "framux: %s\n", e.message ? e.message : strerror(-r));
  return sd_bus_error_has_name(&e, "io.github.framux.Error.NoSession") ? 4 : 1;
}

// Chama um método que devolve "s" e imprime a resposta.
int call_s(const char *method, const char *sig, ...) {
  Bus b;
  if (!open_bus(b)) return 1;
  sd_bus_message *m = nullptr, *reply = nullptr;
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message_new_method_call(b.bus, &m, kBus, kPath, kIface, method);
  if (sig && *sig) {
    va_list ap;
    va_start(ap, sig);
    sd_bus_message_appendv(m, sig, ap);
    va_end(ap);
  }
  int r = sd_bus_call(b.bus, m, 5'000'000, &err, &reply);
  sd_bus_message_unref(m);
  if (r < 0) {
    const int rc = report_error(err, r);
    sd_bus_error_free(&err);
    return rc;
  }
  const char *text = nullptr;
  if (sd_bus_message_read(reply, "s", &text) >= 0 && text && *text) {
    std::fputs(text, stdout);
    if (text[std::strlen(text) - 1] != '\n') std::fputc('\n', stdout);
  }
  sd_bus_message_unref(reply);
  return 0;
}

struct Waiter {
  uint64_t request_id = 0;
  std::string kind;
  bool done = false;
  bool failed = false;
  std::string result;
};

int on_file_saved(sd_bus_message *m, void *ud, sd_bus_error *) {
  auto *w = static_cast<Waiter *>(ud);
  uint64_t id = 0;
  const char *kind = nullptr, *path = nullptr;
  if (sd_bus_message_read(m, "tss", &id, &kind, &path) < 0) return 0;
  if (id == w->request_id && w->kind == kind) {
    w->done = true;
    w->result = path;
  }
  return 0;
}

int on_error(sd_bus_message *m, void *ud, sd_bus_error *) {
  auto *w = static_cast<Waiter *>(ud);
  uint64_t id = 0;
  const char *msg = nullptr;
  if (sd_bus_message_read(m, "ts", &id, &msg) < 0) return 0;
  if (id && id == w->request_id) {
    w->done = w->failed = true;
    w->result = msg;
  }
  return 0;
}

uint64_t mono_us() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000 + uint64_t(ts.tv_nsec) / 1000;
}

void wait_for(Bus &b, Waiter &w, int seconds) {
  const uint64_t deadline = mono_us() + uint64_t(seconds) * 1000000;
  while (!w.done && mono_us() < deadline) {
    const int r = sd_bus_process(b.bus, nullptr);
    if (r > 0) continue;
    if (r < 0) break;
    sd_bus_wait(b.bus, deadline - mono_us());
  }
}

void subscribe(Bus &b, Waiter &w) {
  sd_bus_match_signal(b.bus, nullptr, kBus, kPath, kIface, "FileSaved", on_file_saved, &w);
  sd_bus_match_signal(b.bus, nullptr, kBus, kPath, kIface, "Error", on_error, &w);
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
  Bus b;
  if (!open_bus(b)) return 1;
  Waiter w;
  w.kind = "screenshot";
  subscribe(b, w);
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = nullptr;
  int r = sd_bus_call_method(b.bus, kBus, kPath, kIface, "Screenshot", &err, &reply, "b", overlay);
  if (r < 0) {
    const int rc = report_error(err, r);
    sd_bus_error_free(&err);
    return rc;
  }
  sd_bus_message_read(reply, "t", &w.request_id);
  sd_bus_message_unref(reply);
  wait_for(b, w, 10);
  if (!w.done) {
    std::fprintf(stderr, "framux: captura solicitada, mas o arquivo não foi confirmado em 10 s\n");
    return 1;
  }
  if (w.failed) {
    std::fprintf(stderr, "framux: screenshot falhou: %s\n", w.result.c_str());
    return 1;
  }
  std::printf("%s\n", w.result.c_str());
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
  Bus b;
  if (!open_bus(b)) return 1;
  Waiter w;
  w.kind = "benchmark";
  subscribe(b, w);
  sd_bus_error err = SD_BUS_ERROR_NULL;
  sd_bus_message *reply = nullptr;
  int r = sd_bus_call_method(b.bus, kBus, kPath, kIface, "Benchmark", &err, &reply, "su", args[0].c_str(), duration);
  if (r < 0) {
    const int rc = report_error(err, r);
    sd_bus_error_free(&err);
    return rc;
  }
  const char *msg = nullptr;
  sd_bus_message_read(reply, "s", &msg);
  std::printf("%s\n", msg ? msg : "");
  const bool stopped = msg && std::strstr(msg, "encerrado");
  sd_bus_message_unref(reply);
  if (stopped) {
    wait_for(b, w, 15);
    if (w.done && !w.failed) std::printf("%s\n", w.result.c_str());
  }
  return 0;
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
    const std::string game = args.size() > 1 ? args[1] : "";
    load_config(game, &warnings);
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
  if (argc < 2) {
    // Sem argumentos abre a GUI (§C.9), instalada ao lado da CLI.
    const std::string gui = std::string(FRAMUX_BINDIR) + "/framux-gui";
    execl(gui.c_str(), gui.c_str(), nullptr);
    execlp("framux-gui", "framux-gui", nullptr);
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
  if (cmd == "flatpak") return cmd_flatpak(args);
  if (cmd == "config") return cmd_config(args);
  if (cmd == "status") {
    const bool json = !args.empty() && args[0] == "--json";
    return call_s(json ? "StatusJson" : "Status", "");
  }
  if (cmd == "bench") return cmd_bench(args);
  if (cmd == "shot") return cmd_shot(args);
  if (cmd == "overlay") {
    if (args.size() != 1) {
      std::fprintf(stderr, "uso: framux overlay toggle|next|hide|show\n");
      return 2;
    }
    return call_s("Overlay", "s", args[0].c_str());
  }
  if (cmd == "rec" || cmd == "loop") {
    if (args.size() != 1) {
      std::fprintf(stderr, "uso: framux %s\n", cmd == "rec" ? "rec start|stop|toggle" : "loop on|off|save");
      return 2;
    }
    return call_s(cmd == "rec" ? "Record" : "Loop", "s", args[0].c_str());
  }
  std::fprintf(stderr, "framux: comando desconhecido: %s\n\n", cmd.c_str());
  usage(stderr);
  return 2;
}
