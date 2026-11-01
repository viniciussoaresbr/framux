// Cliente do canal de controle do framuxd no Windows (control_proto.h),
// usado pela CLI e pela GUI.
#pragma once

#ifdef _WIN32

#include <cstdint>
#include <string>
#include <vector>

namespace framux::ctl {

struct Reply {
  bool ok = false;
  bool unreachable = false;  // daemon não está rodando
  std::string text;          // OK: resposta; ERR: mensagem
  std::string error_name;    // ERR: ex. io.github.framux.Error.NoSession
};

// Um pedido, uma resposta (conexão própria).
Reply call(const std::vector<std::string> &request, uint32_t timeout_ms = 5000);

// Conexão inscrita nos eventos (FileSaved, Error, StatsChanged, Notify).
class Subscription {
 public:
  ~Subscription();
  bool open(uint32_t timeout_ms = 1000);
  bool is_open() const { return sock_ >= 0; }
  // Próximo evento (campos já separados) ou vazio se o prazo passar.
  // `closed` = o daemon fechou a conexão.
  std::vector<std::string> next(uint32_t timeout_ms, bool *closed = nullptr);
  void close();

 private:
  int sock_ = -1;
};

}  // namespace framux::ctl

#endif
