#include "common/control_client.h"

#include "common/control_proto.h"
#include "common/ipc_socket.h"

namespace framux::ctl {

namespace {

Reply parse_reply(const std::string &s) {
  Reply r;
  const auto f = split(s);
  if (f[0] == "OK") {
    r.ok = true;
    for (size_t i = 1; i < f.size(); ++i) r.text += (i > 1 ? "\t" : "") + f[i];
  } else {
    r.error_name = f.size() > 1 ? f[1] : "";
    r.text = f.size() > 2 ? f[2] : "resposta inválida do framuxd";
  }
  return r;
}

}  // namespace

Reply call(const std::vector<std::string> &request, uint32_t timeout_ms) {
  Reply r;
  const int s = ipc::pipe_connect(control_pipe_name(), 500);
  if (s < 0) {
    r.unreachable = true;
    r.text = "o framuxd não está rodando";
    return r;
  }
  const std::string req = join(request);
  ipc::Message m;
  if (!ipc::send_msg(s, kCtlRequest, req.data(), req.size()) ||
      ipc::recv_msg_wait(s, m, nullptr, timeout_ms) != ipc::RecvResult::Ok || m.type != kCtlReply) {
    ipc::pipe_close(s);
    r.text = "sem resposta do framuxd";
    return r;
  }
  ipc::pipe_close(s);
  return parse_reply(std::string(m.payload.begin(), m.payload.end()));
}

Subscription::~Subscription() { close(); }

bool Subscription::open(uint32_t timeout_ms) {
  close();
  sock_ = ipc::pipe_connect(control_pipe_name(), timeout_ms);
  if (sock_ < 0) return false;
  const std::string req = "Subscribe";
  ipc::Message m;
  if (!ipc::send_msg(sock_, kCtlRequest, req.data(), req.size()) ||
      ipc::recv_msg_wait(sock_, m, nullptr, timeout_ms) != ipc::RecvResult::Ok || m.type != kCtlReply) {
    close();
    return false;
  }
  return true;
}

std::vector<std::string> Subscription::next(uint32_t timeout_ms, bool *closed) {
  if (closed) *closed = false;
  if (sock_ < 0) {
    if (closed) *closed = true;
    return {};
  }
  ipc::Message m;
  const auto r = ipc::recv_msg_wait(sock_, m, nullptr, timeout_ms);
  if (r == ipc::RecvResult::WouldBlock) return {};
  if (r != ipc::RecvResult::Ok || m.type != kCtlEvent) {
    if (r != ipc::RecvResult::Ok) {
      close();
      if (closed) *closed = true;
    }
    return {};
  }
  return split(std::string(m.payload.begin(), m.payload.end()));
}

void Subscription::close() {
  if (sock_ >= 0) ipc::pipe_close(sock_);
  sock_ = -1;
}

}  // namespace framux::ctl
