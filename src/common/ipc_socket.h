// Envio/recebimento de mensagens SOCK_SEQPACKET com fds (SCM_RIGHTS).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#ifdef _WIN32
#include <functional>
#include <thread>
#endif

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

#ifdef _WIN32
// No Windows o transporte é um named pipe em modo mensagem (cada mensagem é
// um WriteFile, como os datagramas do SEQPACKET). `sock` é o id de uma
// conexão registrada aqui; os "fds" das mensagens são handles, duplicados
// pelo emissor para dentro do processo do outro lado (como o SCM_RIGHTS).
#ifndef MSG_DONTWAIT
#define MSG_DONTWAIT 0x40  // envio com prazo curto; recebimento sem esperar
#endif

// Registra um pipe já conectado. `server`: este processo criou o pipe (o
// outro lado é o cliente). Devolve o id ou -1.
int pipe_adopt(void *pipe, bool server);
// Conecta ao pipe `name` (\\.\pipe\…). -1 se ninguém escuta.
int pipe_connect(const std::string &name, uint32_t timeout_ms);
// Cancela a I/O pendente e solta a conexão.
void pipe_close(int sock);
uint32_t pipe_peer_pid(int sock);
// Espera uma mensagem, `cancel` (evento) ou o prazo (devolve WouldBlock).
RecvResult recv_msg_wait(int sock, Message &out, void *cancel, uint32_t timeout_ms);

// Escuta conexões em `name` numa thread própria; `on_connect` recebe o id
// da conexão (na thread de escuta). Só o usuário atual pode conectar.
class PipeListener {
 public:
  ~PipeListener();
  bool start(const std::string &name, std::function<void(int sock)> on_connect, std::string *err);
  void stop();

 private:
  void main();
  std::string name_;
  std::function<void(int)> on_connect_;
  void *stop_event_ = nullptr;
  void *first_ = nullptr;  // primeira instância, criada em start() para detectar outro servidor
  std::thread thread_;
};

// Descritor de segurança que só dá acesso ao usuário atual (e ao SYSTEM).
void *user_only_security(bool *ok);
#endif

}  // namespace framux::ipc
