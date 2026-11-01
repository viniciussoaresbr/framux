// framux doctor no Windows: o que falta para o Framux funcionar nesta máquina.
#include <windows.h>

#include <cstdio>
#include <string>

#include "cli/cli.h"
#include "common/config.h"
#include "common/control_client.h"
#include "common/paths.h"
#include "common/win_setup.h"
#include "common/win_util.h"
#include "framux_build_config.h"

namespace framux::cli {

namespace {

int g_problems = 0;

void line(bool ok, const std::string &what, const std::string &hint = {}) {
  std::printf("%s %s\n", ok ? "[ok]  " : "[erro]", what.c_str());
  if (!ok) {
    ++g_problems;
    if (!hint.empty()) std::printf("       → %s\n", hint.c_str());
  }
}

void info(const std::string &what) { std::printf("[info] %s\n", what.c_str()); }

std::string windows_version() {
  using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
  auto fn = reinterpret_cast<RtlGetVersionFn>(
      reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
  OSVERSIONINFOW v{sizeof v};
  if (!fn || fn(&v) != 0) return "?";
  return std::to_string(v.dwMajorVersion) + "." + std::to_string(v.dwMinorVersion) + " build " +
         std::to_string(v.dwBuildNumber);
}

DWORD windows_build() {
  using RtlGetVersionFn = LONG(WINAPI *)(OSVERSIONINFOW *);
  auto fn = reinterpret_cast<RtlGetVersionFn>(
      reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
  OSVERSIONINFOW v{sizeof v};
  return fn && fn(&v) == 0 ? v.dwBuildNumber : 0;
}

}  // namespace

int cmd_doctor(const std::vector<std::string> &) {
  std::printf("framux %s · Windows %s\n\n", FRAMUX_VERSION, windows_version().c_str());

  // Loopback de áudio por processo: Windows 10 2004 (build 19041).
  const DWORD build = windows_build();
  line(build >= 19041, "Windows 10 2004 ou mais novo (áudio só do jogo)",
       "atualize o Windows; sem isso a gravação sai sem o som do jogo");

  const ctl::Reply st = ctl::call({"Status"});
  line(!st.unreachable, "framuxd rodando", "abra o Framux (framux-gui) ou rode `framux run <jogo.exe>`");
  if (st.ok) {
    // A detecção roda num processo filho logo que o daemon sobe: espera ela.
    ctl::Reply js = ctl::call({"StatusJson"});
    for (int i = 0; i < 20 && js.text.find("\"encoders\":[]") != std::string::npos; ++i) {
      Sleep(500);
      js = ctl::call({"StatusJson"});
    }
    const size_t p = js.text.find("\"encoders\":[");
    if (p != std::string::npos) {
      const std::string list = js.text.substr(p + 12, js.text.find(']', p) - p - 12);
      line(!list.empty(), "encoders de vídeo: " + (list.empty() ? std::string("nenhum") : list),
           "atualize o driver da placa de vídeo (NVIDIA, AMD Adrenalin ou Intel)");
    }
  }

  HMODULE vk = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
  line(vk != nullptr, "loader Vulkan (vulkan-1.dll)", "instale ou atualize o driver da placa de vídeo");
  if (vk) FreeLibrary(vk);

  const std::string manifest = win::layer_manifest_path();
  line(GetFileAttributesW(win::widen(manifest).c_str()) != INVALID_FILE_ATTRIBUTES, "manifesto da camada: " + manifest,
       "reinstale o Framux");
  line(win::layer_registered(), "camada Vulkan registrada", "abra o Framux uma vez (ou reinstale)");

  if (win::auto_mode_enabled())
    info(win::auto_mode_needs_restart() ? "modo automático ligado; feche e abra o Steam e os jogos para valer"
                                        : "modo automático ligado");
  else
    info("modo automático desligado: use `framux run <jogo.exe>` ou ligue na janela do Framux");

  line(win::can_trace_presents(), "permissão para medir o FPS de jogos DirectX/OpenGL",
       "reinstale o Framux (o instalador adiciona você a \"Usuários do log de desempenho\") e entre de novo no "
       "Windows");

  std::vector<std::string> warnings;
  const Config cfg = load_config("", &warnings);
  line(warnings.empty(), "config: " + main_config_path(), warnings.empty() ? "" : warnings.front());
  for (const auto &[label, folder] : {std::pair<const char *, std::string>{"screenshots", cfg.screenshot.folder},
                                      {"vídeos", cfg.video.folder},
                                      {"benchmarks", cfg.benchmark.folder}}) {
    std::string err;
    const std::string dir = expand_path(folder);
    line(make_dirs(dir, &err), std::string(label) + ": " + dir, err);
  }

  std::printf("\n%s\n", g_problems ? (std::to_string(g_problems) + " problema(s) encontrado(s)").c_str()
                                   : "tudo certo");
  return g_problems ? 1 : 0;
}

}  // namespace framux::cli
