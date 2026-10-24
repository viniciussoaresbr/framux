// Estatísticas de benchmark (§C.7.3).
#pragma once

#include <cstdint>
#include <vector>

namespace framux {

struct BenchStats {
  uint64_t frames = 0;
  uint64_t duration_ns = 0;              // do primeiro ao último present
  double avg_fps = 0;                    // frames / duração
  uint32_t min_fps = 0, max_fps = 0;     // da série por segundo inteiro
  std::vector<uint32_t> fps_per_second;  // só segundos completos
  double ft_avg_ms = 0, ft_p50_ms = 0, ft_p90_ms = 0, ft_p95_ms = 0, ft_p99_ms = 0;
  double low_1pct_fps = 0, low_01pct_fps = 0;  // 1000 / média dos piores N %

  uint64_t duration_ms() const { return (duration_ns + 500'000) / 1'000'000; }
};

// present_ns em ordem crescente (CLOCK_MONOTONIC).
BenchStats compute_stats(const std::vector<uint64_t> &present_ns);

// Percentil com interpolação linear entre posições (método "linear" do
// NumPy): posição = p/100 · (n − 1). `sorted` em ordem crescente.
double percentile(const std::vector<double> &sorted, double p);

// 1000 / média dos `pct` % maiores frame times (no mínimo um).
double low_fps(const std::vector<double> &sorted_ft_ms, double pct);

}  // namespace framux
