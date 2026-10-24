// Arbitragem da sessão ativa (§C.3.3): para qual jogo vão as hotkeys e o
// que a CLI/estatísticas mostram.
#pragma once

#include <cstdint>
#include <vector>

namespace framux {

struct SessionView {
  uint64_t id = 0;
  int focus = 2;                 // 0 sem foco, 1 com foco, 2 desconhecido
  uint64_t last_present_ns = 0;  // 0 = nunca apresentou
  bool busy = false;             // gravando ou em benchmark
};

class Arbiter {
 public:
  static constexpr uint64_t kStaleNs = 5'000'000'000ull;

  // Regras, em ordem:
  //  1. a sessão ativa ocupada (gravação/benchmark) nunca é trocada;
  //  2. a sessão com foco (e presents nos últimos 5 s) vira a ativa;
  //  3. sem foco informado, continua a última;
  //  4. a ativa sem presents há ≥ 5 s perde a vez para a que apresentou
  //     mais recentemente;
  //  5. se a ativa sumiu, assume a que apresentou mais recentemente.
  uint64_t update(const std::vector<SessionView> &sessions, uint64_t now);
  uint64_t active() const { return active_; }
  void reset() { active_ = 0; }

 private:
  uint64_t active_ = 0;
};

}  // namespace framux
