#include "common/identity.h"

#include <unistd.h>

#include <cctype>
#include <climits>
#include <fstream>
#include <iterator>

#include "common/paths.h"

namespace framux {

namespace {

std::string basename_any(const std::string &p) {
  // Caminhos Windows chegam com '\' no Wine.
  const size_t pos = p.find_last_of("/\\");
  return pos == std::string::npos ? p : p.substr(pos + 1);
}

bool ends_with_icase(const std::string &s, const char *suffix) {
  const size_t n = std::char_traits<char>::length(suffix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; ++i)
    if (std::tolower((unsigned char)s[s.size() - n + i]) != suffix[i]) return false;
  return true;
}

bool is_wine_binary(const std::string &base) {
  return base == "wine" || base == "wine64" || base == "wine-preloader" || base == "wine64-preloader";
}

}  // namespace

ProcessIdentity identity_from(const std::string &exe, const std::string &comm, const std::vector<std::string> &argv) {
  ProcessIdentity id;
  id.exe = exe;
  id.exe_base = basename_any(exe);
  id.comm = comm;

  std::string win_exe;
  for (const auto &a : argv) {
    if (ends_with_icase(a, ".exe")) {
      win_exe = basename_any(a);
      break;
    }
  }
  id.wine = is_wine_binary(id.exe_base) || (!win_exe.empty() && ends_with_icase(comm, ".exe"));
  if (id.wine && !win_exe.empty()) {
    id.game = win_exe.substr(0, win_exe.size() - 4);
    id.config_name = "wine-" + id.game;
  } else {
    id.game = id.exe_base.empty() ? comm : id.exe_base;
    id.config_name = id.game;
  }
  id.game = sanitize_filename(id.game);
  return id;
}

ProcessIdentity current_identity() {
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  std::string exe = n > 0 ? std::string(buf, size_t(n)) : std::string();

  std::string comm;
  {
    std::ifstream in("/proc/self/comm");
    std::getline(in, comm);
  }
  std::vector<std::string> argv;
  {
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    size_t start = 0;
    for (size_t i = 0; i <= all.size(); ++i) {
      if (i == all.size() || all[i] == '\0') {
        if (i > start) argv.push_back(all.substr(start, i - start));
        start = i + 1;
      }
    }
  }
  ProcessIdentity id = identity_from(exe, comm, argv);
  id.pid = getpid();
  return id;
}

}  // namespace framux
