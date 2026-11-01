// ipc_socket.h no Windows: named pipes em modo mensagem, I/O sobreposta.
//
// Formato de cada mensagem: MsgHeader, payload (header.size bytes), número
// de handles (uint32) e os handles (uint64 cada), já válidos no processo de
// destino (o emissor os duplica com DuplicateHandle).
#include <windows.h>
#include <sddl.h>

#include <cstring>
#include <map>
#include <memory>
#include <mutex>

#include "common/ipc_socket.h"
#include "common/log.h"
#include "common/win_util.h"

namespace framux::ipc {

namespace {

struct Conn {
  HANDLE pipe = INVALID_HANDLE_VALUE;
  HANDLE peer = nullptr;  // processo do outro lado, com PROCESS_DUP_HANDLE
  DWORD peer_pid = 0;
  HANDLE read_event = nullptr;
  HANDLE write_event = nullptr;
  std::mutex write_mu;
  ~Conn() {
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    if (peer) CloseHandle(peer);
    if (read_event) CloseHandle(read_event);
    if (write_event) CloseHandle(write_event);
  }
};

std::mutex g_mu;
std::map<int, std::shared_ptr<Conn>> g_conns;
int g_next_id = 1;

std::shared_ptr<Conn> find(int sock) {
  std::lock_guard lk(g_mu);
  auto it = g_conns.find(sock);
  return it == g_conns.end() ? nullptr : it->second;
}

HANDLE h_of(int fd) { return reinterpret_cast<HANDLE>(intptr_t(fd)); }

constexpr size_t kTrailerMax = sizeof(uint32_t) + sizeof(uint64_t) * kMaxFds;
constexpr size_t kBufSize = sizeof(MsgHeader) + kMaxMessage + kTrailerMax;
constexpr DWORD kPipeBuffer = 256 * 1024;

}  // namespace

Message::Message(Message &&o) noexcept : type(o.type), payload(std::move(o.payload)), nfds(o.nfds) {
  for (int i = 0; i < kMaxFds; ++i) {
    fds[i] = o.fds[i];
    o.fds[i] = -1;
  }
  o.nfds = 0;
}

Message &Message::operator=(Message &&o) noexcept {
  if (this != &o) {
    this->~Message();
    new (this) Message(std::move(o));
  }
  return *this;
}

Message::~Message() {
  for (int i = 0; i < kMaxFds; ++i)
    if (fds[i] != -1) CloseHandle(h_of(fds[i]));
}

int Message::take_fd(int i) {
  if (i < 0 || i >= nfds) return -1;
  const int fd = fds[i];
  fds[i] = -1;
  return fd;
}

void set_cloexec_nonblock(int, bool) {}

int pipe_adopt(void *pipe, bool server) {
  auto c = std::make_shared<Conn>();
  c->pipe = static_cast<HANDLE>(pipe);
  ULONG pid = 0;
  if (server)
    GetNamedPipeClientProcessId(c->pipe, &pid);
  else
    GetNamedPipeServerProcessId(c->pipe, &pid);
  c->peer_pid = pid;
  // Sem PROCESS_DUP_HANDLE (o outro lado é elevado) a conexão funciona, só
  // não passa handles para lá.
  if (pid) c->peer = OpenProcess(PROCESS_DUP_HANDLE, FALSE, pid);
  c->read_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  c->write_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  std::lock_guard lk(g_mu);
  const int id = g_next_id++;
  g_conns[id] = std::move(c);
  return id;
}

int pipe_connect(const std::string &name, uint32_t timeout_ms) {
  const std::wstring w = win::widen(name);
  const ULONGLONG deadline = GetTickCount64() + timeout_ms;
  for (;;) {
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      DWORD mode = PIPE_READMODE_MESSAGE;
      SetNamedPipeHandleState(h, &mode, nullptr, nullptr);
      return pipe_adopt(h, false);
    }
    if (GetLastError() != ERROR_PIPE_BUSY) return -1;
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline || !WaitNamedPipeW(w.c_str(), DWORD(deadline - now))) return -1;
  }
}

void pipe_close(int sock) {
  std::shared_ptr<Conn> c;
  {
    std::lock_guard lk(g_mu);
    auto it = g_conns.find(sock);
    if (it == g_conns.end()) return;
    c = std::move(it->second);
    g_conns.erase(it);
  }
  // Acorda quem estiver lendo; o handle fecha quando a última referência sair.
  CancelIoEx(c->pipe, nullptr);
  DisconnectNamedPipe(c->pipe);
}

uint32_t pipe_peer_pid(int sock) {
  auto c = find(sock);
  return c ? c->peer_pid : 0;
}

bool send_msg(int sock, MsgType type, const void *body, size_t body_len, const void *extra, size_t extra_len,
              const int *fds, int nfds, int flags) {
  if (body_len + extra_len > kMaxMessage || nfds > kMaxFds || nfds < 0) return false;
  auto c = find(sock);
  if (!c) return false;
  std::vector<uint8_t> buf(sizeof(MsgHeader) + body_len + extra_len + sizeof(uint32_t) + sizeof(uint64_t) * nfds);
  MsgHeader hdr{uint32_t(type), uint32_t(body_len + extra_len)};
  uint8_t *p = buf.data();
  std::memcpy(p, &hdr, sizeof hdr);
  p += sizeof hdr;
  if (body_len) std::memcpy(p, body, body_len);
  p += body_len;
  if (extra_len) std::memcpy(p, extra, extra_len);
  p += extra_len;
  const uint32_t n = uint32_t(nfds);
  std::memcpy(p, &n, sizeof n);
  p += sizeof n;
  HANDLE remote[kMaxFds] = {};
  auto close_remote = [&](int upto) {
    for (int i = 0; i < upto; ++i)
      if (remote[i]) DuplicateHandle(c->peer, remote[i], nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
  };
  for (int i = 0; i < nfds; ++i) {
    if (!c->peer || !DuplicateHandle(GetCurrentProcess(), h_of(fds[i]), c->peer, &remote[i], 0, FALSE,
                                     DUPLICATE_SAME_ACCESS)) {
      FRAMUX_LOG_ONCE("ipc: não consegui passar um handle ao outro processo (%s)", win::error_text().c_str());
      close_remote(i);
      return false;
    }
    const uint64_t v = uint64_t(uintptr_t(remote[i]));
    std::memcpy(p, &v, sizeof v);
    p += sizeof v;
  }

  std::lock_guard lk(c->write_mu);
  OVERLAPPED ov{};
  ov.hEvent = c->write_event;
  ResetEvent(ov.hEvent);
  DWORD written = 0;
  BOOL ok = WriteFile(c->pipe, buf.data(), DWORD(buf.size()), &written, &ov);
  if (!ok && GetLastError() == ERROR_IO_PENDING) {
    // MSG_DONTWAIT: o daemon nunca espera um jogo travado.
    if (WaitForSingleObject(ov.hEvent, (flags & MSG_DONTWAIT) ? 200 : 5000) != WAIT_OBJECT_0) CancelIoEx(c->pipe, &ov);
    ok = GetOverlappedResult(c->pipe, &ov, &written, TRUE);
  }
  if (!ok || written != buf.size()) {
    close_remote(nfds);
    return false;
  }
  return true;
}

RecvResult recv_msg_wait(int sock, Message &out, void *cancel, uint32_t timeout_ms) {
  out = Message();
  auto c = find(sock);
  if (!c) return RecvResult::Closed;
  std::vector<uint8_t> buf(kBufSize);
  OVERLAPPED ov{};
  ov.hEvent = c->read_event;
  ResetEvent(ov.hEvent);
  DWORD got = 0;
  BOOL ok = ReadFile(c->pipe, buf.data(), DWORD(buf.size()), &got, &ov);
  DWORD err = ok ? 0 : GetLastError();
  if (!ok && err == ERROR_IO_PENDING) {
    HANDLE hs[2] = {ov.hEvent, static_cast<HANDLE>(cancel)};
    const DWORD w = WaitForMultipleObjects(cancel ? 2 : 1, hs, FALSE, timeout_ms);
    if (w != WAIT_OBJECT_0) CancelIoEx(c->pipe, &ov);
    ok = GetOverlappedResult(c->pipe, &ov, &got, TRUE);
    err = ok ? 0 : GetLastError();
    // Cancelado sem dados: nada chegou no prazo.
    if (!ok && err == ERROR_OPERATION_ABORTED) {
      std::lock_guard lk(g_mu);
      return g_conns.count(sock) ? RecvResult::WouldBlock : RecvResult::Closed;
    }
  }
  if (!ok) {
    if (err == ERROR_MORE_DATA) return RecvResult::Error;
    return err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_NO_DATA ? RecvResult::Closed
                                                                                                 : RecvResult::Error;
  }
  if (got == 0) return RecvResult::Closed;
  if (got < sizeof(MsgHeader) + sizeof(uint32_t)) return RecvResult::Error;
  MsgHeader hdr;
  std::memcpy(&hdr, buf.data(), sizeof hdr);
  if (hdr.size > kMaxMessage || sizeof hdr + hdr.size + sizeof(uint32_t) > got) return RecvResult::Error;
  uint32_t n = 0;
  std::memcpy(&n, buf.data() + sizeof hdr + hdr.size, sizeof n);
  if (n > kMaxFds || got != sizeof hdr + hdr.size + sizeof n + sizeof(uint64_t) * n) return RecvResult::Error;
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t v;
    std::memcpy(&v, buf.data() + sizeof hdr + hdr.size + sizeof n + sizeof v * i, sizeof v);
    out.fds[out.nfds++] = int(intptr_t(v));
  }
  out.type = MsgType(hdr.type);
  out.payload.assign(buf.begin() + sizeof hdr, buf.begin() + sizeof hdr + hdr.size);
  return RecvResult::Ok;
}

RecvResult recv_msg(int sock, Message &out, int flags) {
  if (flags & MSG_DONTWAIT) {
    auto c = find(sock);
    if (!c) return RecvResult::Closed;
    DWORD avail = 0;
    if (!PeekNamedPipe(c->pipe, nullptr, 0, nullptr, &avail, nullptr))
      return GetLastError() == ERROR_BROKEN_PIPE ? RecvResult::Closed : RecvResult::Error;
    if (!avail) return RecvResult::WouldBlock;
  }
  // Bloqueante com prazo de 2 s, como o SO_RCVTIMEO do handshake no Linux.
  return recv_msg_wait(sock, out, nullptr, 2000);
}

// ---------------------------------------------------------------------------
// Servidor

void *user_only_security(bool *ok) {
  static SECURITY_ATTRIBUTES sa{};
  static bool good = [] {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    DWORD len = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &len);
    std::vector<uint8_t> info(len);
    bool r = GetTokenInformation(tok, TokenUser, info.data(), len, &len);
    CloseHandle(tok);
    if (!r) return false;
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(info.data())->User.Sid, &sid)) return false;
    // Só o usuário (qualquer nível de integridade) e o SYSTEM.
    const std::wstring sddl = std::wstring(L"D:P(A;;GA;;;") + sid + L")(A;;GA;;;SY)S:(ML;;NW;;;LW)";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
      return false;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = sd;  // vive até o fim do processo
    sa.bInheritHandle = FALSE;
    return true;
  }();
  if (ok) *ok = good;
  return good ? &sa : nullptr;
}

namespace {

HANDLE create_instance(const std::wstring &name, bool first) {
  return CreateNamedPipeW(name.c_str(),
                          PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                          PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                          PIPE_UNLIMITED_INSTANCES, kPipeBuffer, kPipeBuffer, 0,
                          static_cast<SECURITY_ATTRIBUTES *>(user_only_security(nullptr)));
}

}  // namespace

PipeListener::~PipeListener() { stop(); }

bool PipeListener::start(const std::string &name, std::function<void(int)> on_connect, std::string *err) {
  name_ = name;
  on_connect_ = std::move(on_connect);
  HANDLE first = create_instance(win::widen(name), true);
  if (first == INVALID_HANDLE_VALUE) {
    if (err)
      *err = GetLastError() == ERROR_ACCESS_DENIED ? "outro framuxd já está escutando em " + name
                                                   : name + ": " + win::error_text();
    return false;
  }
  first_ = first;
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  thread_ = std::thread([this] { main(); });
  return true;
}

void PipeListener::stop() {
  if (stop_event_) SetEvent(static_cast<HANDLE>(stop_event_));
  if (thread_.joinable()) thread_.join();
  if (stop_event_) CloseHandle(static_cast<HANDLE>(stop_event_));
  stop_event_ = nullptr;
}

void PipeListener::main() {
  const std::wstring wname = win::widen(name_);
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE inst = static_cast<HANDLE>(first_);
  first_ = nullptr;
  for (;;) {
    if (inst == INVALID_HANDLE_VALUE) inst = create_instance(wname, false);
    if (inst == INVALID_HANDLE_VALUE) {
      if (WaitForSingleObject(static_cast<HANDLE>(stop_event_), 1000) == WAIT_OBJECT_0) break;
      continue;
    }
    OVERLAPPED ov{};
    ov.hEvent = ev;
    ResetEvent(ev);
    bool connected = ConnectNamedPipe(inst, &ov);
    if (!connected) {
      const DWORD e = GetLastError();
      if (e == ERROR_PIPE_CONNECTED) {
        connected = true;
      } else if (e == ERROR_IO_PENDING) {
        HANDLE hs[2] = {ev, static_cast<HANDLE>(stop_event_)};
        const DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
        if (w != WAIT_OBJECT_0) {
          CancelIoEx(inst, &ov);
          DWORD dummy;
          GetOverlappedResult(inst, &ov, &dummy, TRUE);
          CloseHandle(inst);
          break;
        }
        DWORD dummy;
        connected = GetOverlappedResult(inst, &ov, &dummy, FALSE);
      }
    }
    if (connected) {
      on_connect_(pipe_adopt(inst, true));
    } else {
      CloseHandle(inst);
    }
    inst = INVALID_HANDLE_VALUE;
  }
  CloseHandle(ev);
}

}  // namespace framux::ipc
