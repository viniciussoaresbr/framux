// identity.h no Windows: o jogo é o nome do .exe (sem a extensão).
#include <windows.h>

#include <cctype>

#include "common/identity.h"
#include "common/paths.h"
#include "common/win_util.h"

namespace framux {

namespace {

std::string basename_any(const std::string &p) {
  const size_t pos = p.find_last_of("/\\");
  return pos == std::string::npos ? p : p.substr(pos + 1);
}

std::string strip_exe(const std::string &s) {
  if (s.size() > 4) {
    std::string ext = s.substr(s.size() - 4);
    for (char &c : ext) c = char(std::tolower((unsigned char)c));
    if (ext == ".exe") return s.substr(0, s.size() - 4);
  }
  return s;
}

}  // namespace

ProcessIdentity identity_from(const std::string &exe, const std::string &comm, const std::vector<std::string> &) {
  ProcessIdentity id;
  id.exe = exe;
  id.exe_base = strip_exe(basename_any(exe));
  id.comm = comm.empty() ? basename_any(exe) : comm;
  id.game = id.exe_base.empty() ? strip_exe(id.comm) : id.exe_base;
  id.config_name = id.game;
  id.game = sanitize_filename(id.game);
  return id;
}

ProcessIdentity current_identity() {
  const std::string exe = win::module_path(nullptr);
  ProcessIdentity id = identity_from(exe, basename_any(exe), {});
  id.pid = int(GetCurrentProcessId());
  return id;
}

}  // namespace framux
