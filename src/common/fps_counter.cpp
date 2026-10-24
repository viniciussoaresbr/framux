#include "common/fps_counter.h"

#include <algorithm>

namespace framux {

namespace {
constexpr uint64_t kDisplayIntervalNs = 100 * kNsPerMs;
constexpr uint64_t kReanchorNs = 2 * kNsPerSec;
}  // namespace

FpsCounter::FpsCounter(FpsMode mode) : mode_(mode), ring_(kCapacity, 0) {}

void FpsCounter::set_mode(FpsMode mode) {
  if (mode == mode_) return;
  mode_ = mode;
  reset();
}

void FpsCounter::reset() {
  head_ = oldest_ = 0;
  last_display_update_ = 0;
  started_ = false;
  bucket_start_ = 0;
  bucket_count_ = 0;
  last_t_ = last_ft_ = 0;
  fps_ = display_ = 0;
}

// Janela deslizante (agora − 1 s, agora]. O intervalo entre o último quadro
// fora da janela e o mais antigo dentro dela atravessa a borda; ele é
// arredondado para o vizinho mais próximo: se a borda está mais perto do
// quadro de dentro (a maior parte do intervalo cai fora), o quadro mais
// antigo não conta. Assim 60 Hz exatos dão 60, e o jitter perto da borda
// não faz o número oscilar entre 60 e 61.
uint32_t FpsCounter::smooth_count(uint64_t t) {
  constexpr uint64_t mask = kCapacity - 1;
  ring_[head_ & mask] = t;
  ++head_;
  if (head_ - oldest_ > kCapacity) oldest_ = head_ - kCapacity;

  if (t < kNsPerSec) {
    // Relógio perto de zero (só em testes): nada pode estar fora da janela.
    return uint32_t(head_ - oldest_);
  }
  const uint64_t edge = t - kNsPerSec;
  while (oldest_ < head_ && ring_[oldest_ & mask] <= edge) ++oldest_;

  uint64_t count = head_ - oldest_;
  const bool have_outside = oldest_ > 0 && head_ - (oldest_ - 1) <= kCapacity;
  if (have_outside && count > 0) {
    const uint64_t first_out = ring_[(oldest_ - 1) & mask];
    const uint64_t oldest_in = ring_[oldest_ & mask];
    if (oldest_in - edge < edge - first_out) --count;
  }
  return uint32_t(std::min<uint64_t>(count, kCapacity));
}

void FpsCounter::on_present(uint64_t t) {
  if (last_t_ != 0 && t >= last_t_) last_ft_ = t - last_t_;

  if (mode_ == FpsMode::Smooth) {
    fps_ = smooth_count(t);
    if (last_display_update_ == 0 || t - last_display_update_ >= kDisplayIntervalNs) {
      display_ = fps_;
      last_display_update_ = t;
    }
  } else {
    if (!started_) {
      started_ = true;
      bucket_start_ = t;
      bucket_count_ = 0;
    }
    const uint64_t elapsed = t - bucket_start_;
    if (elapsed >= kNsPerSec) {
      fps_ = bucket_count_;
      display_ = fps_;
      bucket_count_ = 0;
      // Pausa longa (jogo parado): reancora em agora em vez de gerar baldes
      // vazios em sequência.
      bucket_start_ = elapsed > kReanchorNs ? t : bucket_start_ + kNsPerSec;
    }
    ++bucket_count_;
  }
  last_t_ = t;
}

}  // namespace framux
