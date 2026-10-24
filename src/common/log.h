// Mensagens no stderr com o prefixo "[framux]" (§C.11.6).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace framux {

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Nível de detalhe por FRAMUX_DEBUG=1.
bool debug_enabled();

inline uint64_t now_ns() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
}

}  // namespace framux

#define FRAMUX_LOG(...) ::framux::log_msg(__VA_ARGS__)
#define FRAMUX_DEBUG(...)                                          \
  do {                                                             \
    if (::framux::debug_enabled()) ::framux::log_msg(__VA_ARGS__); \
  } while (0)
// Registra só na primeira vez que a linha é atingida.
#define FRAMUX_LOG_ONCE(...)                                            \
  do {                                                                  \
    static std::atomic<bool> framux_logged_{false};                     \
    if (!framux_logged_.exchange(true)) ::framux::log_msg(__VA_ARGS__); \
  } while (0)
