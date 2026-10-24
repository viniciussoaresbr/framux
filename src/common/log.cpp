#include "common/log.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>

namespace framux {

bool debug_enabled() {
  static const bool on = [] {
    const char *v = std::getenv("FRAMUX_DEBUG");
    return v && *v && std::strcmp(v, "0") != 0;
  }();
  return on;
}

void log_msg(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  std::fprintf(stderr, "[framux] %s\n", buf);
}

}  // namespace framux
