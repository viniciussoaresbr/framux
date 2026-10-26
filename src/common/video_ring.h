// Buffer de quadros de vídeo em memória compartilhada (§B.7, "fallback
// universal"): a camada copia cada quadro para um slot livre e avisa o
// daemon com VIDEO_FRAME; o daemon codifica e devolve o slot.
//
// Sem slot livre (encoder atrasado), a camada descarta o quadro em vez de
// esperar: nunca segura o jogo (§B.10, backpressure = "drop").
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace framux {

inline constexpr uint32_t kVideoRingMagic = 0x46524d56;  // "FRMV"
inline constexpr uint32_t kVideoSlots = 4;

enum VideoSlotState : uint32_t { kSlotFree = 0, kSlotFilled = 1 };

struct VideoRingHeader {
  uint32_t magic;
  uint32_t slot_count;
  uint64_t slot_bytes;  // capacidade de cada slot
  std::atomic<uint32_t> state[kVideoSlots];
  uint64_t _reserved[4];
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(sizeof(VideoRingHeader) == 64);

// Slots começam alinhados à página, depois do cabeçalho.
inline constexpr size_t kVideoRingDataOffset = 4096;

inline size_t video_ring_bytes(uint64_t slot_bytes) { return kVideoRingDataOffset + size_t(slot_bytes) * kVideoSlots; }

inline uint8_t *video_slot(VideoRingHeader *r, uint32_t slot) {
  return reinterpret_cast<uint8_t *>(r) + kVideoRingDataOffset + size_t(r->slot_bytes) * slot;
}

}  // namespace framux
