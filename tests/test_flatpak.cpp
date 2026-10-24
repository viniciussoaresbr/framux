#include <catch2/catch.hpp>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "common/flatpak.h"
#include "common/paths.h"

using namespace framux;

namespace {

void put(const std::string &path, const std::string &text) {
  make_dirs(path.substr(0, path.rfind('/')));
  std::ofstream(path, std::ios::binary) << text;
}

std::string get(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

bool exists(const std::string &p) {
  struct stat st{};
  return lstat(p.c_str(), &st) == 0;
}

}  // namespace

TEST_CASE("Steam Flatpak: integração e remoção", "[flatpak]") {
  char tmpl[] = "/tmp/framux-flatpak-XXXXXX";
  REQUIRE(mkdtemp(tmpl));
  const std::string t = tmpl;
  setenv("HOME", t.c_str(), 1);
  setenv("XDG_DATA_HOME", (t + "/data").c_str(), 1);
  setenv("XDG_CONFIG_HOME", (t + "/cfg").c_str(), 1);
  unsetenv("FRAMUX_CONFIG");
  setenv("FRAMUX_LIBDIR64", (t + "/src64").c_str(), 1);
  setenv("FRAMUX_LIBDIR32", (t + "/src32").c_str(), 1);
  setenv("FRAMUX_DATADIR", (t + "/share").c_str(), 1);

  const std::string manifest =
      "{\n  \"layer\": {\n    \"name\": \"VK_LAYER_FRAMUX_overlay_@\",\n"
      "    \"library_path\": \"/usr/lib64/framux/libframux_vk.so\",\n    \"type\": \"GLOBAL\"\n  }\n}\n";
  put(t + "/src64/libframux_vk.so", "vk64");
  put(t + "/src64/libframux_gl.so", "gl64");
  put(t + "/share/vulkan/implicit_layer.d/framux_x86_64.json", manifest);

  // Override do usuário com coisas que não são nossas: precisam sobreviver.
  const std::string user_override = "[Context]\nfilesystems=xdg-config/MangoHud:ro;\n\n[Environment]\nMANGOHUD=1\n";
  put(flatpak::overrides_path(), user_override);

  std::string err;
  REQUIRE(flatpak::enable(true, &err));
  const std::string b = flatpak::bundle_dir();
  REQUIRE(get(b + "/lib64/libframux_vk.so") == "vk64");
  REQUIRE(get(b + "/lib64/libframux_gl.so") == "gl64");
  const std::string copied = get(flatpak::steam_home() + "/.local/share/vulkan/implicit_layer.d/framux_x86_64.json");
  REQUIRE(copied.find("\"library_path\": \"" + b + "/lib64/libframux_vk.so\"") != std::string::npos);
  REQUIRE(copied.find("VK_LAYER_FRAMUX_overlay_@") != std::string::npos);
  // Caminhos explícitos por arquitetura, como o Steam passa o próprio overlay.
  const std::string run = get(b + "/bin/framux-run");
  REQUIRE(run.find("\"$d/lib/libframux_gl.so\" \"$d/lib64/libframux_gl.so\"") != std::string::npos);
  REQUIRE(run.find("$LIB") == std::string::npos);
  REQUIRE(access((b + "/bin/framux-run").c_str(), X_OK) == 0);

  const std::string ov = get(flatpak::overrides_path());
  REQUIRE(ov.find("filesystems=xdg-config/MangoHud:ro;xdg-run/framux:create;xdg-config/framux:ro;") !=
          std::string::npos);
  REQUIRE(ov.find("MANGOHUD=1") != std::string::npos);
  REQUIRE(ov.find("FRAMUX_AUTO=1") != std::string::npos);
  REQUIRE(ov.find("FRAMUX_CONFIG=" + t + "/cfg/framux/config.toml") != std::string::npos);

  auto s = flatpak::status(true);
  REQUIRE(s.enabled);
  REQUIRE(s.layer64);
  REQUIRE_FALSE(s.layer32);
  REQUIRE(s.auto_env);
  REQUIRE_FALSE(s.outdated);

  // framux-layer.i686 instalado depois: aparece como desatualizado e o sync copia.
  put(t + "/src32/libframux_vk.so", "vk32");
  put(t + "/src32/libframux_gl.so", "gl32");
  put(t + "/share/vulkan/implicit_layer.d/framux_i686.json", manifest);
  REQUIRE(flatpak::status(true).outdated);
  REQUIRE(flatpak::sync(&err));
  s = flatpak::status(true);
  REQUIRE(s.layer32);
  REQUIRE_FALSE(s.outdated);
  REQUIRE(s.auto_env);  // o sync mantém o modo automático
  REQUIRE(get(b + "/lib/libframux_gl.so") == "gl32");

  // Pacote atualizado: cópia difere até o próximo sync.
  put(t + "/src64/libframux_vk.so", "vk64-novo");
  REQUIRE(flatpak::status(true).outdated);
  REQUIRE(flatpak::sync(&err));
  REQUIRE(get(b + "/lib64/libframux_vk.so") == "vk64-novo");

  REQUIRE(flatpak::set_auto(false, &err));
  REQUIRE_FALSE(flatpak::status().auto_env);
  REQUIRE(get(flatpak::overrides_path()).find("FRAMUX_CONFIG=") != std::string::npos);

  // Desfazer devolve o override do usuário intacto e apaga as cópias.
  REQUIRE(flatpak::disable(&err));
  REQUIRE(get(flatpak::overrides_path()) == user_override);
  REQUIRE_FALSE(exists(b));
  REQUIRE_FALSE(flatpak::status().enabled);
  // Sem integração, sync e set_auto não criam nada.
  REQUIRE(flatpak::sync(&err));
  REQUIRE(flatpak::set_auto(true, &err));
  REQUIRE_FALSE(exists(b));
  REQUIRE(get(flatpak::overrides_path()) == user_override);

  for (const char *v : {"FRAMUX_LIBDIR64", "FRAMUX_LIBDIR32", "FRAMUX_DATADIR", "XDG_DATA_HOME", "XDG_CONFIG_HOME"})
    unsetenv(v);
  REQUIRE(std::system(("rm -rf '" + t + "'").c_str()) == 0);
}
