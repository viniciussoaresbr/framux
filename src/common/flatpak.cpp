#include "common/flatpak.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include "common/config.h"
#include "common/paths.h"
#include "framux_build_config.h"

namespace framux::flatpak {

namespace {

// Entradas que a integração põe no override; removidas pelo prefixo.
constexpr const char *kRunFs = "xdg-run/framux";
constexpr const char *kConfigFs = "xdg-config/framux";

struct Arch {
  const char *name;     // sufixo do manifesto
  const char *src;      // diretório instalado
  const char *src_env;  // variável que troca `src` (testes)
  const char *subdir;   // diretório da cópia em bundle_dir()
};
const Arch kArchs[] = {{"x86_64", FRAMUX_LIBDIR64, "FRAMUX_LIBDIR64", "lib64"},
                       {"i686", FRAMUX_LIBDIR32, "FRAMUX_LIBDIR32", "lib"}};
const char *kLibs[] = {"libframux_vk.so", "libframux_gl.so"};

std::string env_or(const char *name, const char *fallback) {
  const char *v = std::getenv(name);
  return v && *v ? v : fallback;
}
std::string source_dir(const Arch &a) { return env_or(a.src_env, a.src); }

std::string data_home() {
  const char *v = std::getenv("XDG_DATA_HOME");
  if (v && v[0] == '/') return v;
  return home_dir() + "/.local/share";
}

std::string layer_dir() { return steam_home() + "/.local/share/vulkan/implicit_layer.d"; }
std::string manifest_path(const Arch &a) { return layer_dir() + "/framux_" + a.name + ".json"; }
std::string source_manifest(const Arch &a) {
  return env_or("FRAMUX_DATADIR", FRAMUX_DATADIR) + "/vulkan/implicit_layer.d/framux_" + a.name + ".json";
}

bool exists(const std::string &p) {
  struct stat st{};
  return lstat(p.c_str(), &st) == 0;
}

bool read_file(const std::string &p, std::string *out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  *out = ss.str();
  return true;
}

bool fail(std::string *err, const std::string &msg) {
  if (err) *err = msg;
  return false;
}

// Grava por arquivo temporário + rename: um jogo aberto continua com o .so
// antigo mapeado em vez de ver o arquivo mudar por baixo.
bool write_file(const std::string &p, const std::string &data, mode_t mode, std::string *err) {
  const std::string tmp = p + ".tmp";
  FILE *f = std::fopen(tmp.c_str(), "wb");
  if (!f) return fail(err, tmp + ": " + std::strerror(errno));
  const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  if (std::fclose(f) != 0 || !ok) {
    unlink(tmp.c_str());
    return fail(err, tmp + ": falha ao gravar");
  }
  chmod(tmp.c_str(), mode);
  if (rename(tmp.c_str(), p.c_str()) != 0) {
    unlink(tmp.c_str());
    return fail(err, p + ": " + std::strerror(errno));
  }
  return true;
}

// Copia só se o conteúdo mudou. `changed` diz se houve cópia.
bool copy_if_changed(const std::string &src, const std::string &dst, std::string *err) {
  std::string a, b;
  if (!read_file(src, &a)) return fail(err, src + ": " + std::strerror(errno));
  if (read_file(dst, &b) && a == b) return true;
  return write_file(dst, a, 0755, err);
}

// Manifesto instalado com library_path apontando para a cópia.
bool manifest_for(const Arch &a, std::string *out, std::string *err) {
  std::string text;
  if (!read_file(source_manifest(a), &text)) return fail(err, source_manifest(a) + ": ausente");
  const std::string key = "\"library_path\"";
  size_t k = text.find(key);
  size_t q1 = k == std::string::npos ? k : text.find('"', k + key.size());
  size_t q2 = q1 == std::string::npos ? q1 : text.find('"', q1 + 1);
  if (q2 == std::string::npos) return fail(err, source_manifest(a) + ": sem library_path");
  const std::string lib = bundle_dir() + "/" + a.subdir + "/libframux_vk.so";
  *out = text.substr(0, q1 + 1) + lib + text.substr(q2);
  return true;
}

bool source_present(const Arch &a) {
  for (const char *lib : kLibs)
    if (!exists(source_dir(a) + "/" + lib)) return false;
  return exists(source_manifest(a));
}

bool copies_present(const Arch &a) {
  for (const char *lib : kLibs)
    if (!exists(bundle_dir() + "/" + a.subdir + "/" + lib)) return false;
  return exists(manifest_path(a));
}

bool copies_match(const Arch &a) {
  for (const char *lib : kLibs) {
    std::string x, y;
    if (!read_file(source_dir(a) + "/" + lib, &x) || !read_file(bundle_dir() + "/" + a.subdir + "/" + lib, &y) ||
        x != y)
      return false;
  }
  std::string want, have;
  return manifest_for(a, &want, nullptr) && read_file(manifest_path(a), &have) && want == have;
}

void remove_arch(const Arch &a) {
  unlink(manifest_path(a).c_str());
  for (const char *lib : kLibs) unlink((bundle_dir() + "/" + a.subdir + "/" + lib).c_str());
}

bool install_arch(const Arch &a, std::string *err) {
  const std::string dir = bundle_dir() + "/" + a.subdir;
  if (!make_dirs(dir, err) || !make_dirs(layer_dir(), err)) return false;
  for (const char *lib : kLibs)
    if (!copy_if_changed(source_dir(a) + "/" + lib, dir + "/" + lib, err)) return false;
  std::string manifest, have;
  if (!manifest_for(a, &manifest, err)) return false;
  if (read_file(manifest_path(a), &have) && have == manifest) return true;
  return write_file(manifest_path(a), manifest, 0644, err);
}

bool write_run_script(std::string *err) {
  const std::string bin = bundle_dir() + "/bin";
  if (!make_dirs(bin, err)) return false;
  const std::string script =
      "#!/bin/sh\n"
      "# Gerado pelo Framux: o `framux run` dentro do Steam Flatpak.\n"
      "# Steam > Propriedades > Opções de inicialização: " +
      run_command() +
      "\n"
      "d='" +
      bundle_dir() +
      "'\n"
      "export FRAMUX=1\n"
      "unset DISABLE_FRAMUX FRAMUX_AUTO\n"
      // Um caminho por arquitetura, como o Steam faz com o próprio overlay: o
      // container da Steam (pressure-vessel) separa por classe ELF e, fora
      // dele, o ld.so ignora a de classe errada. Vai no fim da lista, depois
      // do overlay da Steam: com o shim antes, o dlsym do shim fica na frente
      // do dlsym do overlay e processos auxiliares (bash, jogo) travam.
      "if [ \"$FRAMUX_NO_GL\" != 1 ]; then\n"
      "  for f in \"$d/lib/libframux_gl.so\" \"$d/lib64/libframux_gl.so\"; do\n"
      "    [ -e \"$f\" ] && LD_PRELOAD=\"${LD_PRELOAD:+$LD_PRELOAD:}$f\"\n"
      "  done\n"
      "  export LD_PRELOAD\n"
      "fi\n"
      "exec \"$@\"\n";
  std::string have;
  if (read_file(bin + "/framux-run", &have) && have == script) return true;
  return write_file(bin + "/framux-run", script, 0755, err);
}

// --- override (formato GKeyFile, editado à mão para não depender do
// executável `flatpak` e para desfazer só o que é nosso) ---

struct KeyFile {
  std::vector<std::string> lines;

  bool load(const std::string &path) {
    std::string text;
    if (!read_file(path, &text)) return false;
    std::istringstream in(text);
    for (std::string l; std::getline(in, l);) lines.push_back(l);
    return true;
  }
  std::string text() const {
    std::string out;
    for (const auto &l : lines) out += l + "\n";
    return out;
  }
  // [início, fim) das linhas do grupo; início = npos se não existir.
  std::pair<size_t, size_t> group(const std::string &g) const {
    size_t begin = std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i) {
      if (lines[i].empty() || lines[i][0] != '[') continue;
      if (begin != std::string::npos) return {begin, i};
      if (lines[i] == "[" + g + "]") begin = i + 1;
    }
    return {begin, lines.size()};
  }
  size_t find(const std::string &g, const std::string &key) const {
    auto [b, e] = group(g);
    if (b == std::string::npos) return std::string::npos;
    for (size_t i = b; i < e; ++i)
      if (lines[i].compare(0, key.size() + 1, key + "=") == 0) return i;
    return std::string::npos;
  }
  std::string get(const std::string &g, const std::string &key) const {
    size_t i = find(g, key);
    return i == std::string::npos ? std::string() : lines[i].substr(key.size() + 1);
  }
  void set(const std::string &g, const std::string &key, const std::string &value) {
    if (size_t i = find(g, key); i != std::string::npos) {
      lines[i] = key + "=" + value;
      return;
    }
    auto [b, e] = group(g);
    if (b == std::string::npos) {
      if (!lines.empty() && !lines.back().empty()) lines.emplace_back();
      lines.push_back("[" + g + "]");
      lines.push_back(key + "=" + value);
      return;
    }
    while (e > b && lines[e - 1].empty()) --e;  // antes da linha em branco final
    lines.insert(lines.begin() + e, key + "=" + value);
  }
  void remove(const std::string &g, const std::string &key) {
    if (size_t i = find(g, key); i != std::string::npos) lines.erase(lines.begin() + i);
  }
};

std::vector<std::string> split_list(const std::string &v) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : v) {
    if (c == ';') {
      if (!cur.empty()) out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool ours(const std::string &fs) {
  const std::string base = fs.substr(0, fs.find(':'));
  return base == kRunFs || base == kConfigFs;
}

bool update_overrides(bool integrate, bool auto_mode, std::string *err) {
  KeyFile kf;
  kf.load(overrides_path());
  std::vector<std::string> fs;
  for (auto &f : split_list(kf.get("Context", "filesystems")))
    if (!ours(f)) fs.push_back(f);
  if (integrate) {
    fs.push_back(std::string(kRunFs) + ":create");
    fs.push_back(std::string(kConfigFs) + ":ro");
  }
  if (fs.empty()) {
    kf.remove("Context", "filesystems");
  } else {
    std::string v;
    for (auto &f : fs) v += f + ";";
    kf.set("Context", "filesystems", v);
  }
  // A config do sandbox ($XDG_CONFIG_HOME do Steam) é outra: aponta para a real.
  if (integrate)
    kf.set("Environment", "FRAMUX_CONFIG", main_config_path());
  else
    kf.remove("Environment", "FRAMUX_CONFIG");
  if (integrate && auto_mode) {
    kf.set("Environment", "FRAMUX", "1");
    kf.set("Environment", "FRAMUX_AUTO", "1");
  } else {
    kf.remove("Environment", "FRAMUX");
    kf.remove("Environment", "FRAMUX_AUTO");
  }
  const std::string path = overrides_path();
  if (!make_dirs(path.substr(0, path.rfind('/')), err)) return false;
  return write_file(path, kf.text(), 0644, err);
}

}  // namespace

std::string steam_home() { return home_dir() + "/.var/app/" + kSteamId; }
std::string bundle_dir() { return steam_home() + "/.local/share/framux"; }
std::string overrides_path() { return data_home() + "/flatpak/overrides/" + kSteamId; }

bool steam_installed() {
  return exists(std::string("/var/lib/flatpak/app/") + kSteamId) || exists(data_home() + "/flatpak/app/" + kSteamId);
}

Status status(bool deep) {
  Status s;
  s.steam = steam_installed();
  s.enabled = exists(manifest_path(kArchs[0]));
  s.layer64 = copies_present(kArchs[0]);
  s.layer32 = copies_present(kArchs[1]);
  s.source32 = source_present(kArchs[1]);
  if (s.enabled && deep) {
    for (const Arch &a : kArchs)
      if (source_present(a) && !copies_match(a)) s.outdated = true;
    if (s.layer32 && !s.source32) s.outdated = true;
  }
  KeyFile kf;
  kf.load(overrides_path());
  s.auto_env = kf.get("Environment", "FRAMUX") == "1" && kf.get("Environment", "FRAMUX_AUTO") == "1";
  return s;
}

bool enable(bool auto_mode, std::string *err) {
  if (!source_present(kArchs[0])) return fail(err, "camada de 64 bits não encontrada em " + source_dir(kArchs[0]));
  for (const Arch &a : kArchs) {
    if (source_present(a)) {
      if (!install_arch(a, err)) return false;
    } else {
      remove_arch(a);
    }
  }
  // Links do $LIB criados pela 0.3.0, que não são mais usados.
  for (const char *p : {"/lib/x86_64-linux-gnu", "/lib/i386-linux-gnu"}) unlink((bundle_dir() + p).c_str());
  return write_run_script(err) && update_overrides(true, auto_mode, err);
}

bool disable(std::string *err) {
  for (const Arch &a : kArchs) remove_arch(a);
  const std::string b = bundle_dir();
  for (const char *p : {"/lib/x86_64-linux-gnu", "/lib/i386-linux-gnu", "/bin/framux-run"}) unlink((b + p).c_str());
  for (const char *d : {"/lib64", "/lib", "/bin", ""}) rmdir((b + d).c_str());
  rmdir(layer_dir().c_str());
  return update_overrides(false, false, err);
}

bool sync(std::string *err) {
  if (!exists(manifest_path(kArchs[0]))) return true;
  KeyFile kf;
  kf.load(overrides_path());
  return enable(kf.get("Environment", "FRAMUX_AUTO") == "1", err);
}

bool set_auto(bool on, std::string *err) {
  if (!exists(manifest_path(kArchs[0]))) return true;
  return update_overrides(true, on, err);
}

std::string run_command() {
  std::string path = bundle_dir() + "/bin/framux-run";
  if (path.find(' ') != std::string::npos) path = "\"" + path + "\"";
  return path + " %command%";
}

}  // namespace framux::flatpak
