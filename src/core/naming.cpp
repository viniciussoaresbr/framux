#include "core/naming.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

namespace framux {

std::string file_timestamp(std::time_t t, int centis) {
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[40];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H-%M-%S", &tm);
  std::string s = buf;
  if (centis >= 0) {
    std::snprintf(buf, sizeof buf, "-%02d", centis % 100);
    s += buf;
  }
  return s;
}

int create_unique(const std::string &dir, const std::string &stem, const std::string &suffix, std::string *path,
                  std::string *err) {
  for (int n = 1; n < 1000; ++n) {
    std::string p = dir + "/" + stem + (n == 1 ? "" : " (" + std::to_string(n) + ")") + suffix;
    const int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd >= 0) {
      if (path) *path = p;
      return fd;
    }
    if (errno != EEXIST) {
      if (err) *err = p + ": " + std::strerror(errno);
      return -1;
    }
  }
  if (err) *err = dir + "/" + stem + suffix + ": nomes esgotados";
  return -1;
}

bool write_all_close(int fd, const std::string &data, std::string *err) {
  size_t done = 0;
  while (done < data.size()) {
    const ssize_t w = write(fd, data.data() + done, data.size() - done);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) {
      if (err) *err = std::strerror(errno);
      close(fd);
      return false;
    }
    done += size_t(w);
  }
  if (close(fd) != 0) {
    if (err) *err = std::strerror(errno);
    return false;
  }
  return true;
}

bool append_file(const std::string &path, const std::string &data, std::string *err) {
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) {
    if (err) *err = path + ": " + std::strerror(errno);
    return false;
  }
  return write_all_close(fd, data, err);
}

}  // namespace framux
