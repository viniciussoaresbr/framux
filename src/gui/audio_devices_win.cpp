// audio_devices.h no Windows: microfones pelo MMDevice API (o "name" é o ID
// do endpoint, o valor de [audio] microphone_device) e o medidor de nível
// pelo IAudioMeterInformation, com um stream de captura aberto enquanto o
// medidor está visível (sem stream, o Windows não mede a entrada).
#include "gui/audio_devices.h"

#include <windows.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>

#include <condition_variable>
#include <mutex>
#include <thread>

#include "common/win_util.h"

// endpointvolume.h do MinGW só declara a interface; ABI documentado.
MIDL_INTERFACE("C02216F6-8C67-4B5B-9D00-D008E73E0064")
IAudioMeterInformation : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetPeakValue(float *peak) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetMeteringChannelCount(UINT *count) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetChannelsPeakValues(UINT32 count, float *peaks) = 0;
  virtual HRESULT STDMETHODCALLTYPE QueryHardwareSupport(DWORD *mask) = 0;
};
__CRT_UUID_DECL(IAudioMeterInformation, 0xC02216F6, 0x8C67, 0x4B5B, 0x9D, 0x00, 0xD0, 0x08, 0xE7, 0x3E, 0x00, 0x64)

namespace framux::gui {

struct AudioDevices::Impl {
  std::mutex mu;
  std::condition_variable cv;
  std::vector<Source> sources;  // mu
  std::atomic<uint64_t> revision{0};
  std::atomic<float> peak{0};
  std::string want;  // mu: dispositivo a medir ("" = nenhum)
  bool stop = false;
  bool ok = false;
  std::thread thread;

  void main();
};

namespace {

template <class T>
void release(T *&p) {
  if (p) p->Release();
  p = nullptr;
}

std::vector<AudioDevices::Source> enumerate(IMMDeviceEnumerator *en) {
  std::vector<AudioDevices::Source> out;
  IMMDeviceCollection *col = nullptr;
  if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) return out;
  UINT n = 0;
  col->GetCount(&n);
  for (UINT i = 0; i < n; ++i) {
    IMMDevice *dev = nullptr;
    if (FAILED(col->Item(i, &dev))) continue;
    LPWSTR id = nullptr;
    IPropertyStore *props = nullptr;
    AudioDevices::Source s;
    if (SUCCEEDED(dev->GetId(&id))) s.name = win::narrow(id);
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
      PROPVARIANT v;
      PropVariantInit(&v);
      if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR)
        s.description = win::narrow(v.pwszVal);
      PropVariantClear(&v);
      props->Release();
    }
    CoTaskMemFree(id);
    dev->Release();
    if (!s.name.empty()) out.push_back(std::move(s));
  }
  col->Release();
  return out;
}

bool same(const std::vector<AudioDevices::Source> &a, const std::vector<AudioDevices::Source> &b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (a[i].name != b[i].name || a[i].description != b[i].description) return false;
  return true;
}

}  // namespace

// Thread própria (COM MTA): reenumera a cada 2 s e, com um dispositivo
// pedido, lê o pico a cada 30 ms.
void AudioDevices::Impl::main() {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  IMMDeviceEnumerator *en = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                              reinterpret_cast<void **>(&en)))) {
    CoUninitialize();
    return;
  }
  {
    std::lock_guard lk(mu);
    ok = true;
  }
  std::string current;
  IAudioClient *client = nullptr;
  IAudioMeterInformation *meter = nullptr;
  ULONGLONG next_enum = 0;
  for (;;) {
    std::string target;
    {
      std::unique_lock lk(mu);
      cv.wait_for(lk, std::chrono::milliseconds(30), [this] { return stop; });
      if (stop) break;
      target = want;
    }
    if (GetTickCount64() >= next_enum) {
      next_enum = GetTickCount64() + 2000;
      auto list = enumerate(en);
      std::lock_guard lk(mu);
      if (!same(list, sources)) {
        sources = std::move(list);
        ++revision;
      }
    }
    if (target != current) {
      if (client) client->Stop();
      release(meter);
      release(client);
      current = target;
      if (!target.empty()) {
        IMMDevice *dev = nullptr;
        const HRESULT hr = target == "default" ? en->GetDefaultAudioEndpoint(eCapture, eCommunications, &dev)
                                               : en->GetDevice(win::widen(target).c_str(), &dev);
        if (SUCCEEDED(hr)) {
          dev->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&meter));
          if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client)))) {
            WAVEFORMATEX *mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix)) &&
                SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, mix, nullptr)))
              client->Start();
            CoTaskMemFree(mix);
          }
          dev->Release();
        }
      }
    }
    if (meter) {
      float p = 0;
      if (SUCCEEDED(meter->GetPeakValue(&p)) && p > peak.load()) peak.store(p);
    }
  }
  if (client) client->Stop();
  release(meter);
  release(client);
  en->Release();
  CoUninitialize();
}

AudioDevices::AudioDevices() : impl_(std::make_unique<Impl>()) {
  impl_->thread = std::thread([this] { impl_->main(); });
}

AudioDevices::~AudioDevices() {
  {
    std::lock_guard lk(impl_->mu);
    impl_->stop = true;
  }
  impl_->cv.notify_all();
  if (impl_->thread.joinable()) impl_->thread.join();
}

bool AudioDevices::available() const {
  std::lock_guard lk(impl_->mu);
  return impl_->ok;
}

uint64_t AudioDevices::revision() const { return impl_->revision.load(); }

std::vector<AudioDevices::Source> AudioDevices::sources() const {
  std::lock_guard lk(impl_->mu);
  return impl_->sources;
}

void AudioDevices::monitor(const std::string &device) {
  std::lock_guard lk(impl_->mu);
  impl_->want = device;
}

float AudioDevices::take_peak() { return impl_->peak.exchange(0); }

}  // namespace framux::gui
