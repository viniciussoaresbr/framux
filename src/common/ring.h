// Ring de timestamps em memória compartilhada (§C.4.2).
//
// Escritor: o processo do jogo (um escritor lógico; a camada serializa os
// presents de várias threads com um spinlock). Leitor: o framuxd.
// Se o leitor atrasar, o escritor sobrescreve os registros mais antigos e
// incrementa `dropped`; o leitor detecta a sobrescrita comparando head e tail.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace framux {

struct FrameRec {          // 32 bytes
  uint64_t present_ns;     // CLOCK_MONOTONIC no início do present
  uint64_t frame_id;       // contador por swapchain lógico
  uint32_t swapchain_id;   // swapchain lógico (recriações herdam o id)
  uint32_t width, height;  // extent atual
  uint32_t flags;
};
static_assert(sizeof(FrameRec) == 32);

inline constexpr uint32_t kRingMagic = 0x46524d58;  // "FRMX"
inline constexpr uint32_t kRingDefaultCapacity = 65536;

struct RingHeader {
  uint32_t magic;
  uint32_t capacity;              // potência de 2
  std::atomic<uint64_t> head;     // próxima posição de escrita
  std::atomic<uint64_t> tail;     // próxima posição de leitura
  std::atomic<uint64_t> dropped;  // registros sobrescritos antes da leitura
  uint64_t _reserved[4];
  FrameRec rec[];
};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(sizeof(RingHeader) == 64);

inline size_t ring_bytes(uint32_t capacity) { return sizeof(RingHeader) + size_t(capacity) * sizeof(FrameRec); }

inline void ring_init(RingHeader *r, uint32_t capacity) {
  r->magic = kRingMagic;
  r->capacity = capacity;
  r->head.store(0, std::memory_order_relaxed);
  r->tail.store(0, std::memory_order_relaxed);
  r->dropped.store(0, std::memory_order_relaxed);
}

// Lado do jogo. Nunca bloqueia nem aloca.
inline void ring_push(RingHeader *r, const FrameRec &rec) {
  const uint64_t head = r->head.load(std::memory_order_relaxed);
  const uint64_t tail = r->tail.load(std::memory_order_acquire);
  if (head - tail >= r->capacity) r->dropped.fetch_add(1, std::memory_order_relaxed);
  r->rec[head & (r->capacity - 1)] = rec;
  r->head.store(head + 1, std::memory_order_release);
}

// Lado do daemon. Chama `fn(const FrameRec&)` para cada registro novo e
// devolve quantos foram lidos. Registros sobrescritos durante a cópia são
// descartados (e contados em `lost`).
template <class Fn>
size_t ring_drain(RingHeader *r, Fn &&fn, uint64_t *lost = nullptr) {
  const uint32_t cap = r->capacity;
  uint64_t tail = r->tail.load(std::memory_order_relaxed);
  const uint64_t head = r->head.load(std::memory_order_acquire);
  if (head - tail > cap) {
    if (lost) *lost += head - tail - cap;
    tail = head - cap;
  }
  size_t n = 0;
  for (; tail < head; ++tail) {
    FrameRec rec = r->rec[tail & (cap - 1)];
    // Se o escritor deu a volta enquanto copiávamos, o registro é inválido.
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t now_head = r->head.load(std::memory_order_relaxed);
    if (now_head - tail > cap) {
      if (lost) ++*lost;
      continue;
    }
    fn(rec);
    ++n;
  }
  r->tail.store(tail, std::memory_order_release);
  return n;
}

}  // namespace framux
