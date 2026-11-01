#include "common/shm.h"

#include <windows.h>

#include <cstring>

namespace framux::shm {

namespace {
HANDLE h_of(int h) { return reinterpret_cast<HANDLE>(intptr_t(h)); }
}  // namespace

int create(size_t size) {
  const uint64_t s = size;
  HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(s >> 32), DWORD(s), nullptr);
  return h ? int(intptr_t(h)) : -1;
}

void *map(int handle, size_t *size, bool writable) {
  if (handle == -1) return nullptr;
  void *p = MapViewOfFile(h_of(handle), writable ? FILE_MAP_READ | FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, 0);
  if (!p) return nullptr;
  MEMORY_BASIC_INFORMATION mbi{};
  if (size) *size = VirtualQuery(p, &mbi, sizeof mbi) ? size_t(mbi.RegionSize) : 0;
  return p;
}

void unmap(void *p) {
  if (p) UnmapViewOfFile(p);
}

void close(int handle) {
  if (handle != -1) CloseHandle(h_of(handle));
}

int from_bytes(const void *data, size_t size) {
  const int h = create(size);
  if (h == -1) return -1;
  void *p = map(h, nullptr, true);
  if (!p) {
    close(h);
    return -1;
  }
  std::memcpy(p, data, size);
  unmap(p);
  return h;
}

}  // namespace framux::shm
