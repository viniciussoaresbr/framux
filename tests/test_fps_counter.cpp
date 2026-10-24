// §C.12 teste 14: FPS exibido com sequências sintéticas de presents.
#include <catch2/catch.hpp>

#include <cmath>
#include <random>

#include "common/fps_counter.h"

using namespace framux;

namespace {
constexpr uint64_t kBase = 10 * kNsPerSec;  // relógio monotônico "real"
uint64_t at(double seconds) { return kBase + uint64_t(std::llround(seconds * 1e9)); }
}  // namespace

TEST_CASE("smooth: 60 Hz exatos dão 60", "[fps]") {
  FpsCounter c(FpsMode::Smooth);
  for (int k = 0; k <= 600; ++k) {
    c.on_present(at(k / 60.0));
    if (k >= 60) REQUIRE(c.fps() == 60);
  }
}

TEST_CASE("smooth: 144 Hz com jitter fica em 144 ± 1", "[fps]") {
  FpsCounter c(FpsMode::Smooth);
  std::mt19937 rng(1234);
  std::uniform_real_distribution<double> jitter(-0.0015, 0.0015);
  for (int k = 0; k <= 144 * 10; ++k) {
    c.on_present(at(k / 144.0 + jitter(rng)));
    if (k >= 150) {
      REQUIRE(c.fps() >= 143);
      REQUIRE(c.fps() <= 145);
    }
  }
}

TEST_CASE("smooth: arredondamento na borda da janela", "[fps]") {
  // 2 fps: quadros em 0, 0.5, 1.0, 1.5… Em t = 1.5 + δ a janela (0.5+δ, 1.5+δ]
  // tem 2 quadros; o intervalo [0.5, 1.0] que cruza a borda está quase todo
  // dentro, logo conta: 2. Com a borda perto do quadro de dentro, não conta.
  FpsCounter c(FpsMode::Smooth);
  for (double t : {0.0, 0.5, 1.0, 1.5, 2.0}) c.on_present(at(t));
  REQUIRE(c.fps() == 2);
  FpsCounter d(FpsMode::Smooth);
  // Quadros em 0 e 0.9: em t = 1.85 a borda (0.85) está a 0.05 do quadro de
  // dentro (0.9) e a 0.85 do de fora (0): o intervalo cai quase todo fora.
  for (double t : {0.0, 0.9, 1.85}) d.on_present(at(t));
  REQUIRE(d.fps() == 1);
}

TEST_CASE("smooth: pausa de 3 s e retomada", "[fps]") {
  FpsCounter c(FpsMode::Smooth);
  int k = 0;
  for (; k <= 120; ++k) c.on_present(at(k / 60.0));
  REQUIRE(c.fps() == 60);
  const double resume = 2.0 + 3.0;
  c.on_present(at(resume));
  REQUIRE(c.fps() <= 1);
  for (int j = 1; j <= 60; ++j) c.on_present(at(resume + j / 60.0));
  REQUIRE(c.fps() == 60);
}

TEST_CASE("smooth: 20 000 fps não satura o anel", "[fps]") {
  FpsCounter c(FpsMode::Smooth);
  for (int k = 0; k <= 20000 * 3; ++k) c.on_present(at(k / 20000.0));
  REQUIRE(c.fps() >= 19999);
  REQUIRE(c.fps() <= 20001);
}

TEST_CASE("smooth: texto atualiza no máximo a cada 100 ms", "[fps]") {
  FpsCounter c(FpsMode::Smooth);
  int changes = 0;
  uint32_t last = c.display_fps();
  // Taxa subindo continuamente: o valor bruto muda quase todo quadro.
  double t = 0;
  for (int k = 0; k < 2000; ++k) {
    t += 1.0 / (30.0 + k * 0.1);
    c.on_present(at(t));
    if (c.display_fps() != last) {
      ++changes;
      last = c.display_fps();
    }
  }
  REQUIRE(changes <= int(t / 0.1) + 1);
}

TEST_CASE("once_per_second: publica a contagem ao fechar o balde", "[fps]") {
  FpsCounter c(FpsMode::OncePerSecond);
  for (int k = 0; k < 60; ++k) c.on_present(at(k / 60.0));
  REQUIRE(c.fps() == 0);  // primeiro balde ainda aberto
  c.on_present(at(1.0));
  REQUIRE(c.fps() == 60);
  for (int k = 61; k <= 600; ++k) {
    c.on_present(at(k / 60.0));
    REQUIRE((c.fps() == 60 || k < 120));
  }
}

TEST_CASE("once_per_second: reancora após pausa > 2 s", "[fps]") {
  FpsCounter c(FpsMode::OncePerSecond);
  for (int k = 0; k <= 120; ++k) c.on_present(at(k / 60.0));
  REQUIRE(c.fps() == 60);
  const double resume = 2.0 + 3.0;
  c.on_present(at(resume));  // fecha o balde antigo e reancora em "agora"
  for (int j = 1; j < 60; ++j) {
    c.on_present(at(resume + j / 60.0));
    REQUIRE(c.fps() != 0);
  }
  c.on_present(at(resume + 1.0));
  REQUIRE(c.fps() == 60);  // um balde inteiro depois da retomada, sem baldes vazios
}
