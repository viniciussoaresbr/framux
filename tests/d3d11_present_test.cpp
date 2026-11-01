// App Direct3D 11 mínimo (janela + swapchain flip + clear) para testar a
// medição e a captura externas do Framux no Windows (ETW + Windows Graphics
// Capture). Cor animada, para a captura ter conteúdo que muda.
// uso: d3d11_present_test [segundos] [janela|semborda|exclusiva] [flip|legado] [carga]
//   legado: swapchain DXGI_SWAP_EFFECT_DISCARD (modelo bitblt), como jogos D3D11 antigos
//   carga: N clears de tela cheia extras por quadro, sem vsync (GPU ocupada, como um jogo)
//   (padrão: roda até ser fechado, em janela)
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

int main(int argc, char **argv) {
  const double seconds = argc > 1 && std::atof(argv[1]) > 0 ? std::atof(argv[1]) : 1e9;
  const char *mode = argc > 2 ? argv[2] : "janela";
  const bool borderless = !std::strcmp(mode, "semborda"), exclusive = !std::strcmp(mode, "exclusiva");
  const bool legacy = argc > 3 && !std::strcmp(argv[3], "legado");
  const int load = argc > 4 ? std::atoi(argv[4]) : 0;
  WNDCLASSW wc{};
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"d3d11_present_test";
  RegisterClassW(&wc);
  RECT rc{0, 0, 640, 480};
  AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"d3d11_present_test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT,
                            CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance,
                            nullptr);
  if (!hwnd) return 77;
  if (borderless || exclusive) {
    // Janela sem bordas do tamanho do monitor (o modo "tela cheia em janela").
    MONITORINFO mi{sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
  }

  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.BufferCount = legacy ? 1 : 2;
  sd.SwapEffect = legacy ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD;
  ID3D11Device *dev = nullptr;
  ID3D11DeviceContext *ctx = nullptr;
  IDXGISwapChain *sc = nullptr;
  if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                           D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx)))
    return 77;
  if (exclusive) {
    // A tela cheia exclusiva exige a janela em primeiro plano; aberto por um
    // script, o Windows não a põe lá sozinho (um Alt "solto" libera a troca).
    HRESULT fs = E_FAIL;
    for (int i = 0; i < 20 && FAILED(fs); ++i) {
      keybd_event(VK_MENU, 0, 0, 0);
      keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
      SetForegroundWindow(hwnd);
      for (MSG msg; PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);) DispatchMessageW(&msg);
      fs = sc->SetFullscreenState(TRUE, nullptr);
      if (FAILED(fs)) Sleep(100);
    }
    if (FAILED(fs)) std::printf("SetFullscreenState falhou\n");
    sc->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
  }
  ID3D11Texture2D *back = nullptr;
  sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&back));
  ID3D11RenderTargetView *rtv = nullptr;
  dev->CreateRenderTargetView(back, nullptr, &rtv);
  back->Release();

  const auto start = std::chrono::steady_clock::now();
  unsigned frames = 0;
  for (;;) {
    for (MSG msg; PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);) DispatchMessageW(&msg);
    if (!IsWindow(hwnd)) break;
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (t >= seconds) break;
    const float c[4] = {0.5f + 0.5f * float(std::sin(t)), 0.2f, 0.5f + 0.5f * float(std::cos(t)), 1.0f};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ctx->ClearRenderTargetView(rtv, c);
    // Cores diferentes: o driver não junta clears iguais seguidos.
    for (int i = 0; i < load; ++i) {
      const float k[4] = {c[0], float(i & 255) / 255.0f, c[2], 1.0f};
      ctx->ClearRenderTargetView(rtv, k);
    }
    ctx->ClearRenderTargetView(rtv, c);
    sc->Present(load > 0 ? 0 : 1, 0);
    ++frames;
  }
  std::printf("%u quadros\n", frames);
  rtv->Release();
  sc->Release();
  ctx->Release();
  dev->Release();
  return 0;
}
