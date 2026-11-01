#include "common/win_setup.h"

#include <windows.h>

#include "common/win_util.h"

namespace framux::win {

namespace {

constexpr const wchar_t *kLayersKey = L"Software\\Khronos\\Vulkan\\ImplicitLayers";
constexpr const wchar_t *kEnvKey = L"Environment";
constexpr const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t *kRunValue = L"Framux";

std::wstring backslashes(std::string p) {
  for (char &c : p)
    if (c == '/') c = '\\';
  return widen(p);
}

bool reg_has_value(HKEY root, const wchar_t *key, const std::wstring &name) {
  HKEY k;
  if (RegOpenKeyExW(root, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
  const bool ok = RegQueryValueExW(k, name.c_str(), nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
  RegCloseKey(k);
  return ok;
}

std::wstring reg_string(HKEY root, const wchar_t *key, const wchar_t *name) {
  wchar_t buf[2048];
  DWORD size = sizeof buf;
  if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, buf, &size) !=
      ERROR_SUCCESS)
    return {};
  return buf;
}

bool reg_set_string(HKEY root, const wchar_t *key, const wchar_t *name, const std::wstring &v, std::string *err) {
  HKEY k;
  LSTATUS r = RegCreateKeyExW(root, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
  if (r == ERROR_SUCCESS) {
    r = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE *>(v.c_str()),
                       DWORD((v.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
  }
  if (r != ERROR_SUCCESS && err) *err = error_text(uint32_t(r));
  return r == ERROR_SUCCESS;
}

bool reg_delete(HKEY root, const wchar_t *key, const wchar_t *name, std::string *err) {
  HKEY k;
  if (RegOpenKeyExW(root, key, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return true;
  const LSTATUS r = RegDeleteValueW(k, name);
  RegCloseKey(k);
  if (r != ERROR_SUCCESS && r != ERROR_FILE_NOT_FOUND) {
    if (err) *err = error_text(uint32_t(r));
    return false;
  }
  return true;
}

// Avisa o Explorer (e quem mais escuta) que o ambiente do usuário mudou.
void broadcast_environment() {
  DWORD_PTR result;
  SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"Environment"),
                      SMTO_ABORTIFHUNG, 2000, &result);
}

}  // namespace

std::string sibling(const char *exe) { return module_dir() + "/" + exe; }

std::string layer_manifest_path() {
  return module_dir() + (sizeof(void *) == 8 ? "/framux_vk_x86_64.json" : "/framux_vk_i686.json");
}

bool layer_registered() {
  const std::wstring m = backslashes(layer_manifest_path());
  return reg_has_value(HKEY_CURRENT_USER, kLayersKey, m) || reg_has_value(HKEY_LOCAL_MACHINE, kLayersKey, m);
}

bool set_layer_registered(bool on, std::string *err) {
  const std::wstring m = backslashes(layer_manifest_path());
  if (!on) return reg_delete(HKEY_CURRENT_USER, kLayersKey, m.c_str(), err);
  HKEY k;
  LSTATUS r = RegCreateKeyExW(HKEY_CURRENT_USER, kLayersKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr);
  if (r == ERROR_SUCCESS) {
    const DWORD zero = 0;  // 0 = camada habilitada (o loader ignora valores ≠ 0)
    r = RegSetValueExW(k, m.c_str(), 0, REG_DWORD, reinterpret_cast<const BYTE *>(&zero), sizeof zero);
    RegCloseKey(k);
  }
  if (r != ERROR_SUCCESS && err) *err = error_text(uint32_t(r));
  return r == ERROR_SUCCESS;
}

bool auto_mode_enabled() {
  return reg_string(HKEY_CURRENT_USER, kEnvKey, L"FRAMUX_AUTO") == L"1" &&
         reg_string(HKEY_CURRENT_USER, kEnvKey, L"FRAMUX") == L"1";
}

bool set_auto_mode(bool on, std::string *err) {
  bool ok;
  if (on)
    ok = reg_set_string(HKEY_CURRENT_USER, kEnvKey, L"FRAMUX", L"1", err) &&
         reg_set_string(HKEY_CURRENT_USER, kEnvKey, L"FRAMUX_AUTO", L"1", err);
  else
    ok = reg_delete(HKEY_CURRENT_USER, kEnvKey, L"FRAMUX", err) &&
         reg_delete(HKEY_CURRENT_USER, kEnvKey, L"FRAMUX_AUTO", err);
  broadcast_environment();
  return ok;
}

bool auto_mode_needs_restart() { return auto_mode_enabled() && env("FRAMUX_AUTO") != "1"; }

bool open_on_login_enabled() { return !reg_string(HKEY_CURRENT_USER, kRunKey, kRunValue).empty(); }

bool set_open_on_login(bool on, std::string *err) {
  if (!on) return reg_delete(HKEY_CURRENT_USER, kRunKey, kRunValue, err);
  const std::wstring cmd = L"\"" + backslashes(sibling("framux-gui.exe")) + L"\" --autostart";
  return reg_set_string(HKEY_CURRENT_USER, kRunKey, kRunValue, cmd, err);
}

bool can_trace_presents() {
  SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
  auto member = [&](DWORD rid) {
    PSID sid = nullptr;
    BOOL in = FALSE;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID, rid, 0, 0, 0, 0, 0, 0, &sid)) {
      CheckTokenMembership(nullptr, sid, &in);
      FreeSid(sid);
    }
    return in != FALSE;
  };
  return member(DOMAIN_ALIAS_RID_LOGGING_USERS) || member(DOMAIN_ALIAS_RID_ADMINS);
}

bool is_elevated() {
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
  TOKEN_ELEVATION e{};
  DWORD n = 0;
  const bool elevated = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
  CloseHandle(tok);
  return elevated;
}

bool spawn_daemon() {
  const std::wstring exe = backslashes(sibling("framuxd.exe"));
  if (is_elevated()) {
    // Ex.: a janela aberta pelo fim do instalador. O Explorer inicia o
    // programa com o token comum do usuário conectado.
    wchar_t win[MAX_PATH];
    if (GetWindowsDirectoryW(win, MAX_PATH)) {
      std::wstring cmd = L"\"" + std::wstring(win) + L"\\explorer.exe\" \"" + exe + L"\"";
      STARTUPINFOW si{sizeof si};
      PROCESS_INFORMATION pi{};
      if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
      }
    }
  }
  std::wstring cmd = L"\"" + exe + L"\"";
  STARTUPINFOW si{sizeof si};
  PROCESS_INFORMATION pi{};
  // O daemon não herda o FRAMUX=1 destinado ao jogo.
  SetEnvironmentVariableW(L"FRAMUX", nullptr);
  const bool ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NO_WINDOW,
                                 nullptr, nullptr, &si, &pi);
  if (ok) {
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
  }
  return ok;
}

}  // namespace framux::win
