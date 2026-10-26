#include <catch2/catch.hpp>

#include <X11/keysym.h>

#include <cstdlib>

#include "common/config.h"
#include "common/hotkey.h"
#include "common/identity.h"
#include "common/overlay.h"
#include "common/paths.h"
#include "common/ring.h"
#include "common/video_rate.h"

using namespace framux;

TEST_CASE("hotkeys: sintaxe da config", "[hotkey]") {
  REQUIRE(parse_hotkey("F9")->keysym == XK_F9);
  auto h = parse_hotkey("Ctrl+Shift+F9");
  REQUIRE(h);
  REQUIRE(h->mods == (ipc::kModCtrl | ipc::kModShift));
  REQUIRE(format_hotkey(*h) == "Ctrl+Shift+F9");
  REQUIRE(parse_hotkey("Mouse4")->mouse_button() == 4);
  REQUIRE(parse_hotkey("alt + x")->keysym == XK_x);
  REQUIRE_FALSE(parse_hotkey("")->enabled());
  REQUIRE_FALSE(parse_hotkey("Hyper+F1"));
  REQUIRE_FALSE(parse_hotkey("F25"));
  REQUIRE_FALSE(parse_hotkey("Ctrl+"));
  for (const char *s : {"F1", "F24", "A", "0", "KP_5", "PageUp", "Print", "Super+Space", "Mouse3"})
    REQUIRE(format_hotkey(*parse_hotkey(s)) == s);
}

TEST_CASE("config: padrões, override e valores inválidos", "[config]") {
  Config c;
  REQUIRE(c.hotkeys.benchmark == "F11");
  REQUIRE(c.overlay.corner == ipc::Corner::TopLeft);
  std::vector<std::string> w;
  REQUIRE(apply_config_toml(c, R"(
[overlay]
corner = "bottom-right"
update = "once_per_second"
color_normal = "#00ff7F"
scale = 99
[general]
theme = "light"
[benchmark]
logs = ["fps"]
fraps_compat = true
stop_after_s = "x"
[hotkeys]
video = "Banana"
[nada]
x = 1
)",
                            "t.toml", &w));
  REQUIRE(c.overlay.corner == ipc::Corner::BottomRight);
  REQUIRE(c.overlay.update == FpsMode::OncePerSecond);
  REQUIRE(c.overlay.color_normal == Rgb{0x00, 0xFF, 0x7F});
  REQUIRE(c.overlay.scale == 1.0);  // fora do intervalo: mantém
  REQUIRE(c.benchmark.log_fps);
  REQUIRE_FALSE(c.benchmark.log_frametimes);
  REQUIRE(c.benchmark.stop_after_s == 0);
  REQUIRE(c.hotkeys.video == "F9");
  REQUIRE(w.size() == 4);  // scale, stop_after_s, video, [nada]; fraps_compat e theme (removidas) sem aviso

  std::vector<std::string> w2;
  Config bad;
  REQUIRE_FALSE(apply_config_toml(bad, "[[[", "x", &w2));
  REQUIRE(w2.size() == 1);
}

TEST_CASE("config: encoders removidos (HEVC, FFV1) migram sem aviso", "[config]") {
  Config c;
  c.video.encoder = "av1_sw";
  std::vector<std::string> w;
  REQUIRE(apply_config_toml(c, "[video]\nencoder = \"hevc_hw\"\n", "t.toml", &w));
  REQUIRE(c.video.encoder == "auto");
  REQUIRE(w.empty());
  REQUIRE(apply_config_toml(c, "[video]\nencoder = \"ffv1\"\n", "t.toml", &w));
  REQUIRE(c.video.encoder == "utvideo");
  REQUIRE(w.empty());
  REQUIRE(apply_config_toml(c, "[video]\nencoder = \"h265\"\n", "t.toml", &w));
  REQUIRE(c.video.encoder == "utvideo");  // inválido: mantém
  REQUIRE(w.size() == 1);
}

TEST_CASE("vídeo: teto de taxa por resolução, fps e qualidade", "[video]") {
  // 2560×1080 a 60 fps, H.264 alta: ~20 Mbit/s (o openh264 do Fedora toca).
  REQUIRE(video_max_bitrate(false, "high", 2560, 1080, 60) == 19'906'560);
  REQUIRE(video_max_bitrate(false, "medium", 1920, 1080, 60) < video_max_bitrate(false, "high", 1920, 1080, 60));
  REQUIRE(video_max_bitrate(false, "low", 1920, 1080, 60) < video_max_bitrate(false, "medium", 1920, 1080, 60));
  REQUIRE(video_max_bitrate(true, "high", 1920, 1080, 60) < video_max_bitrate(false, "high", 1920, 1080, 60));
  REQUIRE(std::llabs(int64_t(video_max_bitrate(false, "high", 1920, 1080, 30) * 2) -
                     int64_t(video_max_bitrate(false, "high", 1920, 1080, 60))) <= 2);
  REQUIRE(video_max_bitrate(false, "lossless", 1920, 1080, 60) == 0);
}

TEST_CASE("config: ida e volta pelo TOML", "[config]") {
  Config c;
  c.overlay.corner = ipc::Corner::Hidden;
  c.screenshot.format = ImageFormat::Jpeg;
  c.benchmark.stop_after_s = 60;
  c.filters.include = {"game"};
  Config d;
  std::vector<std::string> w;
  REQUIRE(apply_config_toml(d, config_to_toml(c), "rt", &w));
  REQUIRE(w.empty());
  REQUIRE(d.overlay.corner == ipc::Corner::Hidden);
  REQUIRE(d.screenshot.format == ImageFormat::Jpeg);
  REQUIRE(d.benchmark.stop_after_s == 60);
  REQUIRE(d.filters.include == std::vector<std::string>{"game"});
}

TEST_CASE("filtros de processo", "[config]") {
  Config c;
  REQUIRE(is_excluded(c, "steamwebhelper", "steamwebhelper"));
  REQUIRE_FALSE(is_excluded(c, "vkcube", "vkcube"));
  c.filters.include = {"eldenring"};
  REQUIRE(is_excluded(c, "vkcube", "vkcube"));
  REQUIRE_FALSE(is_excluded(c, "wine64-preloader", "eldenring"));
}

TEST_CASE("modo automático: só jogos", "[config]") {
  Config c;
  // GTK4, navegadores e afins também carregam a camada; ficam de fora.
  REQUIRE_FALSE(auto_mode_accepts(c, "nautilus", "nautilus", false, false));
  REQUIRE(auto_mode_accepts(c, "wine64-preloader", "eldenring", true, false));
  REQUIRE(auto_mode_accepts(c, "hl2_linux", "hl2_linux", false, true));
  REQUIRE_FALSE(auto_mode_accepts(c, "steamwebhelper", "steamwebhelper", false, true));
  c.filters.include = {"supertuxkart"};
  REQUIRE(auto_mode_accepts(c, "supertuxkart", "supertuxkart", false, false));
}

TEST_CASE("identidade: nativo e Wine", "[identity]") {
  auto n = identity_from("/usr/bin/vkcube", "vkcube", {"vkcube", "--c", "10"});
  REQUIRE(n.game == "vkcube");
  REQUIRE_FALSE(n.wine);
  auto w = identity_from("/opt/proton/bin/wine64-preloader", "eldenring.exe",
                         {"Z:\\games\\ELDEN RING\\Game\\eldenring.exe"});
  REQUIRE(w.wine);
  REQUIRE(w.game == "eldenring");
  REQUIRE(w.config_name == "wine-eldenring");
}

TEST_CASE("caminhos: expansão e saneamento", "[paths]") {
  setenv("HOME", "/home/teste", 1);
  setenv("XDG_PICTURES_DIR", "/fotos", 1);
  REQUIRE(expand_path("~/x") == "/home/teste/x");
  REQUIRE(expand_path("$XDG_PICTURES_DIR/framux") == "/fotos/framux");
  REQUIRE(expand_path("${HOME}/y") == "/home/teste/y");
  REQUIRE(sanitize_filename("a/b\\c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j");
  REQUIRE(sanitize_filename("..") == "_..");
}

TEST_CASE("ring: sobrescrita conta descartes", "[ring]") {
  constexpr uint32_t cap = 8;
  std::vector<uint8_t> mem(ring_bytes(cap));
  auto *r = reinterpret_cast<RingHeader *>(mem.data());
  ring_init(r, cap);
  for (uint64_t i = 0; i < 20; ++i) ring_push(r, FrameRec{i, i, 1, 0, 0, 0});
  REQUIRE(r->dropped.load() == 12);
  std::vector<uint64_t> got;
  uint64_t lost = 0;
  REQUIRE(ring_drain(r, [&](const FrameRec &f) { got.push_back(f.present_ns); }, &lost) == cap);
  REQUIRE(lost == 12);
  REQUIRE(got.front() == 12);
  REQUIRE(got.back() == 19);
  ring_push(r, FrameRec{20, 20, 1, 0, 0, 0});
  got.clear();
  ring_drain(r, [&](const FrameRec &f) { got.push_back(f.present_ns); });
  REQUIRE(got == std::vector<uint64_t>{20});
}

TEST_CASE("overlay: retângulos e cantos", "[overlay]") {
  OverlayContent c;
  c.visible = true;
  c.text = "8";
  c.color = {255, 255, 0};
  OverlayDraw d;
  build_overlay(c, 1920, 1080, d);
  REQUIRE(d.visible);
  REQUIRE(d.text.size() == 7);  // "8" acende os 7 segmentos
  REQUIRE(d.outline.size() == 7);
  for (auto corner : {ipc::Corner::TopLeft, ipc::Corner::TopRight, ipc::Corner::BottomRight, ipc::Corner::BottomLeft}) {
    c.corner = corner;
    c.text = "1234";
    build_overlay(c, 800, 600, d);
    for (const ORect &r : d.outline) {
      REQUIRE(r.x >= 0);
      REQUIRE(r.y >= 0);
      REQUIRE(r.x + r.w <= 800);
      REQUIRE(r.y + r.h <= 600);
    }
    const bool right = corner == ipc::Corner::TopRight || corner == ipc::Corner::BottomRight;
    REQUIRE((d.text.front().x > 400) == right);
  }
  c.corner = ipc::Corner::Hidden;
  build_overlay(c, 800, 600, d);
  REQUIRE_FALSE(d.visible);
}

TEST_CASE("overlay: indicadores do benchmark e do screenshot", "[overlay]") {
  OverlayState st;
  Config::Overlay cfg;
  st.set_config(cfg);
  const uint64_t t = 10'000'000'000ull;
  REQUIRE(st.compute(t, 60, 0).text == "60");
  st.on_indicators(ipc::kIndBenchmark, 0, t);
  auto first = st.compute(t + 500'000'000, 60, 0);
  REQUIRE(first.visible);
  REQUIRE(first.has_box);                                       // caixa verde no 1º segundo
  REQUIRE_FALSE(st.compute(t + 1'500'000'000, 60, 0).visible);  // some depois (hide_after_1s)
  st.on_indicators(ipc::kIndBenchResult, 59.6f, t + 2'000'000'000);
  auto res = st.compute(t + 2'100'000'000, 60, 0);
  REQUIRE(res.visible);
  REQUIRE(res.text == "60");
  REQUIRE(res.color == Rgb{255, 255, 255});
  REQUIRE(st.compute(t + 3'500'000'000, 61, 0).text == "61");
  st.on_indicators(ipc::kIndFlash, 0, t + 4'000'000'000);
  REQUIRE(st.compute(t + 4'050'000'000, 61, 0).box == Rgb{255, 255, 255});
  REQUIRE_FALSE(st.compute(t + 4'200'000'000, 61, 0).has_box);
  // Atalho do overlay: só mostra/oculta, sempre no canto da config.
  st.toggle_visible();
  REQUIRE(st.corner() == ipc::Corner::Hidden);
  st.toggle_visible();
  REQUIRE(st.corner() == ipc::Corner::TopLeft);
  // Atalho de canto: percorre só os cantos visíveis.
  st.next_corner();
  REQUIRE(st.corner() == ipc::Corner::TopRight);
  for (int i = 0; i < 3; ++i) st.next_corner();
  REQUIRE(st.corner() == ipc::Corner::TopLeft);
  // Oculto e com outro canto na config: mostrar volta para o da config.
  Config::Overlay o;
  o.corner = ipc::Corner::BottomRight;
  st.set_config(o);
  st.toggle_visible();
  st.toggle_visible();
  REQUIRE(st.corner() == ipc::Corner::BottomRight);
  REQUIRE(toggled_corner(ipc::Corner::Hidden, ipc::Corner::Hidden) == ipc::Corner::TopLeft);
  REQUIRE(next_corner(ipc::Corner::Hidden, ipc::Corner::BottomLeft) == ipc::Corner::BottomLeft);
}
