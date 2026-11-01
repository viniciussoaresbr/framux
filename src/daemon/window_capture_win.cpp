#include "daemon/window_capture_win.h"

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>

#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <cstring>

#include "common/log.h"
#include "common/win_util.h"

// Windows 11 24H2+: intervalo mínimo entre quadros (o padrão, ~1/60 s, entrega
// só metade dos quadros de um jogo a 75 ou 144 fps). Ausente no MinGW; IID do
// SDK do Windows (o mesmo que o FFmpeg declara em vsrc_gfxcapture).
namespace ABI::Windows::Graphics::Capture {
MIDL_INTERFACE("67C0EA62-1F85-5061-925A-239BE0AC09CB")
IGraphicsCaptureSession5 : public IInspectable {
  virtual HRESULT STDMETHODCALLTYPE get_MinUpdateInterval(ABI::Windows::Foundation::TimeSpan *value) = 0;
  virtual HRESULT STDMETHODCALLTYPE put_MinUpdateInterval(ABI::Windows::Foundation::TimeSpan value) = 0;
};
}  // namespace ABI::Windows::Graphics::Capture
__CRT_UUID_DECL(ABI::Windows::Graphics::Capture::IGraphicsCaptureSession5, 0x67c0ea62, 0x1f85, 0x5061, 0x92, 0x5a,
                0x23, 0x9b, 0xe0, 0xac, 0x09, 0xcb)

namespace framux::daemon {

namespace WGC = ABI::Windows::Graphics::Capture;
namespace WGD = ABI::Windows::Graphics::DirectX;
namespace WGD3D = ABI::Windows::Graphics::DirectX::Direct3D11;

namespace {

template <class T>
struct Com {
  T *p = nullptr;
  Com() = default;
  Com(const Com &) = delete;
  Com &operator=(const Com &) = delete;
  ~Com() { reset(); }
  void reset() {
    if (p) p->Release();
    p = nullptr;
  }
  T **put() {
    reset();
    return &p;
  }
  T *operator->() const { return p; }
  explicit operator bool() const { return p != nullptr; }
};

std::string hr_text(const char *what, HRESULT hr) {
  char buf[48];
  std::snprintf(buf, sizeof buf, " (0x%08lx)", static_cast<unsigned long>(hr));
  return std::string(what) + ": " + win::error_text(uint32_t(hr)) + buf;
}

template <class T>
HRESULT factory(const wchar_t *cls, T **out) {
  HSTRING_HEADER hdr;
  HSTRING name;
  HRESULT hr = WindowsCreateStringReference(cls, UINT32(wcslen(cls)), &hdr, &name);
  if (SUCCEEDED(hr)) hr = RoGetActivationFactory(name, __uuidof(T), reinterpret_cast<void **>(out));
  return hr;
}

}  // namespace

struct WindowCapture::Impl {
  HWND hwnd = nullptr;
  bool cursor = true;
  uint64_t min_period_ns = 0;
  Sink sink;
  GpuTarget gpu;
  int gpu_state = 0;  // 0: a decidir no primeiro quadro; 1: pela GPU; -1: pela memória
  std::atomic<bool> stop{false};
  std::thread thread;

  Com<ID3D11Device> dev;
  Com<ID3D11DeviceContext> ctx;
  Com<WGD3D::IDirect3DDevice> rt_dev;
  Com<WGC::IGraphicsCaptureItem> item;
  Com<WGC::IDirect3D11CaptureFramePool> pool;
  Com<WGC::IGraphicsCaptureSession> session;
  // Fila de texturas de leitura: cada quadro é copiado numa e lido depois,
  // quando a GPU terminar, sem esperar por ela. Com o jogo ocupando a GPU,
  // uma cópia pode levar 50 ms para sair; uma por vez (copia e espera)
  // limitava a gravação a ~19 fps. Com várias em andamento, a taxa só cai se
  // a espera passar de kRing quadros do vídeo.
  // O pool do Windows também: um buffer só volta a ele quando a cópia que lê
  // dele termina na GPU. Com 3 buffers e cópias de ~150 ms (GTX 1650 a 98 %
  // com o jogo), o Windows só conseguia entregar 3 / 0,15 s ≈ 19 fps.
  // Na gravação pela GPU o buffer fica preso até a conversão rodar (~200 ms sem
  // administrador, com o jogo a 98 %): 16 deixam passar ~60 fps.
  static constexpr int kPoolBuffers = 16;
  static constexpr int kRing = 10;
  struct Staging {
    Com<ID3D11Texture2D> tex;
    uint32_t tw = 0, th = 0;          // tamanho da textura
    uint32_t ox = 0, oy = 0, w = 0, h = 0;  // recorte da área cliente
    uint64_t t = 0;                   // horário do quadro
    uint64_t issued = 0;              // quando a cópia foi pedida
  };
  Staging ring[kRing];
  int head = 0, tail = 0, count = 0;  // próxima livre, mais antiga pendente, pendentes
  uint64_t next_due = 0;              // horário do próximo quadro a gravar (com max_fps)
  ABI::Windows::Graphics::SizeInt32 size{};
  std::vector<uint8_t> buf;  // último quadro, BGRA compacto
  uint32_t w = 0, h = 0;

  // Medição (registrada no fim): quantos quadros o Windows entregou, quantos
  // foram repassados e quanto tempo cada etapa da thread levou.
  struct Stat {
    uint64_t n = 0, sum = 0, max = 0;
    void add(uint64_t ns) {
      ++n;
      sum += ns;
      max = std::max(max, ns);
    }
    double avg_ms() const { return n ? double(sum) / double(n) / 1e6 : 0.0; }
    double max_ms() const { return double(max) / 1e6; }
  };
  uint64_t arrived = 0, delivered = 0, throttled = 0, ring_full = 0, first_t = 0, last_t = 0;
  Stat t_copy, t_read, t_sink, t_gpu, gap;
  void log_stats() const;

  bool open(std::string *err);
  void close();
  // Tira um quadro do pool do Windows e pede a cópia dele para a fila (ou o
  // descarta, se o limite de fps não o quer). false: nenhum quadro novo.
  bool take(bool limit);
  // Lê o quadro pendente mais antigo para `buf`. `wait`: espera a GPU;
  // senão devolve false se a cópia ainda não terminou.
  bool poll(bool wait, uint64_t *t_ns);
  void run();
};

bool WindowCapture::Impl::open(std::string *err) {
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, nullptr,
                                 0, D3D11_SDK_VERSION, dev.put(), nullptr, ctx.put());
  if (FAILED(hr)) {
    *err = hr_text("D3D11CreateDevice", hr);
    return false;
  }
  Com<IDXGIDevice> dxgi;
  Com<IInspectable> insp;
  hr = dev->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void **>(dxgi.put()));
  if (SUCCEEDED(hr)) hr = CreateDirect3D11DeviceFromDXGIDevice(dxgi.p, insp.put());
  if (SUCCEEDED(hr))
    hr = insp->QueryInterface(__uuidof(WGD3D::IDirect3DDevice), reinterpret_cast<void **>(rt_dev.put()));
  if (FAILED(hr)) {
    *err = hr_text("dispositivo WinRT", hr);
    return false;
  }
  // Prioridade máxima deste dispositivo na GPU (além da do processo, em
  // main.cpp): as cópias da captura disputam a GPU com o jogo.
  if (HRESULT ph = dxgi->SetGPUThreadPriority(7); FAILED(ph))
    FRAMUX_LOG_ONCE("captura da janela: prioridade de GPU do dispositivo recusada (0x%08lx)", (unsigned long)ph);
  Com<IGraphicsCaptureItemInterop> interop;
  hr = factory(L"Windows.Graphics.Capture.GraphicsCaptureItem", interop.put());
  if (SUCCEEDED(hr))
    hr = interop->CreateForWindow(hwnd, __uuidof(WGC::IGraphicsCaptureItem), reinterpret_cast<void **>(item.put()));
  if (FAILED(hr)) {
    *err = hr_text("captura da janela", hr);
    return false;
  }
  item->get_Size(&size);
  Com<WGC::IDirect3D11CaptureFramePoolStatics2> statics;
  hr = factory(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", statics.put());
  if (SUCCEEDED(hr))
    hr = statics->CreateFreeThreaded(rt_dev.p, WGD::DirectXPixelFormat_B8G8R8A8UIntNormalized, kPoolBuffers, size, pool.put());
  if (SUCCEEDED(hr)) hr = pool->CreateCaptureSession(item.p, session.put());
  if (FAILED(hr)) {
    *err = hr_text("Direct3D11CaptureFramePool", hr);
    return false;
  }
  Com<WGC::IGraphicsCaptureSession2> s2;
  if (SUCCEEDED(session->QueryInterface(__uuidof(WGC::IGraphicsCaptureSession2), reinterpret_cast<void **>(s2.put()))))
    s2->put_IsCursorCaptureEnabled(cursor);
  // Windows 11: sem a borda amarela em volta do jogo.
  Com<WGC::IGraphicsCaptureSession3> s3;
  if (SUCCEEDED(session->QueryInterface(__uuidof(WGC::IGraphicsCaptureSession3), reinterpret_cast<void **>(s3.put()))))
    s3->put_IsBorderRequired(false);
  Com<WGC::IGraphicsCaptureSession5> s5;
  if (SUCCEEDED(session->QueryInterface(__uuidof(WGC::IGraphicsCaptureSession5), reinterpret_cast<void **>(s5.put()))))
    s5->put_MinUpdateInterval(ABI::Windows::Foundation::TimeSpan{10000});  // 1 ms
  hr = session->StartCapture();
  if (FAILED(hr)) {
    *err = hr_text("StartCapture", hr);
    return false;
  }
  return true;
}

void WindowCapture::Impl::close() {
  // Session e pool são IClosable: fechar solta a janela na hora.
  for (IUnknown *u : {static_cast<IUnknown *>(session.p), static_cast<IUnknown *>(pool.p)}) {
    if (!u) continue;
    Com<ABI::Windows::Foundation::IClosable> c;
    if (SUCCEEDED(u->QueryInterface(__uuidof(ABI::Windows::Foundation::IClosable), reinterpret_cast<void **>(c.put()))))
      c->Close();
  }
  session.reset();
  pool.reset();
  item.reset();
  for (Staging &st : ring) st.tex.reset();
  head = tail = count = 0;
  rt_dev.reset();
  ctx.reset();
  dev.reset();
}

bool WindowCapture::Impl::take(bool limit) {
  Com<WGC::IDirect3D11CaptureFrame> frame;
  if (FAILED(pool->TryGetNextFrame(frame.put())) || !frame) return false;
  ABI::Windows::Graphics::SizeInt32 cs{};
  frame->get_ContentSize(&cs);
  ABI::Windows::Foundation::TimeSpan ts{};
  frame->get_SystemRelativeTime(&ts);
  // Unidades de 100 ns do QueryPerformanceCounter: o relógio de now_ns().
  const uint64_t t = ts.Duration > 0 ? uint64_t(ts.Duration) * 100 : now_ns();
  ++arrived;
  if (last_t && t > last_t) gap.add(t - last_t);
  if (!first_t) first_t = t;
  last_t = t;
  if (cs.Width != size.Width || cs.Height != size.Height) {
    // Janela redimensionada: o pool passa a entregar o tamanho novo.
    size = cs;
    pool->Recreate(rt_dev.p, WGD::DirectXPixelFormat_B8G8R8A8UIntNormalized, kPoolBuffers, size);
  }
  // No máximo `max_fps` por segundo, em média, decidido antes da cópia (um
  // jogo a 144 fps num vídeo a 60 não gasta GPU com os quadros que sobram):
  // o próximo quadro vale a partir de meio período antes do horário dele
  // (um jogo a 75 fps num vídeo a 60 entrega 4 de cada 5 quadros, não 1 de 2).
  if (limit && min_period_ns) {
    if (next_due && t + min_period_ns / 2 < next_due) {
      ++throttled;
      return true;
    }
    next_due = (next_due ? next_due : t) + min_period_ns;
    if (next_due + min_period_ns < t) next_due = t + min_period_ns;  // depois de uma pausa
  }
  Com<WGD3D::IDirect3DSurface> surface;
  Com<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
  Com<ID3D11Texture2D> tex;
  if (FAILED(frame->get_Surface(surface.put())) ||
      FAILED(surface->QueryInterface(__uuidof(Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess),
                                     reinterpret_cast<void **>(access.put()))) ||
      FAILED(access->GetInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void **>(tex.put()))))
    return true;
  D3D11_TEXTURE2D_DESC d;
  tex->GetDesc(&d);
  Staging &st = ring[head];
  st.t = t;
  // A textura do pool pode ser maior que o conteúdo logo depois de um resize.
  uint32_t cw = std::min<uint32_t>(d.Width, uint32_t(std::max(cs.Width, 1)));
  uint32_t ch = std::min<uint32_t>(d.Height, uint32_t(std::max(cs.Height, 1)));
  // A captura cobre a janela inteira (barra de título, bordas): recorta a
  // área cliente, que é o que o jogo desenha.
  st.ox = st.oy = 0;
  RECT bounds, client;
  POINT origin{0, 0};
  if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof bounds)) &&
      GetClientRect(hwnd, &client) && ClientToScreen(hwnd, &origin)) {
    const long dx = origin.x - bounds.left, dy = origin.y - bounds.top;
    if (dx >= 0 && dy >= 0 && dx + client.right <= long(cw) && dy + client.bottom <= long(ch) && client.right > 0 &&
        client.bottom > 0) {
      st.ox = uint32_t(dx);
      st.oy = uint32_t(dy);
      cw = uint32_t(client.right);
      ch = uint32_t(client.bottom);
    }
  }
  // Tamanho par: os encoders em 4:2:0 exigem.
  st.w = cw & ~1u;
  st.h = ch & ~1u;
  // Gravação pela GPU: a textura do Windows vai direto ao gravador, que a
  // copia e converte na GPU; nada passa pela memória.
  if (limit && gpu.push) {
    if (gpu_state == 0) {
      gpu_state = gpu.open && gpu.open(dev.p, st.w, st.h) ? 1 : -1;
      FRAMUX_LOG("captura da janela: quadros %s", gpu_state == 1 ? "pela GPU (direto ao encoder)" : "pela memória");
    }
    if (gpu_state == 1) {
      const uint64_t c0 = now_ns();
      gpu.push(tex.p, st.ox, st.oy, st.w, st.h, t);
      t_sink.add(now_ns() - c0);
      ++delivered;
      return true;
    }
  }
  if (!st.tex || st.tw != d.Width || st.th != d.Height) {
    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    if (FAILED(dev->CreateTexture2D(&sd, nullptr, st.tex.put()))) return true;
    st.tw = d.Width;
    st.th = d.Height;
  }
  const uint64_t c0 = now_ns();
  ctx->CopyResource(st.tex.p, tex.p);
  ctx->Flush();
  st.issued = now_ns();
  t_copy.add(st.issued - c0);
  head = (head + 1) % kRing;
  ++count;
  return true;
}

bool WindowCapture::Impl::poll(bool wait, uint64_t *t_ns) {
  if (!count) return false;
  Staging &st = ring[tail];
  const uint64_t r0 = now_ns();
  D3D11_MAPPED_SUBRESOURCE m;
  const HRESULT hr = ctx->Map(st.tex.p, 0, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
  if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
  tail = (tail + 1) % kRing;
  --count;
  if (FAILED(hr)) return false;
  t_gpu.add(r0 - st.issued);
  w = st.w;
  h = st.h;
  const uint32_t stride = w * 4;
  buf.resize(size_t(stride) * h);
  for (uint32_t y = 0; y < h; ++y)
    std::memcpy(buf.data() + size_t(y) * stride,
                static_cast<const uint8_t *>(m.pData) + size_t(y + st.oy) * m.RowPitch + size_t(st.ox) * 4, stride);
  ctx->Unmap(st.tex.p, 0);
  t_read.add(now_ns() - r0);
  *t_ns = st.t;
  return w >= 2 && h >= 2;
}

void WindowCapture::Impl::log_stats() const {
  const double secs = last_t > first_t ? double(last_t - first_t) / 1e9 : 0.0;
  FRAMUX_LOG("captura da janela: %llu quadros do Windows em %.1f s (%.1f fps; maior intervalo %.0f ms), %llu repassados, "
             "%llu pulados pelo limite de fps, fila de cópias cheia %llu vez(es); cópia na GPU %.1f ms "
             "(pior %.0f), leitura %.2f ms (pior %.1f), repasse %.2f ms (pior %.1f)",
             (unsigned long long)arrived, secs, secs > 0 ? double(arrived) / secs : 0.0, gap.max_ms(),
             (unsigned long long)delivered, (unsigned long long)throttled, (unsigned long long)ring_full,
             t_gpu.avg_ms(), t_gpu.max_ms(), t_read.avg_ms(), t_read.max_ms(), t_sink.avg_ms(), t_sink.max_ms());
}

void WindowCapture::Impl::run() {
  SetThreadDescription(GetCurrentThread(), L"framuxd-capture");
  // A thread só espera e copia: acima do normal, para o jogo (que ocupa a
  // CPU) não atrasar a retirada dos quadros do pool do Windows.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  // Espera de ~1 ms entre consultas: o Sleep comum dorme ~15 ms e perderia
  // quadros acima de 60 fps.
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  bool full = false;
  while (!stop) {
    bool busy = false;
    // Pede a cópia de tudo o que chegou, enquanto houver vaga na fila.
    while (count < kRing && take(true)) busy = true;
    if (count == kRing && !full) ++ring_full;
    full = count == kRing;
    // Entrega, em ordem, as cópias que a GPU já terminou.
    uint64_t t = 0;
    while (poll(false, &t)) {
      const uint64_t s0 = now_ns();
      sink(buf.data(), w, h, w * 4, t);
      t_sink.add(now_ns() - s0);
      ++delivered;
      busy = true;
    }
    if (busy) continue;
    LARGE_INTEGER due;
    due.QuadPart = -10000;  // 1 ms, relativo
    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
      WaitForSingleObject(timer, 5);
    else
      Sleep(1);
  }
  if (timer) CloseHandle(timer);
  log_stats();
}

WindowCapture::WindowCapture() = default;
WindowCapture::~WindowCapture() { stop(); }

bool WindowCapture::start(void *hwnd, bool cursor, uint32_t max_fps, Sink sink, std::string *err, GpuTarget gpu) {
  impl_ = std::make_unique<Impl>();
  impl_->hwnd = static_cast<HWND>(hwnd);
  impl_->cursor = cursor;
  impl_->min_period_ns = max_fps ? 1'000'000'000ull / max_fps : 0;
  impl_->sink = std::move(sink);
  impl_->gpu = std::move(gpu);
  // O pool "free threaded" entrega quadros a qualquer thread: abre aqui e lê
  // na thread da captura.
  RoInitialize(RO_INIT_MULTITHREADED);
  std::string e;
  if (!impl_->open(&e)) {
    if (err) *err = e;
    impl_->close();
    impl_.reset();
    return false;
  }
  Impl *im = impl_.get();
  impl_->thread = std::thread([im] {
    RoInitialize(RO_INIT_MULTITHREADED);
    im->run();
    RoUninitialize();
  });
  return true;
}

void WindowCapture::stop() {
  if (!impl_) return;
  impl_->stop = true;
  if (impl_->thread.joinable()) impl_->thread.join();
  impl_->close();
  impl_.reset();
}

bool WindowCapture::grab(void *hwnd, std::vector<uint8_t> &bgra, uint32_t *w, uint32_t *h, std::string *err,
                         uint32_t timeout_ms) {
  RoInitialize(RO_INIT_MULTITHREADED);
  Impl im;
  im.hwnd = static_cast<HWND>(hwnd);
  im.cursor = false;
  std::string e;
  bool ok = im.open(&e);
  if (ok) {
    ok = false;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    uint64_t t;
    while (GetTickCount64() < deadline) {
      if (im.take(false) && im.poll(true, &t)) {
        ok = true;
        break;
      }
      Sleep(5);
    }
    if (!ok) e = "a janela do jogo não entregou nenhum quadro (minimizada?)";
  }
  if (ok) {
    bgra = std::move(im.buf);
    *w = im.w;
    *h = im.h;
  } else if (err) {
    *err = e;
  }
  im.close();
  return ok;
}

bool window_capture_supported() {
  RoInitialize(RO_INIT_MULTITHREADED);
  Com<IGraphicsCaptureItemInterop> interop;
  return SUCCEEDED(factory(L"Windows.Graphics.Capture.GraphicsCaptureItem", interop.put()));
}

}  // namespace framux::daemon
