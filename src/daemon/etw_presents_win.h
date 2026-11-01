// FPS de jogos Direct3D/OpenGL no Windows sem entrar no processo do jogo:
// os eventos de apresentação que o próprio Windows publica no ETW (o método
// do PresentMon). Uma sessão de rastreamento em tempo real, numa thread.
//
// Provedores: Microsoft-Windows-DXGI (Present, D3D10/11/12),
// Microsoft-Windows-D3D9 (Present) e Microsoft-Windows-DxgKrnl
// (Present_Info: OpenGL e o que não passa pelos dois anteriores).
//
// Requer administrador ou o grupo "Usuários do log de desempenho".
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace framux::daemon {

enum class PresentSource : uint8_t { Dxgi, D3d9, Kernel };

class PresentTracer {
 public:
  // Chamado na thread do ETW para cada present: processo, origem, um id da
  // cadeia de troca (0 se desconhecido) e o instante (relógio de now_ns()).
  using Sink = std::function<void(uint32_t pid, PresentSource src, uint64_t swapchain, uint64_t t_ns)>;

  ~PresentTracer();
  bool start(Sink sink, std::string *err);
  void stop();

 private:
  void main();
  Sink sink_;
  uint64_t session_ = 0;  // TRACEHANDLE
  uint64_t trace_ = 0;
  std::thread thread_;
  std::atomic<bool> running_{false};

 public:
  // Usado pelo callback do ETW (C).
  void on_event(const void *record);
  uint64_t qpc_freq_ = 1;
  // Eventos recebidos (diagnóstico no log).
  std::atomic<uint64_t> events_{0};
};

}  // namespace framux::daemon
