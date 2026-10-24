// Envio/recebimento de mensagens SOCK_SEQPACKET com fds (SCM_RIGHTS).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "common/ipc_proto.h"

namespace framux::ipc {

inline constexpr int kMaxFds = 4;

struct Message {
  MsgType type{};
  std::vector<uint8_t> payload;
  int fds[kMaxFds] = {-1, -1, -1, -1};
  int nfds = 0;

  Message() = default;
  Message(const Message &) = delete;
  Message &operator=(const Message &) = delete;
  Message(Message &&o) noexcept;
  Message &operator=(Message &&o) noexcept;
  ~Message();

  // Retira o fd i (o chamador passa a ser dono).
  int take_fd(int i);

  template <class T>
  const T *as() const {
    return payload.size() >= sizeof(T) ? reinterpret_cast<const T *>(payload.data()) : nullptr;
  }
  // Bytes que seguem o struct T (ex.: TOML depois de Welcome).
  template <class T>
  std::string tail() const {
    if (payload.size() <= sizeof(T)) return {};
    return std::string(reinterpret_cast<const char *>(payload.data()) + sizeof(T), payload.size() - sizeof(T));
  }
};

// Envia [header][body][extra] com fds opcionais. flags é repassado ao
// sendmsg (use MSG_DONTWAIT no caminho do present). Devolve false em erro.
bool send_msg(int sock, MsgType type, const void *body, size_t body_len, const void *extra = nullptr,
              size_t extra_len = 0, const int *fds = nullptr, int nfds = 0, int flags = 0);

template <class T>
bool send_struct(int sock, MsgType type, const T &body, int flags = 0) {
  return send_msg(sock, type, &body, sizeof(T), nullptr, 0, nullptr, 0, flags);
}

enum class RecvResult { Ok, WouldBlock, Closed, Error };

// Recebe uma mensagem. Mensagens truncadas ou com tamanho inconsistente
// devolvem Error (o chamador deve encerrar a conexão).
RecvResult recv_msg(int sock, Message &out, int flags = 0);

void set_cloexec_nonblock(int fd, bool nonblock);

}  // namespace framux::ipc
