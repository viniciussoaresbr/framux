#include "common/log.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>

#include <mutex>
#endif

namespace framux {

bool debug_enabled() {
  static const bool on = [] {
    const char *v = std::getenv("FRAMUX_DEBUG");
    return v && *v && std::strcmp(v, "0") != 0;
  }();
  return on;
}

#ifdef _WIN32
namespace {
std::mutex g_log_mu;
FILE *g_log_file = nullptr;
}  // namespace

void log_to_file(const char *path) {
  std::lock_guard lk(g_log_mu);
  if (g_log_file) std::fclose(g_log_file);
  g_log_file = std::fopen(path, "a");
}

uint64_t now_ns() {
  static const uint64_t freq = [] {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return uint64_t(f.QuadPart);
  }();
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  const uint64_t t = uint64_t(c.QuadPart);
  // Sem estouro: segundos inteiros e o resto separados.
  return t / freq * 1'000'000'000ull + t % freq * 1'000'000'000ull / freq;
}
#endif

void log_msg(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "[framux] %s\n", buf);
#ifdef _WIN32
  // Dentro do jogo não há stderr visível: o DebugView mostra estas linhas.
  char dbg[1100];
  std::snprintf(dbg, sizeof dbg, "[framux] %s\n", buf);
  OutputDebugStringA(dbg);
  std::lock_guard lk(g_log_mu);
  if (g_log_file) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::fprintf(g_log_file, "%04d-%02d-%02d %02d:%02d:%02d %s\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond, buf);
    std::fflush(g_log_file);
  }
#endif
}

}  // namespace framux
