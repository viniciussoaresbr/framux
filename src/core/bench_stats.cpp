#include "core/bench_stats.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace framux {

double percentile(const std::vector<double> &sorted, double p) {
  if (sorted.empty()) return 0;
  const double pos = p / 100.0 * double(sorted.size() - 1);
  const size_t lo = size_t(std::floor(pos));
  const size_t hi = std::min(lo + 1, sorted.size() - 1);
  const double frac = pos - double(lo);
  return sorted[lo] + (sorted[hi] - sorted[lo]) * frac;
}

double low_fps(const std::vector<double> &sorted_ft_ms, double pct) {
  if (sorted_ft_ms.empty()) return 0;
  const size_t n = sorted_ft_ms.size();
  const size_t k = std::max<size_t>(1, size_t(std::floor(double(n) * pct / 100.0)));
  const double sum = std::accumulate(sorted_ft_ms.end() - std::ptrdiff_t(k), sorted_ft_ms.end(), 0.0);
  const double mean = sum / double(k);
  return mean > 0 ? 1000.0 / mean : 0;
}

BenchStats compute_stats(const std::vector<uint64_t> &t) {
  BenchStats s;
  s.frames = t.size();
  if (t.empty()) return s;
  const uint64_t t0 = t.front();
  s.duration_ns = t.back() - t0;
  if (s.duration_ns > 0) s.avg_fps = double(s.frames) / (double(s.duration_ns) / 1e9);

  // Série por segundo inteiro desde o primeiro present. O segundo k é
  // [k−1, k) s; só entram os segundos completos (o último incompleto fica
  // de fora, inclusive de min/max).
  const uint64_t full_seconds = s.duration_ns / 1'000'000'000ull;
  s.fps_per_second.assign(size_t(full_seconds), 0);
  for (uint64_t ts : t) {
    const uint64_t sec = (ts - t0) / 1'000'000'000ull;
    if (sec < full_seconds) ++s.fps_per_second[size_t(sec)];
  }
  if (!s.fps_per_second.empty()) {
    auto [mn, mx] = std::minmax_element(s.fps_per_second.begin(), s.fps_per_second.end());
    s.min_fps = *mn;
    s.max_fps = *mx;
  }

  if (t.size() < 2) return s;
  std::vector<double> ft;
  ft.reserve(t.size() - 1);
  for (size_t i = 1; i < t.size(); ++i) ft.push_back(double(t[i] - t[i - 1]) / 1e6);
  s.ft_avg_ms = std::accumulate(ft.begin(), ft.end(), 0.0) / double(ft.size());
  std::sort(ft.begin(), ft.end());
  s.ft_p50_ms = percentile(ft, 50);
  s.ft_p90_ms = percentile(ft, 90);
  s.ft_p95_ms = percentile(ft, 95);
  s.ft_p99_ms = percentile(ft, 99);
  s.low_1pct_fps = low_fps(ft, 1.0);
  s.low_01pct_fps = low_fps(ft, 0.1);
  return s;
}

}  // namespace framux
