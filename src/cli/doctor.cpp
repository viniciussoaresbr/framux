// framux doctor: diagnóstico do ambiente (§C.9).

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <sstream>

#include "cli/cli.h"
#include "common/config.h"
#include "common/flatpak.h"
#include "common/hotkey.h"
#include "common/paths.h"
#include "framux_build_config.h"

namespace framux::cli {

namespace {

int g_problems = 0;

#if FRAMUX_DEBIAN
constexpr const char *kInstallLayer32 = "sudo dpkg --add-architecture i386 && sudo apt install framux-layer:i386";
constexpr const char *kInstallLoader = "sudo apt install libvulkan1";
#else
constexpr const char *kInstallLayer32 = "dnf install framux-layer.i686";
constexpr const char *kInstallLoader = "dnf install vulkan-loader";
#endif

void ok(const std::string &s) { std::printf("  \033[32m✓\033[0m %s\n", s.c_str()); }
void warn(const std::string &s) { std::printf("  \033[33m!\033[0m %s\n", s.c_str()); }
void bad(const std::string &s) {
  std::printf("  \033[31m✗\033[0m %s\n", s.c_str());
  ++g_problems;
}
void section(const char *s) { std::printf("\n%s\n", s); }

std::string env(const char *n) {
  const char *v = std::getenv(n);
  return v ? v : "";
}

// 32, 64 ou 0 (não é ELF / não existe).
int elf_class(const std::string &path) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return 0;
  unsigned char id[5] = {};
  const bool r = read(fd, id, 5) == 5;
  close(fd);
  if (!r || std::memcmp(id,
                        "\x7f"
                        "ELF",
                        4) != 0)
    return 0;
  return id[4] == 1 ? 32 : id[4] == 2 ? 64 : 0;
}

std::string read_all(const std::string &p) {
  std::ifstream in(p);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

struct Manifest {
  std::string path, library;
  int bits = 0;
};

std::vector<Manifest> find_manifests() {
  std::vector<std::string> dirs;
  std::stringstream extra(env("VK_ADD_IMPLICIT_LAYER_PATH"));
  for (std::string d; std::getline(extra, d, ':');)
    if (!d.empty()) dirs.push_back(d);
  std::string data_home = env("XDG_DATA_HOME");
  if (data_home.empty()) data_home = home_dir() + "/.local/share";
  dirs.push_back(data_home + "/vulkan/implicit_layer.d");
  dirs.push_back("/etc/vulkan/implicit_layer.d");
  dirs.push_back(std::string(FRAMUX_DATADIR) + "/vulkan/implicit_layer.d");
  if (std::string(FRAMUX_DATADIR) != "/usr/share") dirs.push_back("/usr/share/vulkan/implicit_layer.d");

  // Com prefix ~/.local, FRAMUX_DATADIR coincide com XDG_DATA_HOME.
  std::vector<std::string> unique;
  for (const auto &d : dirs)
    if (std::find(unique.begin(), unique.end(), d) == unique.end()) unique.push_back(d);

  std::vector<Manifest> out;
  const std::regex lib_re("\"library_path\"\\s*:\\s*\"([^\"]+)\"");
  for (const auto &d : unique) {
    DIR *dir = opendir(d.c_str());
    if (!dir) continue;
    while (dirent *e = readdir(dir)) {
      const std::string name = e->d_name;
      if (name.rfind("framux", 0) != 0 || name.size() < 5 || name.compare(name.size() - 5, 5, ".json") != 0) continue;
      Manifest m;
      m.path = d + "/" + name;
      std::smatch sm;
      const std::string text = read_all(m.path);
      if (std::regex_search(text, sm, lib_re)) {
        m.library = sm[1];
        if (!m.library.empty() && m.library[0] != '/') m.library = d + "/" + m.library;
        m.bits = elf_class(m.library);
      }
      out.push_back(m);
    }
    closedir(dir);
  }
  return out;
}

void check_folder(const char *what, const std::string &configured) {
  const std::string p = expand_path(configured);
  std::string probe = p;
  while (!probe.empty() && access(probe.c_str(), F_OK) != 0) probe = probe.substr(0, probe.rfind('/'));
  if (probe.empty()) probe = "/";
  if (access(probe.c_str(), W_OK) == 0)
    ok(std::string(what) + ": " + p + (probe == p ? "" : " (será criada)"));
  else
    bad(std::string(what) + ": sem permissão de escrita em " + probe);
}

}  // namespace

int cmd_doctor(const std::vector<std::string> &) {
  std::printf("framux doctor %s\n", FRAMUX_VERSION);

  section("Sessão");
  const std::string type = env("XDG_SESSION_TYPE"), desktop = env("XDG_CURRENT_DESKTOP");
  ok("tipo: " + (type.empty() ? std::string("desconhecido") : type) +
     (desktop.empty() ? "" : " · ambiente: " + desktop));
  if (!env("DISPLAY").empty())
    ok("DISPLAY=" + env("DISPLAY") + " (X11/XWayland)");
  else
    warn("sem DISPLAY: hotkeys in-game indisponíveis (só funcionam com X11/XWayland)");
  if (runtime_dir().empty())
    bad("XDG_RUNTIME_DIR não definido: o daemon não tem onde criar o socket");
  else
    ok("diretório de runtime: " + runtime_dir());

  section("Daemon");
  if (daemon_reachable())
    ok("framuxd respondendo em " + socket_path());
  else
    warn(
        "framuxd não está rodando (o `framux run` inicia sob demanda; ou: systemctl --user enable --now "
        "framuxd.socket)");

  section("Camada Vulkan");
  bool have64 = false, have32 = false;
  for (const auto &m : find_manifests()) {
    if (m.bits == 64) have64 = true;
    if (m.bits == 32) have32 = true;
    if (m.bits)
      ok(m.path + " → " + m.library + " (" + std::to_string(m.bits) + " bits)");
    else
      bad(m.path + ": biblioteca ausente ou inválida (" + m.library + ")");
  }
  if (!have64) bad("camada x86_64 não encontrada (pacote framux-layer)");
  if (!have32) warn(std::string("camada i686 não encontrada: jogos 32 bits sem contador (") + kInstallLayer32 + ")");
  if (void *h = dlopen("libvulkan.so.1", RTLD_LAZY | RTLD_LOCAL)) {
    ok("loader Vulkan (libvulkan.so.1) presente");
    dlclose(h);
  } else {
    bad(std::string("loader Vulkan ausente (") + kInstallLoader + ")");
  }

  section("Shim OpenGL");
  const std::string shim = gl_preload_path();
  if (shim.find("$LIB") != std::string::npos) {
    // O $LIB vira o diretório de cada arquitetura (lib64/lib no Fedora,
    // lib/x86_64-linux-gnu e lib/i386-linux-gnu no Debian/Ubuntu).
    for (const char *dir : {FRAMUX_LIBDIR64, FRAMUX_LIBDIR32}) {
      const std::string p = std::string(dir) + "/libframux_gl.so";
      const int bits = elf_class(p);
      const bool is64 = std::strcmp(dir, FRAMUX_LIBDIR64) == 0;
      if (bits)
        ok(p + " (" + std::to_string(bits) + " bits)");
      else if (is64)
        bad(p + " ausente");
      else
        warn(p + " ausente: jogos OpenGL 32 bits sem contador");
    }
  } else if (const int bits = elf_class(shim)) {
    ok(shim + " (" + std::to_string(bits) + " bits)");
  } else {
    bad(shim + " ausente");
  }

  if (const auto fp = flatpak::status(true); fp.steam) {
    section("Steam Flatpak");
    if (!fp.enabled) {
      warn("integração não feita: o sandbox não vê a camada nem o daemon (framux flatpak enable)");
    } else {
      ok("camada e shim copiados para " + flatpak::bundle_dir());
      if (fp.outdated) warn("cópias desatualizadas: rode `framux flatpak enable` (ou abra o Framux)");
      if (!fp.layer32)
        warn(fp.source32 ? "cópias de 32 bits ausentes: rode `framux flatpak enable`"
                         : "sem camada de 32 bits: jogos 32 bits (ex.: Left 4 Dead 2) sem contador");
      ok(std::string("modo automático no sandbox: ") + (fp.auto_env ? "ligado" : "desligado"));
      ok("jogos OpenGL: " + flatpak::run_command());
    }
  }

  section("Configuração");
  std::vector<std::string> warnings;
  const Config cfg = load_config("", &warnings);
  const std::string path = main_config_path();
  if (access(path.c_str(), R_OK) == 0)
    ok("arquivo: " + path);
  else
    ok("arquivo: " + path + " (não existe; usando padrões)");
  for (const auto &w : warnings) bad(w);
  check_folder("benchmarks", cfg.benchmark.folder);
  check_folder("screenshots", cfg.screenshot.folder);

  section("Hotkeys");
  const std::string backend = cfg.hotkeys.backend;
  const bool x11 = type == "x11" && !env("DISPLAY").empty();
  if (backend == "portal" || backend == "evdev")
    warn("backend \"" + backend + "\" ainda não implementado; será usada a leitura in-game");
  else if (backend == "x11" || (backend == "auto" && x11))
    ok("XGrabKey (sessão X11): funcionam com qualquer janela em foco; a tecla não chega ao jogo");
  else
    ok("leitura in-game (XWayland/X11): funcionam com o jogo em foco; a tecla continua chegando ao jogo");
  if (type == "wayland")
    warn("Wayland: jogos com janela Wayland nativa (sem XWayland) não recebem hotkeys nesta versão; use a CLI");
  for (const auto &[name, val] : {std::pair{"benchmark", cfg.hotkeys.benchmark},
                                  {"overlay", cfg.hotkeys.overlay},
                                  {"canto do overlay", cfg.hotkeys.overlay_corner},
                                  {"screenshot", cfg.hotkeys.screenshot},
                                  {"video", cfg.hotkeys.video}}) {
    const auto hk = parse_hotkey(val);
    ok(std::string(name) + ": " + (hk && hk->enabled() ? format_hotkey(*hk) : std::string("desativada")));
  }

  section("Vídeo e áudio");
  warn("gravação de vídeo, loop e áudio ainda não fazem parte desta versão");

  std::printf("\n%s\n", g_problems ? "Há problemas acima." : "Tudo certo.");
  return g_problems ? 1 : 0;
}

}  // namespace framux::cli
