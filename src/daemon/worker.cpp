#include "daemon/worker.h"

#include <pthread.h>
#include <sys/eventfd.h>
#include <unistd.h>

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

MainQueue::MainQueue() : fd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

MainQueue::~MainQueue() {
  if (fd_ >= 0) close(fd_);
}

void MainQueue::post(std::function<void()> fn) {
  {
    std::lock_guard lk(mu_);
    q_.push_back(std::move(fn));
  }
  const uint64_t one = 1;
  (void)!write(fd_, &one, sizeof one);
}

void MainQueue::run_pending() {
  uint64_t v;
  (void)!read(fd_, &v, sizeof v);
  std::deque<std::function<void()>> q;
  {
    std::lock_guard lk(mu_);
    q.swap(q_);
  }
  for (auto &fn : q) fn();
}

}  // namespace framux::daemon
