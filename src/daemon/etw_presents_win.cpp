#include "daemon/etw_presents_win.h"

#include <windows.h>
#include <evntcons.h>
#include <evntrace.h>

#include <cstring>
#include <vector>

#include "common/log.h"
#include "common/win_util.h"

namespace framux::daemon {

namespace {

constexpr const wchar_t *kSessionName = L"Framux-Presents";

// GUIDs e ids públicos dos provedores (os mesmos que o PresentMon usa).
constexpr GUID kDxgi = {0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};
constexpr GUID kD3d9 = {0x783ACA0A, 0x790E, 0x4D7F, {0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9}};
constexpr GUID kDxgKrnl = {0x802EC45A, 0x1E99, 0x4B83, {0x99, 0x20, 0x87, 0xC9, 0x82, 0x77, 0xBA, 0x9D}};
constexpr USHORT kDxgiPresentStart = 42;
constexpr USHORT kDxgiPresentMpoStart = 55;
constexpr USHORT kD3d9PresentStart = 1;
constexpr USHORT kKrnlPresentInfo = 184;
constexpr ULONGLONG kKrnlKeywordBase = 0x1;
constexpr uint32_t kDxgiPresentTest = 0x1;  // DXGI_PRESENT_TEST: não apresenta nada

// evntprov.h do MinGW não tem o filtro por id de evento (Windows 8.1+).
constexpr ULONG kFilterTypeEventId = 0x80000200;  // EVENT_FILTER_TYPE_EVENT_ID
struct FilterEventId {                            // EVENT_FILTER_EVENT_ID
  BOOLEAN FilterIn;
  UCHAR Reserved;
  USHORT Count;
  USHORT Events[1];
};

struct Props {
  EVENT_TRACE_PROPERTIES p;
  wchar_t name[64];
};

void init_props(Props &pr) {
  std::memset(&pr, 0, sizeof pr);
  pr.p.Wnode.BufferSize = sizeof pr;
  pr.p.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
  pr.p.Wnode.ClientContext = 1;  // timestamps em QPC, o relógio de now_ns()
  pr.p.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
  pr.p.LoggerNameOffset = offsetof(Props, name);
  pr.p.FlushTimer = 1;
}

void WINAPI on_record(PEVENT_RECORD r) { static_cast<PresentTracer *>(r->UserContext)->on_event(r); }

// Habilita só os eventos de present do provedor (filtro por id no kernel).
ULONG enable(TRACEHANDLE h, const GUID &provider, std::initializer_list<USHORT> ids, ULONGLONG keywords) {
  std::vector<uint8_t> buf(offsetof(FilterEventId, Events) + sizeof(USHORT) * ids.size());
  auto *f = reinterpret_cast<FilterEventId *>(buf.data());
  f->FilterIn = TRUE;
  f->Count = USHORT(ids.size());
  USHORT *out = f->Events;
  for (USHORT id : ids) *out++ = id;
  EVENT_FILTER_DESCRIPTOR desc{};
  desc.Ptr = ULONGLONG(uintptr_t(buf.data()));
  desc.Size = ULONG(buf.size());
  desc.Type = kFilterTypeEventId;
  ENABLE_TRACE_PARAMETERS params{};
  params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
  params.EnableFilterDesc = &desc;
  params.FilterDescCount = 1;
  // Nível detalhado: alguns eventos de present são publicados nele.
  return EnableTraceEx2(h, &provider, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_VERBOSE, keywords, 0, 0,
                        &params);
}

}  // namespace

PresentTracer::~PresentTracer() { stop(); }

bool PresentTracer::start(Sink sink, std::string *err) {
  sink_ = std::move(sink);
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  qpc_freq_ = uint64_t(f.QuadPart);

  Props pr;
  init_props(pr);
  // Sessão órfã de um framuxd que caiu: encerra e cria de novo.
  ControlTraceW(0, kSessionName, &pr.p, EVENT_TRACE_CONTROL_STOP);
  init_props(pr);
  TRACEHANDLE session = 0;
  ULONG rc = StartTraceW(&session, kSessionName, &pr.p);
  if (rc != ERROR_SUCCESS) {
    if (err)
      *err = rc == ERROR_ACCESS_DENIED
                 ? "sem permissão para ler os eventos de present (entre no grupo \"Usuários do log de desempenho\")"
                 : "StartTrace: " + win::error_text(rc);
    return false;
  }
  session_ = session;
  for (const auto &[name, rc2] : {std::pair{"DXGI", enable(session, kDxgi, {kDxgiPresentStart, kDxgiPresentMpoStart}, 0)},
                                  std::pair{"D3D9", enable(session, kD3d9, {kD3d9PresentStart}, 0)},
                                  std::pair{"DxgKrnl", enable(session, kDxgKrnl, {kKrnlPresentInfo}, kKrnlKeywordBase)}})
    if (rc2 != ERROR_SUCCESS) FRAMUX_LOG("ETW: provedor %s não habilitado: %s", name, win::error_text(rc2).c_str());

  EVENT_TRACE_LOGFILEW lf{};
  lf.LoggerName = const_cast<wchar_t *>(kSessionName);
  lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
  lf.EventRecordCallback = on_record;
  lf.Context = this;
  const TRACEHANDLE trace = OpenTraceW(&lf);
  if (trace == INVALID_PROCESSTRACE_HANDLE) {
    if (err) *err = "OpenTrace: " + win::error_text();
    stop();
    return false;
  }
  trace_ = trace;
  running_ = true;
  thread_ = std::thread([this] { main(); });
  return true;
}

void PresentTracer::main() {
  SetThreadDescription(GetCurrentThread(), L"framuxd-etw");
  TRACEHANDLE t = trace_;
  ProcessTrace(&t, 1, nullptr, nullptr);  // volta quando a sessão é encerrada
}

void PresentTracer::stop() {
  running_ = false;
  if (session_) {
    Props pr;
    init_props(pr);
    ControlTraceW(session_, nullptr, &pr.p, EVENT_TRACE_CONTROL_STOP);
    session_ = 0;
  }
  if (trace_) {
    CloseTrace(trace_);
    trace_ = 0;
  }
  if (thread_.joinable()) thread_.join();
}

void PresentTracer::on_event(const void *rec) {
  const auto *r = static_cast<const EVENT_RECORD *>(rec);
  events_.fetch_add(1, std::memory_order_relaxed);
  const EVENT_HEADER &h = r->EventHeader;
  const uint64_t qpc = uint64_t(h.TimeStamp.QuadPart);
  const uint64_t t = qpc / qpc_freq_ * 1'000'000'000ull + qpc % qpc_freq_ * 1'000'000'000ull / qpc_freq_;
  const USHORT id = h.EventDescriptor.Id;
  if (h.ProviderId == kDxgi && (id == kDxgiPresentStart || id == kDxgiPresentMpoStart)) {
    // Present_Start: pIDXGISwapChain (ponteiro do processo), Flags, SyncInterval.
    const size_t ptr = (h.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
    uint64_t sc = 0;
    uint32_t flags = 0;
    if (r->UserDataLength >= ptr + 4) {
      std::memcpy(&sc, r->UserData, ptr);
      std::memcpy(&flags, static_cast<const uint8_t *>(r->UserData) + ptr, 4);
    }
    if (flags & kDxgiPresentTest) return;
    sink_(h.ProcessId, PresentSource::Dxgi, sc, t);
  } else if (h.ProviderId == kD3d9 && id == kD3d9PresentStart) {
    uint64_t sc = 0;
    const size_t ptr = (h.Flags & EVENT_HEADER_FLAG_32_BIT_HEADER) ? 4 : 8;
    if (r->UserDataLength >= ptr) std::memcpy(&sc, r->UserData, ptr);
    sink_(h.ProcessId, PresentSource::D3d9, sc, t);
  } else if (h.ProviderId == kDxgKrnl && id == kKrnlPresentInfo) {
    sink_(h.ProcessId, PresentSource::Kernel, 0, t);
  }
}

}  // namespace framux::daemon
