// §C.12 teste 15 (lógica): a sessão ativa segue o foco, mas não troca
// durante gravação/benchmark; sessões paradas há ≥ 5 s perdem a vez.
#include <catch2/catch.hpp>

#include "core/arbiter.h"

using namespace framux;

namespace {
constexpr uint64_t S = 1'000'000'000ull;
}

TEST_CASE("foco define a sessão ativa", "[arbiter]") {
  Arbiter a;
  const uint64_t now = 100 * S;
  REQUIRE(a.update({{1, 1, now - S / 10, false}, {2, 0, now - S / 10, false}}, now) == 1);
  REQUIRE(a.update({{1, 0, now - S / 10, false}, {2, 1, now - S / 10, false}}, now) == 2);
  // Sem foco informado: continua a última.
  REQUIRE(a.update({{1, 0, now, false}, {2, 0, now, false}}, now) == 2);
}

TEST_CASE("não troca durante benchmark/gravação", "[arbiter]") {
  Arbiter a;
  const uint64_t now = 100 * S;
  REQUIRE(a.update({{1, 1, now, false}, {2, 0, now, false}}, now) == 1);
  REQUIRE(a.update({{1, 0, now, true}, {2, 1, now, false}}, now) == 1);
  REQUIRE(a.update({{1, 0, now, false}, {2, 1, now, false}}, now) == 2);
}

TEST_CASE("sessão sem presents há 5 s perde a prioridade", "[arbiter]") {
  Arbiter a;
  uint64_t now = 100 * S;
  REQUIRE(a.update({{1, 2, now, false}, {2, 2, now - S, false}}, now) == 1);
  now += 6 * S;
  REQUIRE(a.update({{1, 2, now - 6 * S, false}, {2, 2, now - S / 10, false}}, now) == 2);
  // Foco numa sessão parada não vale.
  REQUIRE(a.update({{1, 1, now - 6 * S, false}, {2, 0, now, false}}, now) == 2);
}

TEST_CASE("a ativa que sumiu é substituída pela mais recente", "[arbiter]") {
  Arbiter a;
  const uint64_t now = 100 * S;
  REQUIRE(a.update({{1, 1, now, false}}, now) == 1);
  REQUIRE(a.update({{2, 2, now - 2 * S, false}, {3, 2, now - S, false}}, now) == 3);
  REQUIRE(a.update({}, now) == 0);
}
