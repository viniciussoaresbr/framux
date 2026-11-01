// mic_audio.h no Windows: captura WASAPI do dispositivo escolhido (ID do
// endpoint, o valor de [audio] microphone_device) ou do padrão de
// comunicação do sistema. Microfone mono vira estéreo no mixer do Windows.
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include "common/win_util.h"
#include "daemon/mic_audio.h"
#include "daemon/wasapi_capture.h"

namespace framux::daemon {

struct MicAudio::Impl {
  wasapi::Capture capture;
};

std::unique_ptr<MicAudio> MicAudio::start(const std::string &device, Sink sink, std::string *err) {
  // A thread do daemon que chama isto pode não ter COM iniciado.
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator *en = nullptr;
  IMMDevice *dev = nullptr;
  IAudioClient *client = nullptr;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void **>(&en));
  if (SUCCEEDED(hr)) {
    if (device.empty() || device == "default")
      hr = en->GetDefaultAudioEndpoint(eCapture, eCommunications, &dev);
    else
      hr = en->GetDevice(win::widen(device).c_str(), &dev);
  }
  if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client));
  if (dev) dev->Release();
  if (en) en->Release();
  if (SUCCEEDED(co)) CoUninitialize();
  if (FAILED(hr) || !client) {
    if (err) *err = "microfone \"" + device + "\" indisponível: " + win::error_text(uint32_t(hr));
    return nullptr;
  }
  std::unique_ptr<MicAudio> m(new MicAudio());
  m->impl_ = std::make_unique<Impl>();
  if (!m->impl_->capture.start(client, false, std::move(sink), err)) return nullptr;
  return m;
}

MicAudio::~MicAudio() = default;

}  // namespace framux::daemon
