// paths.h no Windows. Pastas do usuário pelas Known Folders; o "runtime_dir"
// fica em %LOCALAPPDATA%\framux e o IPC usa named pipes por usuário/sessão.
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <cctype>
#include <cstring>
#include <filesystem>
#include <system_error>

#include "common/paths.h"
#include "common/win_util.h"

namespace framux {

namespace win {

std::wstring widen(std::string_view s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
  std::wstring out(size_t(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
  return out;
}

std::string narrow(std::wstring_view s) {
  if (s.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
  return out;
}

std::string env(const char *name) {
  const std::wstring w = widen(name);
  const DWORD n = GetEnvironmentVariableW(w.c_str(), nullptr, 0);
  if (n == 0) return {};
  std::wstring v(n, L'\0');
  const DWORD got = GetEnvironmentVariableW(w.c_str(), v.data(), n);
  v.resize(got);
  return narrow(v);
}

std::string error_text(uint32_t code) {
  if (!code) code = GetLastError();
  wchar_t *buf = nullptr;
  const DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                     FORMAT_MESSAGE_IGNORE_INSERTS,
                                 nullptr, code, 0, reinterpret_cast<wchar_t *>(&buf), 0, nullptr);
  std::string s = n ? narrow(std::wstring_view(buf, n)) : "erro " + std::to_string(code);
  if (buf) LocalFree(buf);
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '.')) s.pop_back();
  return s;
}

std::string user_tag() {
  static const std::string tag = [] {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    wchar_t name[257];
    DWORD n = 257;
    std::string user = GetUserNameW(name, &n) && n > 1 ? narrow(std::wstring_view(name, n - 1)) : "user";
    for (char &c : user)
      if (c == '\\' || c == '/' || c == ' ' || c == ':') c = '_';
    return std::to_string(session) + "-" + user;
  }();
  return tag;
}

std::string module_path(const void *addr) {
  HMODULE mod = nullptr;
  if (addr)
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       static_cast<LPCWSTR>(addr), &mod);
  std::wstring buf(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(mod, buf.data(), DWORD(buf.size()));
    if (n < buf.size()) {
      buf.resize(n);
      break;
    }
    buf.resize(buf.size() * 2);
  }
  std::string p = narrow(buf);
  for (char &c : p)
    if (c == '\\') c = '/';
  return p;
}

std::string module_dir(const void *addr) {
  const std::string p = module_path(addr);
  const size_t slash = p.rfind('/');
  return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

}  // namespace win

namespace {

std::string known_folder(REFKNOWNFOLDERID id) {
  PWSTR path = nullptr;
  std::string out;
  if (SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path) == S_OK) out = win::narrow(path);
  CoTaskMemFree(path);
  for (char &c : out)
    if (c == '\\') c = '/';
  return out;
}

std::string slashes(std::string s) {
  for (char &c : s)
    if (c == '\\') c = '/';
  return s;
}

}  // namespace

std::string home_dir() {
  std::string h = win::env("USERPROFILE");
  if (h.empty()) h = known_folder(FOLDERID_Profile);
  return h.empty() ? "C:/" : slashes(h);
}

std::string config_dir() {
  std::string base = win::env("APPDATA");
  if (base.empty()) base = known_folder(FOLDERID_RoamingAppData);
  return slashes(base) + "/framux";
}

std::string runtime_dir() {
  std::string base = win::env("LOCALAPPDATA");
  if (base.empty()) base = known_folder(FOLDERID_LocalAppData);
  return base.empty() ? base : slashes(base) + "/framux";
}

std::string socket_path() { return "\\\\.\\pipe\\framux-" + win::user_tag() + "-ipc"; }

std::string stats_path() { return "Local\\framux-" + win::user_tag() + "-stats"; }

std::string stopped_path() {
  const std::string d = runtime_dir();
  return d.empty() ? d : d + "/stopped";
}

std::string xdg_user_dir(std::string_view name) {
  const std::string key = "XDG_" + std::string(name) + "_DIR";
  std::string env = win::env(key.c_str());
  if (!env.empty()) return slashes(env);
  std::string p;
  if (name == "PICTURES") p = known_folder(FOLDERID_Pictures);
  if (name == "VIDEOS") p = known_folder(FOLDERID_Videos);
  if (name == "DOCUMENTS") p = known_folder(FOLDERID_Documents);
  if (name == "MUSIC") p = known_folder(FOLDERID_Music);
  if (name == "DESKTOP") p = known_folder(FOLDERID_Desktop);
  if (name == "DOWNLOAD") p = known_folder(FOLDERID_Downloads);
  if (!p.empty()) return p;
  if (name == "PICTURES") return home_dir() + "/Pictures";
  if (name == "VIDEOS") return home_dir() + "/Videos";
  if (name == "DOCUMENTS") return home_dir() + "/Documents";
  return home_dir();
}

std::string expand_path(std::string_view in) {
  std::string out;
  size_t i = 0;
  if (!in.empty() && in[0] == '~' && (in.size() == 1 || in[1] == '/' || in[1] == '\\')) {
    out = home_dir();
    i = 1;
  }
  while (i < in.size()) {
    // %NOME% do Windows, além de $NOME / ${NOME}.
    if (in[i] == '%') {
      const size_t end = in.find('%', i + 1);
      if (end != std::string_view::npos && end > i + 1) {
        out += slashes(win::env(std::string(in.substr(i + 1, end - i - 1)).c_str()));
        i = end + 1;
        continue;
      }
    }
    if (in[i] != '$') {
      out += in[i] == '\\' ? '/' : in[i];
      ++i;
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
      out += slashes(win::env(name.c_str()));
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
  // O Windows não aceita nomes terminados em ponto ou espaço.
  while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.back() = '_';
  if (out.empty() || out == "." || out == "..") out = "_" + out;
  return out;
}

bool make_dirs(const std::string &path, std::string *err) {
  if (path.empty()) return false;
  std::error_code ec;
  const std::filesystem::path p(win::widen(path));
  std::filesystem::create_directories(p, ec);
  if (!std::filesystem::is_directory(p)) {
    if (err) *err = path + ": " + (ec ? win::narrow(win::widen(ec.message())) : std::string("não é um diretório"));
    return false;
  }
  return true;
}

}  // namespace framux

#include "common/control_proto.h"

std::string framux::ctl::control_pipe_name() { return "\\\\.\\pipe\\framux-" + framux::win::user_tag() + "-ctl"; }
