// Canal de controle no Windows (o equivalente à API D-Bus
// io.github.framux.Daemon1 do Linux), usado pela CLI e pela GUI.
//
// Transporte: o mesmo do IPC (ipc_socket.h), no pipe control_pipe_name().
// Cada mensagem é texto UTF-8 com campos separados por '\t':
//   pedido   (kCtlRequest): Status | StatusJson | Benchmark <ação> <s> |
//            Screenshot <0|1> | Overlay <ação> | Record <ação> | Loop <ação> |
//            Reload | Quit | Subscribe
//   resposta (kCtlReply):   OK <texto>  |  ERR <nome do erro> <mensagem>
//   eventos  (kCtlEvent, depois de Subscribe):
//            StatsChanged <fps> <quadros> <jogo> <flags>
//            FileSaved <request_id> <tipo> <caminho>
//            Error <request_id> <mensagem>
//            Notify <título> <texto> <ícone>
#pragma once

#include <string>
#include <vector>

#include "common/ipc_proto.h"

namespace framux::ctl {

inline constexpr auto kCtlRequest = ipc::MsgType(200);
inline constexpr auto kCtlReply = ipc::MsgType(201);
inline constexpr auto kCtlEvent = ipc::MsgType(202);

std::string control_pipe_name();

inline std::string join(const std::vector<std::string> &fields) {
  std::string out;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i) out += '\t';
    for (char c : fields[i]) out += c == '\t' ? ' ' : c;
  }
  return out;
}

inline std::vector<std::string> split(const std::string &s) {
  std::vector<std::string> out(1);
  for (char c : s) {
    if (c == '\t')
      out.emplace_back();
    else
      out.back() += c;
  }
  return out;
}

}  // namespace framux::ctl
