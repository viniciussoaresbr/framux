#include "daemon/wasapi_capture.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmreg.h>

#include <vector>

#include "common/log.h"
#include "common/win_util.h"

namespace framux::daemon::wasapi {

namespace {

std::string hr_text(HRESULT hr) {
  char buf[32];
  std::snprintf(buf, sizeof buf, " (0x%08lx)", static_cast<unsigned long>(hr));
  return win::error_text(uint32_t(hr)) + buf;
}

}  // namespace

Capture::~Capture() { stop(); }

bool Capture::start(IAudioClient *client, bool loopback, Sink sink, std::string *err) {
  client_ = client;
  sink_ = std::move(sink);
  WAVEFORMATEXTENSIBLE fmt{};
  fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
  fmt.Format.nChannels = kChannels;
  fmt.Format.nSamplesPerSec = kRate;
  fmt.Format.wBitsPerSample = 32;
  fmt.Format.nBlockAlign = kChannels * 4;
  fmt.Format.nAvgBytesPerSec = kRate * fmt.Format.nBlockAlign;
  fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
  fmt.Samples.wValidBitsPerSample = 32;
  fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
  fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
  // O mixer do Windows converte formato, taxa e canais (mono → estéreo).
  DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
  if (loopback) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
  HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200 * 10000 /* 200 ms */, 0,
                                   reinterpret_cast<WAVEFORMATEX *>(&fmt), nullptr);
  if (FAILED(hr)) {
    if (err) *err = "IAudioClient::Initialize: " + hr_text(hr);
    return false;
  }
  event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  hr = client_->SetEventHandle(static_cast<HANDLE>(event_));
  if (SUCCEEDED(hr)) hr = client_->Start();
  if (FAILED(hr)) {
    if (err) *err = "IAudioClient::Start: " + hr_text(hr);
    return false;
  }
  thread_ = std::thread([this] { main(); });
  return true;
}

void Capture::stop() {
  stop_ = true;
  if (event_) SetEvent(static_cast<HANDLE>(event_));
  if (thread_.joinable()) thread_.join();
  if (client_) {
    client_->Stop();
    client_->Release();
    client_ = nullptr;
  }
  if (event_) CloseHandle(static_cast<HANDLE>(event_));
  event_ = nullptr;
}

void Capture::main() {
  SetThreadDescription(GetCurrentThread(), L"framuxd-audio");
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  DWORD task = 0;
  HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &task);
  IAudioCaptureClient *cap = nullptr;
  if (FAILED(client_->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void **>(&cap)))) {
    FRAMUX_LOG("áudio: sem IAudioCaptureClient");
    CoUninitialize();
    return;
  }
  std::vector<float> silence;
  while (!stop_) {
    WaitForSingleObject(static_cast<HANDLE>(event_), 500);
    for (;;) {
      BYTE *data = nullptr;
      UINT32 frames = 0;
      DWORD flags = 0;
      UINT64 qpc = 0;
      const HRESULT hr = cap->GetBuffer(&data, &frames, &flags, nullptr, &qpc);
      if (hr == AUDCLNT_S_BUFFER_EMPTY || FAILED(hr) || frames == 0) {
        if (SUCCEEDED(hr) && frames == 0 && hr != AUDCLNT_S_BUFFER_EMPTY) cap->ReleaseBuffer(0);
        break;
      }
      // qpc vem em unidades de 100 ns do QueryPerformanceCounter: o mesmo
      // relógio de now_ns().
      const uint64_t t = qpc ? qpc * 100 : now_ns();
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        silence.assign(size_t(frames) * kChannels, 0.0f);
        sink_(silence.data(), frames, t);
      } else {
        sink_(reinterpret_cast<const float *>(data), frames, t);
      }
      cap->ReleaseBuffer(frames);
    }
  }
  cap->Release();
  if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
  CoUninitialize();
}

}  // namespace framux::daemon::wasapi
