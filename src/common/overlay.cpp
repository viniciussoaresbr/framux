#include "common/overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace framux {

namespace {

// Fonte de 7 segmentos própria. Célula de 4×7 unidades, traço de 1 unidade,
// avanço de 5 unidades (1 de espaço).
constexpr int kCellW = 4, kCellH = 7, kAdvance = 5;

struct Seg {
  int8_t x, y, w, h;
};
constexpr Seg kSegs[7] = {
    {0, 0, 4, 1},  // a: topo
    {3, 0, 1, 4},  // b: superior direito
    {3, 3, 1, 4},  // c: inferior direito
    {0, 6, 4, 1},  // d: base
    {0, 3, 1, 4},  // e: inferior esquerdo
    {0, 0, 1, 4},  // f: superior esquerdo
    {0, 3, 4, 1},  // g: meio
};
//                          gfedcba
constexpr uint8_t kDigits[10] = {0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
                                 0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111};

OColor to_ocolor(Rgb c, float k = 1.0f) { return {c.r / 255.0f * k, c.g / 255.0f * k, c.b / 255.0f * k, 1.0f}; }

int unit_for(double scale, uint32_t height) {
  const double h = std::clamp(height / 1080.0, 0.5, 4.0);
  return std::max(2, int(std::lround(4.0 * scale * h)));
}

}  // namespace

int text_width(std::string_view s, int unit) {
  int w = 0;
  for (char c : s) w += (c == '.' ? 2 : kAdvance) * unit;
  return s.empty() ? 0 : w - unit;  // sem o espaço depois do último
}

int layout_text(std::string_view s, int x, int y, int unit, std::vector<ORect> &out) {
  const int x0 = x;
  for (char c : s) {
    uint8_t mask = 0;
    if (c >= '0' && c <= '9')
      mask = kDigits[c - '0'];
    else if (c == '-')
      mask = 1u << 6;
    if (c == '.') {
      out.push_back({x, y + 6 * unit, unit, unit});
      x += 2 * unit;
      continue;
    }
    for (int i = 0; i < 7; ++i) {
      if (!(mask & (1u << i))) continue;
      const Seg &sg = kSegs[i];
      out.push_back({x + sg.x * unit, y + sg.y * unit, sg.w * unit, sg.h * unit});
    }
    x += kAdvance * unit;
  }
  return x - x0;
}

void build_overlay(const OverlayContent &c, uint32_t width, uint32_t height, OverlayDraw &out) {
  out.visible = false;
  out.has_box = false;
  out.outline.clear();
  out.text.clear();
  if (!c.visible || c.corner == ipc::Corner::Hidden || width == 0 || height == 0) return;

  const int u = unit_for(c.scale, height);
  const int su = std::max(1, u / 2);
  const int pad = u;
  const int margin = 2 * u;
  const int tw = std::max(text_width(c.text, u), text_width(c.subtext, su));
  const int gap = u;  // entre o FPS e o frame time
  const int th = kCellH * u + (c.subtext.empty() ? 0 : gap + kCellH * su);
  const int bw = tw + 2 * pad, bh = th + 2 * pad;

  int bx = margin, by = margin;
  if (c.corner == ipc::Corner::TopRight || c.corner == ipc::Corner::BottomRight) bx = int(width) - margin - bw;
  if (c.corner == ipc::Corner::BottomLeft || c.corner == ipc::Corner::BottomRight) by = int(height) - margin - bh;
  bx = std::max(0, bx);
  by = std::max(0, by);

  layout_text(c.text, bx + pad, by + pad, u, out.text);
  if (!c.subtext.empty()) layout_text(c.subtext, bx + pad, by + pad + kCellH * u + gap, su, out.text);

  const int o = std::max(1, u / 3);
  out.outline.reserve(out.text.size());
  for (const ORect &r : out.text) out.outline.push_back({r.x - o, r.y - o, r.w + 2 * o, r.h + 2 * o});

  out.text_color = to_ocolor(c.color);
  out.has_box = c.has_box;
  if (c.has_box) {
    out.box = {bx, by, bw, bh};
    out.box_color = to_ocolor(c.box);
  }
  out.visible = true;
}

void OverlayState::set_config(const Config::Overlay &o) {
  const bool corner_changed = o.corner != cfg_.corner;
  cfg_ = o;
  if (corner_changed) corner_ = o.corner;
}

ipc::Corner home_corner(ipc::Corner configured) {
  return configured == ipc::Corner::Hidden ? ipc::Corner::TopLeft : configured;
}

ipc::Corner toggled_corner(ipc::Corner current, ipc::Corner configured) {
  return current == ipc::Corner::Hidden ? home_corner(configured) : ipc::Corner::Hidden;
}

ipc::Corner next_corner(ipc::Corner current, ipc::Corner configured) {
  if (current == ipc::Corner::Hidden) return home_corner(configured);
  return ipc::Corner((uint32_t(current) + 1) % 4);  // TL → TR → BR → BL → TL
}

void OverlayState::on_indicators(uint32_t flags, float bench_result, uint64_t now) {
  const bool was_bench = flags_ & ipc::kIndBenchmark;
  const bool is_bench = flags & ipc::kIndBenchmark;
  if (is_bench && !was_bench) bench_start_ = now;
  if (flags & ipc::kIndBenchResult) {
    bench_result_ = bench_result;
    result_until_ = now + kResultNs;
  }
  if (flags & ipc::kIndFlash) flash_until_ = now + kFlashNs;
  if (flags & ipc::kIndUnavailable) unavailable_until_ = now + kUnavailableNs;
  // Os bits de evento (flash, resultado, indisponível) não ficam no estado.
  flags_ = flags & (ipc::kIndRecording | ipc::kIndLoop | ipc::kIndBenchmark);
}

OverlayContent OverlayState::compute(uint64_t now, uint32_t fps, uint64_t frametime_ns) const {
  OverlayContent c;
  c.corner = corner_;
  c.scale = cfg_.scale;
  if (corner_ == ipc::Corner::Hidden) return c;

  const bool recording = flags_ & (ipc::kIndRecording | ipc::kIndLoop);
  c.color = (flags_ & ipc::kIndLoop)        ? Rgb{0xFF, 0x30, 0xFF}
            : (flags_ & ipc::kIndRecording) ? cfg_.color_recording
                                            : cfg_.color_normal;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%u", std::min<uint32_t>(fps, 9999));
  c.text = buf;
  if (cfg_.show_frametime) {
    std::snprintf(buf, sizeof buf, "%.1f", std::min(frametime_ns / 1e6, 9999.9));
    c.subtext = buf;
  }
  c.visible = true;

  if (flags_ & ipc::kIndBenchmark) {
    const bool first_second = now - bench_start_ < kBenchBoxNs;
    if (cfg_.during_benchmark == DuringBenchmark::Hide && !recording)
      c.visible = false;
    else if (first_second) {
      c.has_box = true;
      c.box = Rgb{uint8_t(cfg_.color_benchmark.r * 0.3), uint8_t(cfg_.color_benchmark.g * 0.3),
                  uint8_t(cfg_.color_benchmark.b * 0.3)};
    } else if (cfg_.during_benchmark == DuringBenchmark::HideAfter1s && !recording) {
      c.visible = false;  // não interferir na medição (como o FRAPS)
    }
  }
  if (now < result_until_) {
    c.visible = true;
    c.has_box = true;
    c.box = Rgb{0x40, 0x00, 0x00};
    c.color = Rgb{0xFF, 0xFF, 0xFF};
    std::snprintf(buf, sizeof buf, "%.0f", std::min<double>(bench_result_, 9999));
    c.text = buf;
    c.subtext.clear();
  }
  if (now < unavailable_until_) {
    c.visible = true;
    c.text = "-";
    c.subtext.clear();
  }
  if (now < flash_until_ && c.visible) {
    c.has_box = true;
    c.box = Rgb{0xFF, 0xFF, 0xFF};
  }
  return c;
}

}  // namespace framux
