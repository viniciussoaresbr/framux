#include "daemon/overlay_window_win.h"

#include <windows.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace framux::daemon {

namespace {

constexpr UINT kMsgUpdate = WM_APP + 1;
constexpr UINT kMsgQuit = WM_APP + 2;
constexpr const wchar_t *kClass = L"FramuxOverlay";

uint32_t premul(OColor c) {
  auto ch = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  // BGRA pré-multiplicado, alfa total.
  return 0xFF000000u | (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

bool same(const ORect &a, const ORect &b) { return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h; }
bool same(const OColor &a, const OColor &b) { return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a; }
bool same(const std::vector<ORect> &a, const std::vector<ORect> &b) {
  return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](const ORect &x, const ORect &y) { return same(x, y); });
}
bool same(const OverlayDraw &a, const OverlayDraw &b) {
  return a.has_box == b.has_box && (!a.has_box || (same(a.box, b.box) && same(a.box_color, b.box_color))) &&
         same(a.outline_color, b.outline_color) && same(a.text_color, b.text_color) && same(a.outline, b.outline) &&
         same(a.text, b.text);
}

void fill(uint32_t *px, int w, int h, const ORect &r, int ox, int oy, uint32_t color) {
  const int x0 = std::max(0, r.x - ox), y0 = std::max(0, r.y - oy);
  const int x1 = std::min(w, r.x - ox + r.w), y1 = std::min(h, r.y - oy + r.h);
  for (int y = y0; y < y1; ++y) std::fill(px + size_t(y) * w + x0, px + size_t(y) * w + x1, color);
}

}  // namespace

OverlayWindow::~OverlayWindow() { stop(); }

bool OverlayWindow::start() {
  thread_ = std::thread([this] { main(); });
  std::unique_lock lk(mu_);
  started_.wait(lk, [this] { return ready_; });
  return hwnd_ != nullptr;
}

void OverlayWindow::stop() {
  if (!thread_.joinable()) return;
  if (hwnd_) PostMessageW(static_cast<HWND>(hwnd_), kMsgQuit, 0, 0);
  thread_.join();
}

void OverlayWindow::show(void *target, const OverlayDraw &draw) {
  {
    std::lock_guard lk(mu_);
    target_ = target;
    draw_ = draw;
    visible_ = draw.visible;
  }
  if (hwnd_) PostMessageW(static_cast<HWND>(hwnd_), kMsgUpdate, 0, 0);
}

void OverlayWindow::hide() {
  {
    std::lock_guard lk(mu_);
    if (!visible_) return;
    visible_ = false;
  }
  if (hwnd_) PostMessageW(static_cast<HWND>(hwnd_), kMsgUpdate, 0, 0);
}

void OverlayWindow::main() {
  SetThreadDescription(GetCurrentThread(), L"framuxd-overlay");
  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);
  HWND w = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           kClass, L"Framux", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
  // Fora de screenshots e gravações do sistema (Windows 10 2004+).
  // FRAMUX_OVERLAY_IN_CAPTURES=1 deixa aparecer (testes com print da tela).
  const char *in_captures = std::getenv("FRAMUX_OVERLAY_IN_CAPTURES");
  if (w && !(in_captures && *in_captures == '1')) SetWindowDisplayAffinity(w, WDA_EXCLUDEFROMCAPTURE);
  {
    std::lock_guard lk(mu_);
    hwnd_ = w;
    ready_ = true;
  }
  started_.notify_all();
  if (!w) return;
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == kMsgQuit) break;
    if (msg.message == kMsgUpdate) {
      render();
      continue;
    }
    DispatchMessageW(&msg);
  }
  DestroyWindow(w);
  UnregisterClassW(kClass, wc.hInstance);
}

void OverlayWindow::render() {
  OverlayDraw d;
  HWND target;
  bool visible;
  {
    std::lock_guard lk(mu_);
    d = draw_;
    target = static_cast<HWND>(target_);
    visible = visible_;
  }
  HWND self = static_cast<HWND>(hwnd_);
  auto hide_self = [&] {
    if (shown_) ShowWindow(self, SW_HIDE);
    shown_ = false;
  };
  if (!visible || !target || !IsWindow(target) || IsIconic(target)) {
    hide_self();
    return;
  }
  // Caixa que envolve todos os retângulos: a janela tem só esse tamanho.
  int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
  auto grow = [&](const ORect &r) {
    x0 = std::min(x0, r.x);
    y0 = std::min(y0, r.y);
    x1 = std::max(x1, r.x + r.w);
    y1 = std::max(y1, r.y + r.h);
  };
  if (d.has_box) grow(d.box);
  for (const ORect &r : d.outline) grow(r);
  for (const ORect &r : d.text) grow(r);
  if (x1 <= x0 || y1 <= y0) {
    hide_self();
    return;
  }
  POINT origin{0, 0};
  ClientToScreen(target, &origin);
  POINT pos{origin.x + x0, origin.y + y0};
  const unsigned long long tick = GetTickCount64();
  // Mesmo conteúdo no mesmo lugar: só reforça o topo de vez em quando (outra
  // janela "sempre no topo" pode ter passado à frente).
  if (shown_ && pos.x == shown_x_ && pos.y == shown_y_ && same(d, shown_draw_)) {
    if (tick - topmost_at_ >= 1000) {
      topmost_at_ = tick;
      SetWindowPos(self, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    return;
  }
  const int w = x1 - x0, h = y1 - y0;
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof bi.bmiHeader;
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;  // de cima para baixo
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void *bits = nullptr;
  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(screen);
  HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bmp) {
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return;
  }
  auto *px = static_cast<uint32_t *>(bits);
  std::memset(px, 0, size_t(w) * h * 4);
  if (d.has_box) fill(px, w, h, d.box, x0, y0, premul(d.box_color));
  for (const ORect &r : d.outline) fill(px, w, h, r, x0, y0, premul(d.outline_color));
  for (const ORect &r : d.text) fill(px, w, h, r, x0, y0, premul(d.text_color));
  HGDIOBJ old = SelectObject(mem, bmp);
  SIZE size{w, h};
  POINT src{0, 0};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  UpdateLayeredWindow(self, screen, &pos, &size, mem, &src, 0, &blend, ULW_ALPHA);
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
  if (!shown_ || tick - topmost_at_ >= 1000) {
    topmost_at_ = tick;
    SetWindowPos(self, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  }
  shown_ = true;
  shown_draw_ = d;
  shown_x_ = pos.x;
  shown_y_ = pos.y;
}

}  // namespace framux::daemon
