// §C.12 teste 6: estatísticas exatas (§C.7.3) e formato nativo.
#include <catch2/catch.hpp>

#include <cstdlib>

#include "core/bench_files.h"
#include "core/bench_stats.h"

using namespace framux;

namespace {

constexpr uint64_t kT0 = 5'000'000'000ull;

// Série B: frame times 1, 2, …, 100 ms (101 quadros).
std::vector<uint64_t> series_ramp() {
  std::vector<uint64_t> t{kT0};
  for (int k = 1; k <= 100; ++k) t.push_back(t.back() + uint64_t(k) * 1'000'000ull);
  return t;
}

// Série A: 100 fps exatos por 2,5 s (251 quadros).
std::vector<uint64_t> series_flat() {
  std::vector<uint64_t> t;
  for (int i = 0; i <= 250; ++i) t.push_back(kT0 + uint64_t(i) * 10'000'000ull);
  return t;
}

}  // namespace

TEST_CASE("percentil com interpolação linear", "[stats]") {
  std::vector<double> v{1, 2, 3, 4};
  REQUIRE(percentile(v, 0) == Approx(1));
  REQUIRE(percentile(v, 50) == Approx(2.5));
  REQUIRE(percentile(v, 100) == Approx(4));
  REQUIRE(percentile({7}, 99) == Approx(7));
}

TEST_CASE("série de 100 fps constantes", "[stats]") {
  const BenchStats s = compute_stats(series_flat());
  REQUIRE(s.frames == 251);
  REQUIRE(s.duration_ms() == 2500);
  REQUIRE(s.avg_fps == Approx(100.4));
  REQUIRE(s.fps_per_second == std::vector<uint32_t>{100, 100});  // 0,5 s final incompleto fica de fora
  REQUIRE(s.min_fps == 100);
  REQUIRE(s.max_fps == 100);
  REQUIRE(s.ft_p50_ms == Approx(10));
  REQUIRE(s.ft_p99_ms == Approx(10));
  REQUIRE(s.low_1pct_fps == Approx(100));
}

TEST_CASE("série de frame times 1..100 ms", "[stats]") {
  const BenchStats s = compute_stats(series_ramp());
  REQUIRE(s.frames == 101);
  REQUIRE(s.duration_ms() == 5050);
  REQUIRE(s.avg_fps == Approx(20.0));
  REQUIRE(s.fps_per_second == std::vector<uint32_t>{45, 18, 14, 12, 11});
  REQUIRE(s.min_fps == 11);
  REQUIRE(s.max_fps == 45);
  REQUIRE(s.ft_avg_ms == Approx(50.5));
  REQUIRE(s.ft_p50_ms == Approx(50.5));
  REQUIRE(s.ft_p90_ms == Approx(90.1));
  REQUIRE(s.ft_p95_ms == Approx(95.05));
  REQUIRE(s.ft_p99_ms == Approx(99.01));
  REQUIRE(s.low_1pct_fps == Approx(10.0));   // pior 1 % = {100 ms}
  REQUIRE(s.low_01pct_fps == Approx(10.0));  // no mínimo um quadro
}

TEST_CASE("1% low é a média dos piores, não o percentil", "[stats]") {
  // 1000 frame times: 990 de 10 ms e 10 de 40 ms → pior 1 % = média de 40 ms.
  std::vector<uint64_t> t{kT0};
  for (int i = 0; i < 1000; ++i) t.push_back(t.back() + (i % 100 == 0 ? 40'000'000ull : 10'000'000ull));
  const BenchStats s = compute_stats(t);
  REQUIRE(s.low_1pct_fps == Approx(25.0));
  REQUIRE(s.low_01pct_fps == Approx(25.0));
}

TEST_CASE("casos degenerados", "[stats]") {
  REQUIRE(compute_stats({}).frames == 0);
  const BenchStats one = compute_stats({kT0});
  REQUIRE(one.frames == 1);
  REQUIRE(one.avg_fps == 0);
  REQUIRE(one.fps_per_second.empty());
}

TEST_CASE("formato nativo", "[native]") {
  std::vector<BenchFrame> f{{1'000'000'000, 2560, 1440}, {1'016'712'000, 2560, 1440}};
  REQUIRE(format_native_frametimes(f) ==
          "frame,present_ns,frametime_ms,displayed_ns,width,height\n"
          "1,1000000000,0.000,,2560,1440\n"
          "2,1016712000,16.712,,2560,1440\n");
  BenchStats s = compute_stats(series_flat());
  REQUIRE(format_native_fps(s) == "second,frames\n1,100\n2,100\n");

  BenchMeta m;
  m.game = "eldenring";
  m.api = "vulkan";
  m.engine = "DXVK";
  const std::string j = format_summary_json(m, s, "2560x1440");
  REQUIRE(j.find("\"frames\":251") != std::string::npos);
  REQUIRE(j.find("\"avg_fps\":100.400") != std::string::npos);
  REQUIRE(j.find("\"low_definition\":\"mean_of_worst\"") != std::string::npos);
  REQUIRE(j.find("\"layer\":\"dxvk\"") != std::string::npos);
  REQUIRE(j.find("\"resolution\":\"2560x1440\"") != std::string::npos);
}
