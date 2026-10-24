// Overlay de segmentos retangulares (§B.5): o texto vira uma lista de
// retângulos que cada backend pinta com "clear" restrito (vkCmdClearAttachments
// no Vulkan, glScissor + glClear no OpenGL). Sem shaders, texturas ou fontes.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "common/config.h"
#include "common/ipc_proto.h"

namespace framux {

struct ORect {
  int32_t x, y;  // origem no canto superior esquerdo
  int32_t w, h;
};

struct OColor {
  float r, g, b, a;
};

struct OverlayDraw {
  bool visible = false;
  bool has_box = false;
  OColor box_color{};
  ORect box{};
  OColor outline_color{0, 0, 0, 1};
  std::vector<ORect> outline;  // contorno escuro (1ª passada)
  OColor text_color{};
  std::vector<ORect> text;  // dígitos (2ª passada)
};

// O que desenhar, já decidido pela máquina de estados.
struct OverlayContent {
  bool visible = false;
  ipc::Corner corner = ipc::Corner::TopLeft;
  double scale = 1.0;
  std::string text;     // dígitos, '.', '-'
  std::string subtext;  // linha menor (frame time), opcional
  Rgb color{};
  bool has_box = false;
  Rgb box{};
};

// Gera os retângulos para uma superfície width×height (origem no topo).
void build_overlay(const OverlayContent &c, uint32_t width, uint32_t height, OverlayDraw &out);

// Retângulos de um texto com unidade `unit` px a partir de (x, y).
// Devolve a largura total em px. Caracteres não suportados viram espaço.
int layout_text(std::string_view s, int x, int y, int unit, std::vector<ORect> &out);
int text_width(std::string_view s, int unit);

// Canto em que o overlay aparece ao ser mostrado: o da config (TopLeft se a
// config o deixa oculto).
ipc::Corner home_corner(ipc::Corner configured);
// Oculto ↔ canto da config.
ipc::Corner toggled_corner(ipc::Corner current, ipc::Corner configured);
// Próximo canto visível no sentido horário; oculto → canto da config.
ipc::Corner next_corner(ipc::Corner current, ipc::Corner configured);

// Estado dos indicadores do overlay (§B.5, §A.9): canto, benchmark, flash.
class OverlayState {
 public:
  static constexpr uint64_t kFlashNs = 120'000'000;
  static constexpr uint64_t kBenchBoxNs = 1'000'000'000;
  static constexpr uint64_t kResultNs = 1'000'000'000;
  static constexpr uint64_t kUnavailableNs = 1'000'000'000;

  void set_config(const Config::Overlay &o);
  void set_corner(ipc::Corner c) { corner_ = c; }
  ipc::Corner corner() const { return corner_; }
  // Atalho do overlay: mostra/oculta, sempre no canto da config.
  void toggle_visible() { corner_ = toggled_corner(corner_, cfg_.corner); }
  // Atalho de canto: TL → TR → BR → BL → TL; oculto aparece no da config.
  void next_corner() { corner_ = framux::next_corner(corner_, cfg_.corner); }

  void on_indicators(uint32_t flags, float bench_result, uint64_t now);
  void show_unavailable(uint64_t now) { unavailable_until_ = now + kUnavailableNs; }

  OverlayContent compute(uint64_t now, uint32_t fps, uint64_t frametime_ns) const;

  bool benchmark_running() const { return flags_ & ipc::kIndBenchmark; }

 private:
  Config::Overlay cfg_;
  ipc::Corner corner_ = ipc::Corner::TopLeft;
  uint32_t flags_ = 0;
  uint64_t bench_start_ = 0;
  uint64_t flash_until_ = 0;
  uint64_t result_until_ = 0;
  uint64_t unavailable_until_ = 0;
  float bench_result_ = 0;
};

}  // namespace framux
