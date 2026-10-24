// Contador de FPS exibido (§B.4), com a semântica do FRAPS (§A.9).
#pragma once

#include <cstdint>
#include <vector>

namespace framux {

inline constexpr uint64_t kNsPerSec = 1'000'000'000ull;
inline constexpr uint64_t kNsPerMs = 1'000'000ull;

enum class FpsMode { Smooth, OncePerSecond };

class FpsCounter {
 public:
  // Capacidade do anel de timestamps do modo smooth (≥ 16 384 exigido;
  // 32 768 cobre 20 000 fps sem saturar).
  static constexpr uint32_t kCapacity = 32768;

  explicit FpsCounter(FpsMode mode = FpsMode::Smooth);

  void set_mode(FpsMode mode);
  FpsMode mode() const { return mode_; }

  // Registra uma apresentação no instante t (ns, monotônico).
  void on_present(uint64_t t_ns);

  // Valor bruto no último present (smooth: contagem na janela de 1 s;
  // once_per_second: último balde fechado).
  uint32_t fps() const { return fps_; }

  // Valor para o texto do overlay: no modo smooth é atualizado no máximo a
  // cada 100 ms; no modo once_per_second é igual a fps().
  uint32_t display_fps() const { return display_; }

  // Tempo entre os dois últimos presents, em ns (0 se houver só um).
  uint64_t last_frametime_ns() const { return last_ft_; }
  uint64_t last_present_ns() const { return last_t_; }

  void reset();

 private:
  uint32_t smooth_count(uint64_t t);

  FpsMode mode_;
  // smooth
  std::vector<uint64_t> ring_;
  uint64_t head_ = 0;    // total de timestamps gravados
  uint64_t oldest_ = 0;  // índice absoluto do mais antigo dentro da janela
  uint64_t last_display_update_ = 0;
  // once_per_second
  bool started_ = false;
  uint64_t bucket_start_ = 0;
  uint32_t bucket_count_ = 0;

  uint64_t last_t_ = 0;
  uint64_t last_ft_ = 0;
  uint32_t fps_ = 0;
  uint32_t display_ = 0;
};

}  // namespace framux
