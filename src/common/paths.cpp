#include "common/paths.h"

#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace framux {

namespace {
std::string getenv_str(const char *name) {
  const char *v = std::getenv(name);
  return v ? std::string(v) : std::string();
}
}  // namespace

std::string home_dir() {
  std::string h = getenv_str("HOME");
  if (!h.empty()) return h;
  if (const passwd *pw = getpwuid(getuid())) return pw->pw_dir;
  return "/";
}

std::string config_dir() {
  std::string base = getenv_str("XDG_CONFIG_HOME");
  if (base.empty() || base[0] != '/') base = home_dir() + "/.config";
  return base + "/framux";
}

std::string runtime_dir() {
  std::string base = getenv_str("XDG_RUNTIME_DIR");
  if (base.empty() || base[0] != '/') return {};
  return base + "/framux";
}

std::string socket_path() {
  const std::string d = runtime_dir();
  return d.empty() ? d : d + "/ipc.sock";
}

std::string stats_path() {
  const std::string d = runtime_dir();
  return d.empty() ? d : d + "/stats";
}

std::string stopped_path() {
  const std::string d = runtime_dir();
  return d.empty() ? d : d + "/stopped";
}

std::string xdg_user_dir(std::string_view name) {
  const std::string key = "XDG_" + std::string(name) + "_DIR";
  std::string env = getenv_str(key.c_str());
  if (!env.empty()) return env;

  std::string cfg = getenv_str("XDG_CONFIG_HOME");
  if (cfg.empty()) cfg = home_dir() + "/.config";
  std::ifstream in(cfg + "/user-dirs.dirs");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind(key + "=", 0) != 0) continue;
    std::string v = line.substr(key.size() + 1);
    if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
    if (v.rfind("$HOME", 0) == 0) v = home_dir() + v.substr(5);
    return v;
  }
  // Padrões do xdg-user-dirs em inglês.
  if (name == "PICTURES") return home_dir() + "/Pictures";
  if (name == "VIDEOS") return home_dir() + "/Videos";
  return home_dir();
}

std::string expand_path(std::string_view in) {
  std::string out;
  size_t i = 0;
  if (!in.empty() && in[0] == '~' && (in.size() == 1 || in[1] == '/')) {
    out = home_dir();
    i = 1;
  }
  while (i < in.size()) {
    if (in[i] != '$') {
      out += in[i++];
      continue;
    }
    size_t start = i + 1, end;
    bool braced = start < in.size() && in[start] == '{';
    if (braced) {
      end = in.find('}', start);
      if (end == std::string_view::npos) {
        out += in.substr(i);
        break;
      }
      ++start;
    } else {
      end = start;
      while (end < in.size() && (std::isalnum((unsigned char)in[end]) || in[end] == '_')) ++end;
    }
    const std::string name(in.substr(start, end - start));
    i = braced ? end + 1 : end;
    if (name.empty()) {
      out += '$';
      continue;
    }
    if (name == "HOME")
      out += home_dir();
    else if (name.rfind("XDG_", 0) == 0 && name.size() > 8 && name.compare(name.size() - 4, 4, "_DIR") == 0 &&
             name != "XDG_RUNTIME_DIR")
      out += xdg_user_dir(name.substr(4, name.size() - 8));
    else
      out += getenv_str(name.c_str());
  }
  return out;
}

std::string sanitize_filename(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    const unsigned char u = (unsigned char)c;
    if (u < 0x20 || u == 0x7f || std::strchr("/\\:*?\"<>|", c))
      out += '_';
    else
      out += c;
  }
  if (out.empty() || out == "." || out == "..") out = "_" + out;
  return out;
}

bool make_dirs(const std::string &path, std::string *err) {
  if (path.empty()) return false;
  std::string cur;
  size_t pos = 0;
  while (pos != std::string::npos) {
    pos = path.find('/', pos + 1);
    cur = path.substr(0, pos);
    if (cur.empty()) continue;
    if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
      if (err) *err = cur + ": " + std::strerror(errno);
      return false;
    }
  }
  struct stat st;
  if (stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    if (err) *err = path + ": não é um diretório";
    return false;
  }
  return true;
}

}  // namespace framux
