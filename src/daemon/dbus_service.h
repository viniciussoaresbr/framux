// API D-Bus io.github.framux.Daemon1 (§C.3.3), usada pela CLI e por scripts.
// No Windows a mesma interface é servida pelo pipe de controle.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace framux::daemon {

inline constexpr const char *kBusName = "io.github.framux.Daemon1";
inline constexpr const char *kObjectPath = "/io/github/framux/Daemon1";
inline constexpr const char *kInterface = "io.github.framux.Daemon1";

// Resultado de uma ação: ok ou erro D-Bus nomeado.
struct ActionResult {
  bool ok = true;
  std::string error_name;  // ex.: io.github.framux.Error.NoSession
  std::string message;
  uint64_t request_id = 0;
};

struct DbusHandlers {
  std::function<std::string(bool json)> status;
  std::function<ActionResult(const std::string &action, uint32_t duration_s)> benchmark;
  std::function<ActionResult(bool force_overlay)> screenshot;
  std::function<ActionResult(const std::string &action)> overlay;
  std::function<ActionResult(const std::string &what, const std::string &action)> video;
  std::function<void()> reload;
  std::function<void()> quit;  // só no Windows ("Parar o Framux" pela GUI)
};

class MainQueue;

class DbusService {
 public:
  ~DbusService();
  // nullptr se não houver barramento de sessão. *already_running = true se
  // outro framuxd já tem o nome.
#ifdef _WIN32
  // No Windows: pipe de controle (common/control_proto.h). Os handlers rodam
  // na thread principal, por `queue`.
  static std::unique_ptr<DbusService> open(DbusHandlers h, bool *already_running, MainQueue &queue);
#else
  static std::unique_ptr<DbusService> open(DbusHandlers h, bool *already_running);

  int fd() const;
  uint32_t events() const;  // EPOLLIN/EPOLLOUT pedidos pelo sd-bus
  int timeout_ms() const;   // -1 = sem prazo
  void process();
#endif

  void emit_stats(double fps, uint64_t frames, const std::string &game, uint32_t flags);
  void emit_file_saved(uint64_t request_id, const std::string &kind, const std::string &path);
  void emit_error(uint64_t request_id, const std::string &message);
  void notify(const std::string &summary, const std::string &body, const std::string &icon);

  struct Impl;  // público só para os callbacks C do sd-bus

 private:
  DbusService() = default;
  std::unique_ptr<Impl> impl_;
};

}  // namespace framux::daemon
