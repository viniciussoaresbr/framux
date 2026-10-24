// Thread de trabalho do daemon: codificação de imagens e escrita de arquivos
// ficam fora do loop principal, que nunca bloqueia em disco.
#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace framux::daemon {

class Worker {
 public:
  Worker();
  ~Worker();
  void post(std::function<void()> job);
  // Termina os trabalhos pendentes e encerra a thread.
  void stop();

 private:
  void main();
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> jobs_;
  bool stopping_ = false;
  std::thread thread_;
};

// Fila de callbacks para executar no loop principal (acordado por eventfd).
class MainQueue {
 public:
  MainQueue();
  ~MainQueue();
  int fd() const { return fd_; }
  void post(std::function<void()> fn);
  void run_pending();

 private:
  int fd_;
  std::mutex mu_;
  std::deque<std::function<void()>> q_;
};

}  // namespace framux::daemon
