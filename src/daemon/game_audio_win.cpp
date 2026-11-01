// game_audio.h no Windows: loopback de processo do WASAPI (Windows 10 2004+),
// que captura só o som do jogo e dos processos filhos dele, sem misturar o
// resto do sistema — o equivalente às ligações manuais no PipeWire.
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propidl.h>

#include <atomic>

#include "common/log.h"
#include "daemon/game_audio.h"
#include "daemon/wasapi_capture.h"

namespace framux::daemon {

namespace {

// audioclientactivationparams.h (ausente no MinGW): ABI documentado.
enum AUDIOCLIENT_ACTIVATION_TYPE { AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0, AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK };
enum PROCESS_LOOPBACK_MODE {
  PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
  PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
};
struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
  DWORD TargetProcessId;
  PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
};
struct AUDIOCLIENT_ACTIVATION_PARAMS {
  AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
  union {
    AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
  };
};
constexpr const wchar_t *kProcessLoopbackDevice = L"VAD\\Process_Loopback";

// Recebe o resultado da ativação assíncrona. IAgileObject: o WASAPI pode
// chamar de qualquer thread.
class Activation : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
 public:
  Activation() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
  virtual ~Activation() { CloseHandle(done_); }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **out) override {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
      *out = static_cast<IActivateAudioInterfaceCompletionHandler *>(this);
    } else if (iid == __uuidof(IAgileObject)) {
      *out = static_cast<IAgileObject *>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG r = --refs_;
    if (r == 0) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation *op) override {
    IUnknown *unk = nullptr;
    HRESULT act = E_FAIL;
    result_ = op->GetActivateResult(&act, &unk);
    if (SUCCEEDED(result_)) result_ = act;
    if (SUCCEEDED(result_) && unk) unk->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void **>(&client_));
    if (unk) unk->Release();
    SetEvent(done_);
    return S_OK;
  }

  // Espera a ativação; devolve o cliente (com referência) ou nullptr.
  IAudioClient *wait(HRESULT *hr) {
    if (WaitForSingleObject(done_, 5000) != WAIT_OBJECT_0) {
      *hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      return nullptr;
    }
    *hr = result_;
    IAudioClient *c = client_;
    client_ = nullptr;
    return c;
  }

 private:
  std::atomic<ULONG> refs_{1};
  HANDLE done_;
  HRESULT result_ = E_FAIL;
  IAudioClient *client_ = nullptr;
};

}  // namespace

struct GameAudio::Impl {
  wasapi::Capture capture;
};

std::unique_ptr<GameAudio> GameAudio::start(int pid, int peer_pid, Sink sink, std::string *err) {
  // No Windows os dois são o mesmo processo; o informado pelo jogo vale.
  const DWORD target = DWORD(pid ? pid : peer_pid);
  AUDIOCLIENT_ACTIVATION_PARAMS params{};
  params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  params.ProcessLoopbackParams.TargetProcessId = target;
  params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
  PROPVARIANT pv{};
  pv.vt = VT_BLOB;
  pv.blob.cbSize = sizeof params;
  pv.blob.pBlobData = reinterpret_cast<BYTE *>(&params);

  auto *handler = new Activation();
  IActivateAudioInterfaceAsyncOperation *op = nullptr;
  HRESULT hr = ActivateAudioInterfaceAsync(kProcessLoopbackDevice, __uuidof(IAudioClient), &pv, handler, &op);
  IAudioClient *client = nullptr;
  if (SUCCEEDED(hr)) client = handler->wait(&hr);
  if (op) op->Release();
  handler->Release();
  if (!client) {
    if (err) {
      char buf[96];
      std::snprintf(buf, sizeof buf, "loopback do processo %lu indisponível (0x%08lx; requer Windows 10 2004+)",
                    static_cast<unsigned long>(target), static_cast<unsigned long>(hr));
      *err = buf;
    }
    return nullptr;
  }
  std::unique_ptr<GameAudio> g(new GameAudio());
  g->impl_ = std::make_unique<Impl>();
  if (!g->impl_->capture.start(client, true, std::move(sink), err)) return nullptr;
  return g;
}

GameAudio::~GameAudio() = default;

}  // namespace framux::daemon
