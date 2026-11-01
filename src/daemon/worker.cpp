#include "daemon/worker.h"

#include <pthread.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/eventfd.h>
#include <unistd.h>
#endif

namespace framux::daemon {

Worker::Worker() : thread_([this] { main(); }) {}

Worker::~Worker() { stop(); }

void Worker::post(std::function<void()> job) {
  {
    std::lock_guard lk(mu_);
    jobs_.push_back(std::move(job));
  }
  cv_.notify_one();
}

void Worker::stop() {
  {
    std::lock_guard lk(mu_);
    if (stopping_ && !thread_.joinable()) return;
    stopping_ = true;
  }
  cv_.notify_one();
  if (thread_.joinable()) thread_.join();
}

void Worker::main() {
  pthread_setname_np(pthread_self(), "framuxd-worker");
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock lk(mu_);
      cv_.wait(lk, [this] { return stopping_ || !jobs_.empty(); });
      if (jobs_.empty()) return;  // stopping_ e fila vazia
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    job();
  }
}

#ifdef _WIN32
MainQueue::MainQueue() : fd_(-1), event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}

MainQueue::~MainQueue() {
  if (event_) CloseHandle(event_);
}
#else
MainQueue::MainQueue() : fd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

MainQueue::~MainQueue() {
  if (fd_ >= 0) close(fd_);
}
#endif

void MainQueue::post(std::function<void()> fn) {
  {
    std::lock_guard lk(mu_);
    q_.push_back(std::move(fn));
  }
#ifdef _WIN32
  SetEvent(event_);
#else
  const uint64_t one = 1;
  (void)!write(fd_, &one, sizeof one);
#endif
}

void MainQueue::run_pending() {
#ifndef _WIN32
  uint64_t v;
  (void)!read(fd_, &v, sizeof v);
#endif
  std::deque<std::function<void()>> q;
  {
    std::lock_guard lk(mu_);
    q.swap(q_);
  }
  for (auto &fn : q) fn();
}

}  // namespace framux::daemon
