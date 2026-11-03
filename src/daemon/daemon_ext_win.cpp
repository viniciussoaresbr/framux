// Jogos Direct3D/OpenGL no Windows, medidos e capturados de fora do processo
// deles (o equivalente à camada/shim do Linux, sem entrar no jogo):
//
//  - FPS e benchmark: presents vistos pelo ETW (etw_presents_win) viram
//    registros no ring da sessão, que a lógica comum drena;
//  - contador: janela sobreposta (overlay_window_win);
//  - screenshot e gravação: Windows Graphics Capture (window_capture_win);
//  - atalhos: estado do teclado lido pelo daemon enquanto o jogo tem o foco.
//
// Um processo vira sessão quando apresenta ao menos kMinFps quadros num
// segundo, tem janela visível e não é excluído por [filters].
#include <windows.h>
#include <dxgi.h>
#include <psapi.h>
#include <shellapi.h>

#include <cinttypes>
#include <cstring>

#include "client/win_input.h"
#include "common/identity.h"
#include "common/log.h"
#include "common/paths.h"
#include "common/win_util.h"
#include "core/image.h"
#include "core/naming.h"
#include "daemon/daemon.h"

namespace framux::daemon {

namespace {

constexpr uint32_t kMinFps = 20;           // presents por segundo para virar sessão
constexpr uint64_t kForgetNs = 10 * kNsPerSec;  // processo sem presents há 10 s sai da lista

enum Action { kActBenchmark = 0, kActOverlay, kActScreenshot, kActVideo, kActLoopSave, kActOverlayCorner, kActCount };

struct FindWindow {
  DWORD pid;
  HWND best = nullptr;
  long long area = 0;
};

BOOL CALLBACK find_window_cb(HWND w, LPARAM lp) {
  auto *f = reinterpret_cast<FindWindow *>(lp);
  DWORD pid = 0;
  GetWindowThreadProcessId(w, &pid);
  if (pid != f->pid || !IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
  RECT r;
  if (!GetClientRect(w, &r)) return TRUE;
  const long long a = (long long)(r.right - r.left) * (r.bottom - r.top);
  if (a > f->area) {
    f->area = a;
    f->best = w;
  }
  return TRUE;
}

// Maior janela visível de primeiro nível do processo.
HWND main_window(DWORD pid) {
  FindWindow f{pid};
  EnumWindows(find_window_cb, reinterpret_cast<LPARAM>(&f));
  return f.area > 0 ? f.best : nullptr;
}

bool has_module(HANDLE proc, const char *name) {
  HMODULE mods[1024];
  DWORD need = 0;
  if (!EnumProcessModulesEx(proc, mods, sizeof mods, &need, LIST_MODULES_ALL)) return false;
  for (DWORD i = 0; i < std::min<DWORD>(need / sizeof(HMODULE), 1024); ++i) {
    char base[MAX_PATH];
    if (GetModuleBaseNameA(proc, mods[i], base, sizeof base) && !_stricmp(base, name)) return true;
  }
  return false;
}

std::string gpu_description() {
  IDXGIFactory1 *f = nullptr;
  if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&f)))) return {};
  std::string out;
  IDXGIAdapter1 *a = nullptr;
  if (SUCCEEDED(f->EnumAdapters1(0, &a))) {
    DXGI_ADAPTER_DESC1 d;
    if (SUCCEEDED(a->GetDesc1(&d))) out = win::narrow(d.Description);
    a->Release();
  }
  f->Release();
  return out;
}

// Programas da lista padrão de [filters].exclude. Um config.toml salvo
// guarda a lista daquela versão, que substitui a padrão: os nomes acrescentados
// depois (o player de mídia, por exemplo) nunca chegavam a quem já tinha
// configuração. A lista padrão atual vale sempre, menos para o que estiver em
// [filters].include. Sem diferenciar maiúsculas, como no Windows.
bool is_known_desktop_app(const Config &cfg, const std::string &exe_base, const std::string &game) {
  auto eq = [](const std::string &a, const std::string &b) { return !a.empty() && !_stricmp(a.c_str(), b.c_str()); };
  for (const auto &n : cfg.filters.include)
    if (eq(exe_base, n) || eq(game, n)) return false;
  static const std::vector<std::string> defaults = Config{}.filters.exclude;
  for (const auto &n : defaults)
    if (eq(exe_base, n) || eq(game, n)) return true;
  return false;
}

void copy_str(char *dst, size_t n, const std::string &src) {
  std::strncpy(dst, src.c_str(), n - 1);
  dst[n - 1] = '\0';
}

}  // namespace

// Estado por processo que apresenta, compartilhado com a thread do ETW.
struct Daemon::ExtProc {
  uint32_t pid = 0;
  std::vector<uint8_t> ring_mem;
  RingHeader *ring = nullptr;
  enum State { Watching, Session, Ignored } state = Watching;
  bool posted = false;         // candidato já enviado à thread principal
  bool saw_api = false;        // teve presents de DXGI/D3D9 (ignora os do kernel)
  PresentSource source = PresentSource::Kernel;
  std::map<uint64_t, std::pair<uint32_t, uint64_t>> swapchains;  // ponteiro → (id, quadros)
  uint32_t next_id = 1;
  uint64_t window_start = 0, window_count = 0;
  uint64_t last_t = 0;
  std::atomic<uint32_t> width{0}, height{0};
};

Session::~Session() {
  ext_capture.reset();
  if (ext_process) CloseHandle(static_cast<HANDLE>(ext_process));
}

// Thread do ETW: um present de qualquer processo.
void Daemon::on_external_present(uint32_t pid, PresentSource src, uint64_t sc, uint64_t t) {
  if (pid == GetCurrentProcessId() || pid <= 4) return;
  std::lock_guard lk(ext_mu_);
  auto &p = ext_procs_[pid];
  if (!p) {
    p = std::make_shared<ExtProc>();
    p->pid = pid;
    p->ring_mem.resize(ring_bytes(kRingDefaultCapacity) + 64);
    // Alinhado a 64 bytes, como a seção que a camada recebe.
    void *base = p->ring_mem.data();
    size_t space = p->ring_mem.size();
    p->ring = static_cast<RingHeader *>(std::align(64, ring_bytes(kRingDefaultCapacity), base, space));
    ring_init(p->ring, kRingDefaultCapacity);
    FRAMUX_DEBUG("ETW: primeiro present do processo %u (%s)", pid,
                 src == PresentSource::Dxgi ? "DXGI" : src == PresentSource::D3d9 ? "D3D9" : "kernel");
  }
  ExtProc &e = *p;
  if (e.state == ExtProc::Ignored) return;
  // OpenGL e afins só aparecem no kernel; com DXGI/D3D9 os do kernel repetem.
  if (src == PresentSource::Kernel) {
    if (e.saw_api) return;
  } else if (!e.saw_api) {
    e.saw_api = true;
    e.source = src;
  }
  e.last_t = t;
  auto &[id, frames] = e.swapchains[sc];
  if (!id) id = e.next_id++;
  ring_push(e.ring, FrameRec{t, ++frames, id, e.width.load(), e.height.load(), 0});
  if (e.state == ExtProc::Watching && !e.posted) {
    if (t - e.window_start > kNsPerSec) {
      e.window_start = t;
      e.window_count = 0;
    }
    if (++e.window_count >= kMinFps) {
      e.posted = true;
      main_queue_.post([this, pid] { ext_candidate(pid); });
    }
  }
}

// Thread principal: o processo `pid` apresenta como um jogo; vira sessão?
void Daemon::ext_candidate(uint32_t pid) {
  std::shared_ptr<ExtProc> p;
  {
    std::lock_guard lk(ext_mu_);
    auto it = ext_procs_.find(pid);
    if (it == ext_procs_.end()) return;
    p = it->second;
  }
  auto ignore = [&](const char *why, const std::string &name) {
    std::lock_guard lk(ext_mu_);
    p->state = ExtProc::Ignored;
    FRAMUX_DEBUG("processo %u (%s) ignorado: %s", pid, name.c_str(), why);
  };
  // A camada Vulkan já mede este processo.
  for (const auto &[id, s] : sessions_)
    if (!s->external && s->hello_done && uint32_t(s->hello.pid) == pid) return ignore("tem a camada Vulkan", "");

  HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ | SYNCHRONIZE, FALSE, pid);
  if (!proc) proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
  if (!proc) return ignore("sem acesso ao processo (elevado?)", "");
  wchar_t path[MAX_PATH * 2];
  DWORD n = MAX_PATH * 2;
  std::string exe = QueryFullProcessImageNameW(proc, 0, path, &n) ? win::narrow(std::wstring_view(path, n)) : "";
  for (char &c : exe)
    if (c == '\\') c = '/';
  const ProcessIdentity id = identity_from(exe, {}, {});
  const Config cfg = load_config(id.config_name, nullptr);
  if (exe.empty() || is_excluded(cfg, id.exe_base, id.game)) {
    CloseHandle(proc);
    return ignore("excluído por [filters]", id.exe_base);
  }
  if (is_known_desktop_app(cfg, id.exe_base, id.game)) {
    CloseHandle(proc);
    return ignore("programa de desktop da lista padrão", id.exe_base);
  }
  HWND w = main_window(pid);
  if (!w) {
    // Ainda sem janela (tela de carregamento): tenta de novo mais tarde.
    FRAMUX_DEBUG("processo %u (%s): sem janela visível ainda", pid, id.exe_base.c_str());
    CloseHandle(proc);
    std::lock_guard lk(ext_mu_);
    p->posted = false;
    return;
  }

  auto s = std::make_unique<Session>();
  s->id = next_session_id_++;
  s->external = true;
  s->ext_window = w;
  s->ext_process = proc;
  s->peer_pid = int(pid);
  s->hello.proto_version = ipc::kProtoVersion;
  s->hello.pid = int(pid);
  ipc::Api api = ipc::Api::D3D11;
  if (p->source == PresentSource::D3d9)
    api = ipc::Api::D3D9;
  else if (p->source == PresentSource::Kernel)
    api = has_module(proc, "opengl32.dll") ? ipc::Api::GlWgl : ipc::Api::Unknown;
  else if (has_module(proc, "d3d12.dll"))
    api = ipc::Api::D3D12;
  s->hello.api = uint32_t(api);
  USHORT pm = 0, nm = 0;
  s->hello.bits = IsWow64Process2(proc, &pm, &nm) && pm != IMAGE_FILE_MACHINE_UNKNOWN ? 32 : 64;
  copy_str(s->hello.exe, sizeof s->hello.exe, exe);
  copy_str(s->hello.comm, sizeof s->hello.comm, id.comm);
  copy_str(s->hello.game, sizeof s->hello.game, id.game);
  copy_str(s->hello.config_name, sizeof s->hello.config_name, id.config_name);
  if (gpu_name_.empty()) gpu_name_ = gpu_description();
  copy_str(s->hello.gpu, sizeof s->hello.gpu, gpu_name_);
  s->game = id.game.empty() ? "jogo" : id.game;
  s->config_name = id.config_name;
  s->exe = exe;
  s->cfg = cfg;
  s->corner = cfg.overlay.corner;
  s->ext_overlay = std::make_unique<OverlayState>();
  s->ext_overlay->set_config(cfg.overlay);
  s->ext_overlay->set_corner(s->corner);
  s->ring = p->ring;
  s->ring_size = 0;
  s->hello_done = true;
  FRAMUX_LOG("sessão %" PRIu64 ": %s (pid %u, %s, %u bits, medido de fora)", s->id, s->game.c_str(), pid,
             ipc::api_name(api), s->hello.bits);
  {
    std::lock_guard lk(ext_mu_);
    p->state = ExtProc::Session;
  }
  sessions_[s->id] = std::move(s);
  update_active();
}

// A cada tick: janelas, foco, fim dos processos, overlay e atalhos.
void Daemon::ext_tick() {
  const uint64_t now = now_ns();
  // Diagnóstico: quantos eventos de present o ETW entregou no começo.
  static const uint64_t report_at = now + 10 * kNsPerSec;
  static bool reported = false;
  if (tracer_ && !reported && now >= report_at) {
    reported = true;
    size_t procs;
    {
      std::lock_guard lk(ext_mu_);
      procs = ext_procs_.size();
    }
    FRAMUX_LOG("ETW: %llu eventos de present nos primeiros 10 s, de %zu processo(s)",
               (unsigned long long)tracer_->events_.load(), procs);
  }
  HWND fg = GetForegroundWindow();
  DWORD fg_pid = 0;
  if (fg) GetWindowThreadProcessId(fg, &fg_pid);

  std::vector<uint64_t> ended;
  for (auto &[id, s] : sessions_) {
    if (!s->external) continue;
    const uint32_t pid = uint32_t(s->hello.pid);
    if (WaitForSingleObject(static_cast<HANDLE>(s->ext_process), 0) == WAIT_OBJECT_0) {
      ended.push_back(id);
      continue;
    }
    if (!IsWindow(static_cast<HWND>(s->ext_window)))
      if (HWND w = main_window(pid)) s->ext_window = w;
    RECT r{};
    GetClientRect(static_cast<HWND>(s->ext_window), &r);
    {
      std::lock_guard lk(ext_mu_);
      if (auto it = ext_procs_.find(pid); it != ext_procs_.end()) {
        it->second->width = uint32_t(r.right);
        it->second->height = uint32_t(r.bottom);
      }
    }
    const int focus = fg_pid == pid ? 1 : 0;
    if (focus != s->focus) {
      s->focus = focus;
      update_active();
    }
    if (focus) ext_poll_keys(*s);
  }
  for (uint64_t id : ended) {
    const uint32_t pid = uint32_t(sessions_[id]->hello.pid);
    close_session(id, "o jogo encerrou");
    std::lock_guard lk(ext_mu_);
    ext_procs_.erase(pid);
  }
  {
    // Processos que pararam de apresentar sem virar sessão.
    std::lock_guard lk(ext_mu_);
    for (auto it = ext_procs_.begin(); it != ext_procs_.end();) {
      if (it->second->state != ExtProc::Session && it->second->last_t && now > it->second->last_t + kForgetNs)
        it = ext_procs_.erase(it);
      else
        ++it;
    }
  }

  // Contador sobre o jogo ativo, só com a janela dele em primeiro plano.
  if (!overlay_win_) return;
  Session *a = nullptr;
  if (auto it = sessions_.find(arbiter_.active()); it != sessions_.end()) a = it->second.get();
  if (!a || !a->external || a->focus != 1 || !a->ext_overlay) {
    overlay_win_->hide();
    return;
  }
  const SwapStat *p = a->primary();
  const bool live = p && now - p->last_present_ns < kNsPerSec;
  RECT r{};
  GetClientRect(static_cast<HWND>(a->ext_window), &r);
  const OverlayContent c =
      a->ext_overlay->compute(now, live ? p->fps.display_fps() : 0, live ? p->last_frametime_ns : 0);
  OverlayDraw d;
  build_overlay(c, uint32_t(r.right), uint32_t(r.bottom), d);
  overlay_win_->show(a->ext_window, d);

  // Em tela cheia exclusiva o jogo apresenta direto no monitor, sem passar
  // pelo compositor: nenhuma janela de fora aparece por cima, o contador
  // incluído (FPS, benchmark, screenshot e gravação seguem funcionando).
  // O Windows informa esse modo. O estado vai para o StatusJson (aviso na
  // janela do Framux) e é lido só com o jogo em foco: fora dele (alt+tab), o
  // último valor visto continua valendo. A notificação sai uma vez por sessão.
  static uint64_t next_check = 0;
  if (now >= next_check) {
    next_check = now + kNsPerSec;
    QUERY_USER_NOTIFICATION_STATE q{};
    a->ext_exclusive = SUCCEEDED(SHQueryUserNotificationState(&q)) && q == QUNS_RUNNING_D3D_FULL_SCREEN;
    if (a->ext_exclusive && d.visible && !a->ext_exclusive_warned) {
      a->ext_exclusive_warned = true;
      FRAMUX_LOG("%s: em tela cheia exclusiva; o contador não aparece sobre o jogo nesse modo",
                 a->game.c_str());
      if (a->cfg.overlay.exclusive_notice)
        notify("O contador não aparece neste jogo",
               a->game + " está em tela cheia exclusiva. Para ver o contador, mude o modo de tela nas "
                         "configurações de vídeo do jogo para Janela sem bordas (ou Janela).",
               "dialog-information");
    }
  }
}

void Daemon::ext_overlay_key_notice(Session &s) {
  if (!s.external || !s.ext_exclusive || !s.cfg.overlay.exclusive_notice) return;
  const uint64_t now = now_ns();
  if (s.ext_key_notice_ns && now - s.ext_key_notice_ns < 30 * kNsPerSec) return;
  s.ext_key_notice_ns = now;
  FRAMUX_LOG("%s: atalho do overlay em tela cheia exclusiva; avisando que o contador não aparece", s.game.c_str());
  const std::string body = s.game + " está em tela cheia exclusiva, e o Windows não mostra o contador sobre o jogo. "
                                    "Mude para Janela sem bordas nas configurações de vídeo do jogo.";
  notify("O contador não aparece neste jogo", body, "dialog-information");
}

// Atalhos de um jogo medido de fora, com o jogo em foco: a mesma leitura de
// estado (GetAsyncKeyState) que a camada faz dentro do jogo.
void Daemon::ext_poll_keys(Session &s) {
  static std::unique_ptr<client::WinInput> input = client::WinInput::open();
  input->snapshot();
  const auto keys = bindings(s.cfg);
  for (int i = 0; i < kActCount; ++i) {
    const bool down = input->pressed(keys[i]);
    if (down == s.ext_keys[i]) continue;
    s.ext_keys[i] = down;
    if (down && input->newly_down(keys[i])) on_key(&s, keys[i].keysym, keys[i].mods, true);
  }
}

void Daemon::ext_shot(Session &s, uint64_t request_id) {
  auto it = shots_.find(request_id);
  if (it == shots_.end()) return;
  PendingShot shot = it->second;
  shots_.erase(it);
  void *hwnd = s.ext_window;
  const std::string game = s.game;
  worker_.post([this, hwnd, shot, request_id, game] {
    std::string error, path;
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    if (WindowCapture::grab(hwnd, px, &w, &h, &error)) {
      RawImage raw;
      raw.data = px.data();
      raw.size = px.size();
      raw.width = w;
      raw.height = h;
      raw.stride = w * 4;
      raw.vk_format = 44;  // VK_FORMAT_B8G8R8A8_UNORM
      RgbImage rgb;
      std::vector<uint8_t> bytes;
      const bool ok =
          convert_image(raw, rgb, &error) && encode_image(rgb, shot.cfg.format, shot.cfg.jpeg_quality, bytes, &error);
      const std::string folder = expand_path(shot.cfg.folder);
      if (ok && make_dirs(folder, &error)) {
        const std::string stem = shot.game + " " + file_timestamp(shot.wall, shot.centis);
        const int out =
            create_unique(folder, stem, std::string(".") + image_format_ext(shot.cfg.format), &path, &error);
        if (out >= 0 && !write_all_close(out, std::string(bytes.begin(), bytes.end()), &error)) path.clear();
      }
    }
    main_queue_.post([this, request_id, path, error, via = shot.via_dbus, sid = shot.session] {
      if (auto s = sessions_.find(sid); s != sessions_.end()) send_indicators(*s->second, ipc::kIndFlash);
      if (!path.empty()) {
        FRAMUX_LOG("screenshot salvo: %s", path.c_str());
        if (dbus_) dbus_->emit_file_saved(request_id, "screenshot", path);
        notify("Screenshot salvo", path, "camera-photo");
      } else {
        FRAMUX_LOG("screenshot falhou: %s", error.c_str());
        if (dbus_ && via) dbus_->emit_error(request_id, error);
        notify("Screenshot falhou", error, "dialog-error");
      }
    });
  });
}

bool Daemon::ext_video_start(Session &s, std::string *err) {
  std::weak_ptr<Recorder> weak = s.rec;
  auto cap = std::make_unique<WindowCapture>();
  // Pela GPU quando o encoder aceita (NVENC, AMF): sem cópia lida pela CPU nem
  // envio de volta, que esperavam atrás do jogo com a GPU cheia.
  WindowCapture::GpuTarget gpu;
  gpu.open = [weak](void *device, uint32_t w, uint32_t h) {
    auto r = weak.lock();
    return r && r->gpu_open(device, w, h);
  };
  gpu.push = [weak](void *texture, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint64_t t) {
    if (auto r = weak.lock()) r->gpu_push(texture, x, y, w, h, t);
  };
  const bool ok = cap->start(
      s.ext_window, s.cfg.video.capture_cursor, uint32_t(s.cfg.video.fps),
      [weak](const uint8_t *bgra, uint32_t w, uint32_t h, uint32_t stride, uint64_t t) {
        auto r = weak.lock();
        if (!r) return;
        // O Recorder converte e devolve o quadro logo depois: uma cópia por quadro.
        auto *copy = new std::vector<uint8_t>(bgra, bgra + size_t(stride) * h);
        r->push_video(copy->data(), w, h, stride, 44 /* B8G8R8A8 */, false, false, t, [copy] { delete copy; });
      },
      err, std::move(gpu));
  if (!ok) return false;
  s.ext_capture = std::move(cap);
  return true;
}

}  // namespace framux::daemon
