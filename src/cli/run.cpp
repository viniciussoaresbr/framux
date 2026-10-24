// framux run [--] <cmd> [args]: exporta FRAMUX=1 e o LD_PRELOAD do shim,
// garante o daemon e executa o jogo (exec, sem processo intermediário).

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cli/cli.h"
#include "common/log.h"
#include "common/paths.h"
#include "framux_build_config.h"

namespace framux::cli {

bool daemon_reachable() {
  const std::string path = socket_path();
  if (path.empty()) return false;
  int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
  const bool ok = connect(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) == 0;
  close(s);
  return ok;
}

namespace {

std::string self_dir() {
  char buf[PATH_MAX];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return {};
  std::string p(buf, size_t(n));
  return p.substr(0, p.rfind('/'));
}

bool exists_exec(const std::string &p) { return access(p.c_str(), X_OK) == 0; }

int run_quiet(const std::vector<std::string> &argv) {
  const pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    const int devnull = open("/dev/null", O_RDWR);
    dup2(devnull, 1);
    dup2(devnull, 2);
    std::vector<char *> a;
    for (const auto &s : argv) a.push_back(const_cast<char *>(s.c_str()));
    a.push_back(nullptr);
    execvp(a[0], a.data());
    _exit(127);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

std::string daemon_binary() {
  if (const char *env = std::getenv("FRAMUX_DAEMON"); env && *env) return env;
  const std::string local = self_dir() + "/framuxd";
  if (exists_exec(local)) return local;
  if (exists_exec(FRAMUX_LIBEXEC_DAEMON)) return FRAMUX_LIBEXEC_DAEMON;
  return "framuxd";
}

// Processo avulso, desacoplado do jogo (setsid + fork duplo). O log vai para
// $XDG_RUNTIME_DIR/framux/framuxd.log.
bool spawn_daemon() {
  const std::string bin = daemon_binary();
  const std::string dir = runtime_dir();
  if (dir.empty()) return false;
  mkdir(dir.c_str(), 0700);
  const pid_t pid = fork();
  if (pid < 0) return false;
  if (pid == 0) {
    setsid();
    if (fork() != 0) _exit(0);
    const int in = open("/dev/null", O_RDONLY);
    const int log = open((dir + "/framuxd.log").c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    dup2(in, 0);
    if (log >= 0) {
      dup2(log, 1);
      dup2(log, 2);
    }
    // O daemon não deve herdar a injeção destinada ao jogo.
    unsetenv("LD_PRELOAD");
    unsetenv("FRAMUX");
    execlp(bin.c_str(), bin.c_str(), nullptr);
    _exit(127);
  }
  int st;
  waitpid(pid, &st, 0);
  return true;
}

bool wait_reachable(int ms) {
  for (int waited = 0; waited < ms; waited += 50) {
    if (daemon_reachable()) return true;
    usleep(50'000);
  }
  return false;
}

}  // namespace

bool ensure_daemon(bool verbose) {
  if (daemon_reachable()) return true;
  if (const char *a = std::getenv("FRAMUX_AUTOSTART"); a && !std::strcmp(a, "0")) return false;
  // Preferir a unidade systemd do usuário (ativação por socket).
  const std::vector<std::string> systemctl{"systemctl", "--user", "start", "framuxd.socket"};
  if (!std::getenv("FRAMUX_DAEMON") && run_quiet(systemctl) == 0 && wait_reachable(1000)) return true;
  if (spawn_daemon() && wait_reachable(2000)) {
    if (verbose) std::fprintf(stderr, "framux: framuxd iniciado\n");
    return true;
  }
  return false;
}

std::string gl_preload_path() {
  if (const char *env = std::getenv("FRAMUX_GL_PRELOAD"); env && *env) return env;
  return std::string(FRAMUX_PRELOAD_DIR) + "/libframux_gl.so";
}

int cmd_run(const std::vector<std::string> &args_in) {
  std::vector<std::string> args = args_in;
  if (!args.empty() && args[0] == "--") args.erase(args.begin());
  if (args.empty()) {
    std::fprintf(stderr,
                 "uso: framux run [--] <comando> [argumentos]\n"
                 "  Steam: framux run %%command%%    Lutris: prefixo \"framux run\"\n");
    return 2;
  }
  if (!ensure_daemon(false))
    std::fprintf(stderr,
                 "framux: aviso: framuxd indisponível; o contador funciona, mas benchmark, screenshot e "
                 "hotkeys globais não (rode `framux doctor`)\n");

  setenv("FRAMUX", "1", 1);
  unsetenv("DISABLE_FRAMUX");
  // Rodar explicitamente vale mesmo para o que o modo automático ignoraria.
  unsetenv("FRAMUX_AUTO");
  const char *no_gl = std::getenv("FRAMUX_NO_GL");
  if (!(no_gl && !std::strcmp(no_gl, "1"))) {
    const std::string shim = gl_preload_path();
    const char *cur = std::getenv("LD_PRELOAD");
    std::string pre = cur ? cur : "";
    if (pre.find(shim) == std::string::npos) {
      // No fim da lista, como o MangoHud: o overlay da Steam também
      // intercepta dlsym e trava processos se o do shim vier antes dele.
      pre = pre.empty() ? shim : pre + ":" + shim;
      setenv("LD_PRELOAD", pre.c_str(), 1);
    }
  }
  std::vector<char *> argv;
  for (auto &a : args) argv.push_back(a.data());
  argv.push_back(nullptr);
  execvp(argv[0], argv.data());
  std::fprintf(stderr, "framux: não consegui executar %s: %s\n", argv[0], std::strerror(errno));
  return 127;
}

}  // namespace framux::cli
