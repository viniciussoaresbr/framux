#include "common/ipc_socket.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace framux::ipc {

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
    if (fds[i] >= 0) close(fds[i]);
}

int Message::take_fd(int i) {
  if (i < 0 || i >= nfds) return -1;
  const int fd = fds[i];
  fds[i] = -1;
  return fd;
}

bool send_msg(int sock, MsgType type, const void *body, size_t body_len, const void *extra, size_t extra_len,
              const int *fds, int nfds, int flags) {
  if (body_len + extra_len > kMaxMessage || nfds > kMaxFds) return false;
  MsgHeader hdr{uint32_t(type), uint32_t(body_len + extra_len)};
  iovec iov[3];
  int niov = 0;
  iov[niov++] = {&hdr, sizeof hdr};
  if (body_len) iov[niov++] = {const_cast<void *>(body), body_len};
  if (extra_len) iov[niov++] = {const_cast<void *>(extra), extra_len};

  msghdr msg{};
  msg.msg_iov = iov;
  msg.msg_iovlen = size_t(niov);
  alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * kMaxFds)];
  if (nfds > 0) {
    std::memset(cbuf, 0, sizeof cbuf);
    msg.msg_control = cbuf;
    msg.msg_controllen = CMSG_SPACE(sizeof(int) * size_t(nfds));
    cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int) * size_t(nfds));
    std::memcpy(CMSG_DATA(c), fds, sizeof(int) * size_t(nfds));
  }
  ssize_t r;
  do {
    r = sendmsg(sock, &msg, flags | MSG_NOSIGNAL);
  } while (r < 0 && errno == EINTR);
  return r == ssize_t(sizeof hdr + body_len + extra_len);
}

RecvResult recv_msg(int sock, Message &out, int flags) {
  out = Message();
  std::vector<uint8_t> buf(sizeof(MsgHeader) + kMaxMessage);
  iovec iov{buf.data(), buf.size()};
  alignas(cmsghdr) char cbuf[CMSG_SPACE(sizeof(int) * kMaxFds)];
  msghdr msg{};
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cbuf;
  msg.msg_controllen = sizeof cbuf;

  ssize_t r;
  do {
    r = recvmsg(sock, &msg, flags | MSG_CMSG_CLOEXEC);
  } while (r < 0 && errno == EINTR);
  if (r < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? RecvResult::WouldBlock : RecvResult::Error;
  if (r == 0) return RecvResult::Closed;

  // Recolhe os fds antes de qualquer validação, para não vazá-los.
  for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
    const size_t n = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
    const int *p = reinterpret_cast<const int *>(CMSG_DATA(c));
    for (size_t i = 0; i < n; ++i) {
      if (out.nfds < kMaxFds)
        out.fds[out.nfds++] = p[i];
      else
        close(p[i]);
    }
  }
  if (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) return RecvResult::Error;
  if (size_t(r) < sizeof(MsgHeader)) return RecvResult::Error;
  MsgHeader hdr;
  std::memcpy(&hdr, buf.data(), sizeof hdr);
  if (hdr.size != size_t(r) - sizeof(MsgHeader)) return RecvResult::Error;
  out.type = MsgType(hdr.type);
  out.payload.assign(buf.begin() + sizeof(MsgHeader), buf.begin() + r);
  return RecvResult::Ok;
}

void set_cloexec_nonblock(int fd, bool nonblock) {
  fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
  int fl = fcntl(fd, F_GETFL);
  fcntl(fd, F_SETFL, nonblock ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

}  // namespace framux::ipc
