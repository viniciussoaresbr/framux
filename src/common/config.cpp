#include "common/config.h"

#define TOML_EXCEPTIONS 0
#define TOML_HEADER_ONLY 1
// O toml++ usa assert() internamente e alguns textos inválidos o disparam;
// dentro do processo do jogo isso seria um abort. Erros chegam por parse_result.
#define TOML_ASSERT(expr) static_cast<void>(0)
#include <toml++/toml.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

#include "common/hotkey.h"
#include "common/paths.h"

namespace framux {

namespace {

struct Ctx {
  std::string origin;
  std::vector<std::string> *warnings;
  void warn(const std::string &key, const std::string &msg) const {
    if (warnings) warnings->push_back(origin + ": " + key + ": " + msg);
  }
};

using Setter = std::function<void(const toml::node &, const std::string &, const Ctx &)>;

Setter boolean(bool &dst) {
  return [&dst](const toml::node &n, const std::string &k, const Ctx &c) {
    if (auto v = n.value<bool>())
      dst = *v;
    else
      c.warn(k, "esperado true/false");
  };
}

Setter integer(int &dst, int lo, int hi) {
  return [&dst, lo, hi](const toml::node &n, const std::string &k, const Ctx &c) {
    auto v = n.value<int64_t>();
    if (!v) return c.warn(k, "esperado inteiro");
    if (*v < lo || *v > hi) return c.warn(k, "fora do intervalo " + std::to_string(lo) + ".." + std::to_string(hi));
    dst = int(*v);
  };
}

Setter uinteger(uint32_t &dst, uint32_t hi) {
  return [&dst, hi](const toml::node &n, const std::string &k, const Ctx &c) {
    auto v = n.value<int64_t>();
    if (!v) return c.warn(k, "esperado inteiro");
    if (*v < 0 || *v > int64_t(hi)) return c.warn(k, "fora do intervalo 0.." + std::to_string(hi));
    dst = uint32_t(*v);
  };
}

Setter number(double &dst, double lo, double hi) {
  return [&dst, lo, hi](const toml::node &n, const std::string &k, const Ctx &c) {
    auto v = n.value<double>();  // aceita inteiros também
    if (!v) return c.warn(k, "esperado número");
    if (*v < lo || *v > hi) return c.warn(k, "fora do intervalo");
    dst = *v;
  };
}

Setter string(std::string &dst) {
  return [&dst](const toml::node &n, const std::string &k, const Ctx &c) {
    if (auto v = n.value<std::string>())
      dst = *v;
    else
      c.warn(k, "esperado texto");
  };
}

Setter choice(std::string &dst, std::vector<std::string> allowed) {
  return [&dst, allowed](const toml::node &n, const std::string &k, const Ctx &c) {
    auto v = n.value<std::string>();
    if (!v) return c.warn(k, "esperado texto");
    for (const auto &a : allowed)
      if (*v == a) {
        dst = *v;
        return;
      }
    std::string list;
    for (const auto &a : allowed) list += (list.empty() ? "" : " | ") + a;
    c.warn(k, "valor \"" + *v + "\" inválido (" + list + ")");
  };
}

template <class E>
Setter enumeration(E &dst, std::vector<std::pair<std::string, E>> map) {
  return [&dst, map](const toml::node &n, const std::string &k, const Ctx &c) {
    auto v = n.value<std::string>();
    if (!v) return c.warn(k, "esperado texto");
    for (const auto &[name, val] : map)
      if (*v == name) {
        dst = val;
        return;
      }
    std::string list;
    for (const auto &kv : map) list += (list.empty() ? "" : " | ") + kv.first;
    c.warn(k, "valor \"" + *v + "\" inválido (" + list + ")");
  };
}

Setter hotkey(std::string &dst) {
  return [&dst](const toml::node &n, const std::string &k, const Ctx &c) {
    auto v = n.value<std::string>();
    if (!v) return c.warn(k, "esperado texto");
    if (!parse_hotkey(*v)) return c.warn(k, "hotkey \"" + *v + "\" inválida");
    dst = *v;
  };
}

bool parse_color(std::string_view s, Rgb &out) {
  if (s.size() != 7 || s[0] != '#') return false;
  unsigned v = 0;
  for (size_t i = 1; i < 7; ++i) {
    const char ch = s[i];
    unsigned d;
    if (ch >= '0' && ch <= '9')
      d = ch - '0';
    else if (ch >= 'a' && ch <= 'f')
      d = ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F')
      d = ch - 'A' + 10;
    else
      return false;
    v = v * 16 + d;
  }
  out = {uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
  return true;
}

Setter color(Rgb &dst) {
  return [&dst](const toml::node &n, const std::string &k, const Ctx &c) {
    auto v = n.value<std::string>();
    Rgb rgb;
    if (!v || !parse_color(*v, rgb)) return c.warn(k, "esperado cor \"#RRGGBB\"");
    dst = rgb;
  };
}

Setter string_list(std::vector<std::string> &dst) {
  return [&dst](const toml::node &n, const std::string &k, const Ctx &c) {
    const toml::array *arr = n.as_array();
    if (!arr) return c.warn(k, "esperado lista de textos");
    std::vector<std::string> out;
    for (const auto &el : *arr) {
      auto v = el.value<std::string>();
      if (!v) return c.warn(k, "esperado lista de textos");
      out.push_back(*v);
    }
    dst = std::move(out);
  };
}

Setter log_list(Config::Benchmark &b) {
  return [&b](const toml::node &n, const std::string &k, const Ctx &c) {
    const toml::array *arr = n.as_array();
    if (!arr) return c.warn(k, "esperado lista");
    bool ft = false, fps = false, sum = false;
    for (const auto &el : *arr) {
      auto v = el.value<std::string>();
      if (v && *v == "frametimes")
        ft = true;
      else if (v && *v == "fps")
        fps = true;
      else if (v && *v == "summary")
        sum = true;
      else
        return c.warn(k, "itens válidos: frametimes, fps, summary");
    }
    b.log_frametimes = ft;
    b.log_fps = fps;
    b.log_summary = sum;
  };
}

// Chave de versões antigas: aceita sem aviso e sem efeito.
Setter removed() {
  return [](const toml::node &, const std::string &, const Ctx &) {};
}

using Schema = std::map<std::string, std::map<std::string, Setter>>;

Schema make_schema(Config &cfg) {
  auto &g = cfg.general;
  auto &h = cfg.hotkeys;
  auto &o = cfg.overlay;
  auto &b = cfg.benchmark;
  auto &s = cfg.screenshot;
  auto &v = cfg.video;
  auto &a = cfg.audio;
  using C = ipc::Corner;
  return {
      {"general",
       {{"start_minimized", boolean(g.start_minimized)},
        {"start_on_login", boolean(g.start_on_login)},
        {"tray", boolean(g.tray)},
        {"notifications", boolean(g.notifications)},
        {"theme", removed()}}},  // tema da janela, removido na 0.3.6 (sempre escuro)
      {"hotkeys",
       {{"benchmark", hotkey(h.benchmark)},
        {"overlay", hotkey(h.overlay)},
        {"overlay_corner", hotkey(h.overlay_corner)},
        {"screenshot", hotkey(h.screenshot)},
        {"video", hotkey(h.video)},
        {"loop_save", hotkey(h.loop_save)},
        {"push_to_talk", hotkey(h.push_to_talk)},
        {"backend", choice(h.backend, {"auto", "portal", "x11", "ingame", "evdev"})}}},
      {"overlay",
       {{"corner", enumeration(o.corner, {{"top-left", C::TopLeft},
                                          {"top-right", C::TopRight},
                                          {"bottom-right", C::BottomRight},
                                          {"bottom-left", C::BottomLeft},
                                          {"hidden", C::Hidden}})},
        {"update", enumeration(o.update, {{"smooth", FpsMode::Smooth}, {"once_per_second", FpsMode::OncePerSecond}})},
        {"during_benchmark", enumeration(o.during_benchmark, {{"hide_after_1s", DuringBenchmark::HideAfter1s},
                                                              {"show", DuringBenchmark::Show},
                                                              {"hide", DuringBenchmark::Hide}})},
        {"scale", number(o.scale, 0.25, 8.0)},
        {"show_frametime", boolean(o.show_frametime)},
        {"color_normal", color(o.color_normal)},
        {"color_recording", color(o.color_recording)},
        {"color_benchmark", color(o.color_benchmark)}}},
      {"benchmark",
       {{"folder", string(b.folder)},
        {"stop_after_s", uinteger(b.stop_after_s, 86400)},
        {"logs", log_list(b)},
        {"fraps_compat", removed()},  // cópia no formato do FRAPS, removida na 0.3.4
        {"timing", choice(b.timing, {"present", "displayed"})}}},
      {"screenshot",
       {{"folder", string(s.folder)},
        {"format", enumeration(s.format, {{"png", ImageFormat::Png},
                                          {"jpeg", ImageFormat::Jpeg},
                                          {"bmp", ImageFormat::Bmp},
                                          {"tga", ImageFormat::Tga}})},
        {"jpeg_quality", integer(s.jpeg_quality, 1, 100)},
        {"include_overlay", boolean(s.include_overlay)},
        {"repeat_every_s", number(s.repeat_every_s, 0, 86400)}}},
      {"video",
       {{"folder", string(v.folder)},
        {"fps", integer(v.fps, 1, 240)},
        {"scale", choice(v.scale, {"full", "half"})},
        {"encoder", choice(v.encoder, {"auto", "av1_hw", "hevc_hw", "h264_hw", "ffv1", "x264"})},
        {"quality", choice(v.quality, {"low", "medium", "high", "lossless"})},
        {"container", choice(v.container, {"mkv", "mp4"})},
        {"split_every_mb", integer(v.split_every_mb, 0, 1 << 30)},
        {"lock_fps", boolean(v.lock_fps)},
        {"capture_cursor", boolean(v.capture_cursor)},
        {"loop_seconds", integer(v.loop_seconds, 1, 3600)},
        {"loop_storage", choice(v.loop_storage, {"ram", "disk"})},
        {"backpressure", choice(v.backpressure, {"drop", "block"})}}},
      {"stats_export", {{"enabled", boolean(cfg.stats_export.enabled)}}},
      {"audio",
       {{"game", boolean(a.game)},
        {"system", boolean(a.system)},
        {"microphone", boolean(a.microphone)},
        {"microphone_device", string(a.microphone_device)},
        {"push_to_talk", boolean(a.push_to_talk)},
        {"mix", boolean(a.mix)},
        {"channels", choice(a.channels, {"stereo", "original"})},
        {"codec", choice(a.codec, {"opus", "flac", "pcm"})}}},
      {"filters", {{"exclude", string_list(cfg.filters.exclude)}, {"include", string_list(cfg.filters.include)}}},
  };
}

std::string read_file(const std::string &path, bool *exists) {
  std::ifstream in(path, std::ios::binary);
  *exists = bool(in);
  if (!in) return {};
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

}  // namespace

bool apply_config_toml(Config &cfg, const std::string &text, const std::string &origin,
                       std::vector<std::string> *warnings) {
  toml::parse_result res = toml::parse(text, origin);
  if (!res) {
    if (warnings) {
      std::ostringstream ss;
      ss << origin << ":" << res.error().source().begin.line << ": " << res.error().description();
      warnings->push_back(ss.str());
    }
    return false;
  }
  Config next = cfg;
  Schema schema = make_schema(next);
  Ctx ctx{origin, warnings};
  for (const auto &[sec_key, sec_node] : res.table()) {
    const std::string sec(sec_key.str());
    auto it = schema.find(sec);
    const toml::table *tbl = sec_node.as_table();
    if (it == schema.end() || !tbl) {
      ctx.warn(sec, "seção desconhecida");
      continue;
    }
    for (const auto &[key, node] : *tbl) {
      const std::string k(key.str());
      auto st = it->second.find(k);
      if (st == it->second.end()) {
        ctx.warn(sec + "." + k, "chave desconhecida");
        continue;
      }
      st->second(node, sec + "." + k, ctx);
    }
  }
  cfg = std::move(next);
  return true;
}

std::string main_config_path() {
  const char *env = std::getenv("FRAMUX_CONFIG");
  if (env && *env) return env;
  return config_dir() + "/config.toml";
}

std::string game_config_path(const std::string &config_name) {
  return config_dir() + "/games/" + sanitize_filename(config_name) + ".toml";
}

Config load_config(const std::string &config_name, std::vector<std::string> *warnings) {
  Config cfg;
  bool exists = false;
  const std::string main_path = main_config_path();
  std::string text = read_file(main_path, &exists);
  if (exists) apply_config_toml(cfg, text, main_path, warnings);
  if (!config_name.empty()) {
    const std::string game_path = game_config_path(config_name);
    text = read_file(game_path, &exists);
    if (exists) apply_config_toml(cfg, text, game_path, warnings);
  }
  return cfg;
}

const char *corner_name(ipc::Corner c) {
  switch (c) {
    case ipc::Corner::TopLeft: return "top-left";
    case ipc::Corner::TopRight: return "top-right";
    case ipc::Corner::BottomRight: return "bottom-right";
    case ipc::Corner::BottomLeft: return "bottom-left";
    default: return "hidden";
  }
}

const char *image_format_ext(ImageFormat f) {
  switch (f) {
    case ImageFormat::Jpeg: return "jpg";
    case ImageFormat::Bmp: return "bmp";
    case ImageFormat::Tga: return "tga";
    default: return "png";
  }
}

namespace {
std::string color_str(Rgb c) {
  char buf[8];
  std::snprintf(buf, sizeof buf, "#%02X%02X%02X", c.r, c.g, c.b);
  return buf;
}

toml::array str_array(const std::vector<std::string> &v) {
  toml::array a;
  for (const auto &s : v) a.push_back(s);
  return a;
}
}  // namespace

std::string config_to_toml(const Config &c) {
  const char *update = c.overlay.update == FpsMode::Smooth ? "smooth" : "once_per_second";
  const char *during = c.overlay.during_benchmark == DuringBenchmark::Show   ? "show"
                       : c.overlay.during_benchmark == DuringBenchmark::Hide ? "hide"
                                                                             : "hide_after_1s";
  const char *fmt = c.screenshot.format == ImageFormat::Jpeg  ? "jpeg"
                    : c.screenshot.format == ImageFormat::Bmp ? "bmp"
                    : c.screenshot.format == ImageFormat::Tga ? "tga"
                                                              : "png";
  toml::array logs;
  if (c.benchmark.log_frametimes) logs.push_back("frametimes");
  if (c.benchmark.log_fps) logs.push_back("fps");
  if (c.benchmark.log_summary) logs.push_back("summary");

  toml::table t{
      {"general", toml::table{{"start_minimized", c.general.start_minimized},
                              {"start_on_login", c.general.start_on_login},
                              {"tray", c.general.tray},
                              {"notifications", c.general.notifications}}},
      {"hotkeys", toml::table{{"benchmark", c.hotkeys.benchmark},
                              {"overlay", c.hotkeys.overlay},
                              {"overlay_corner", c.hotkeys.overlay_corner},
                              {"screenshot", c.hotkeys.screenshot},
                              {"video", c.hotkeys.video},
                              {"loop_save", c.hotkeys.loop_save},
                              {"push_to_talk", c.hotkeys.push_to_talk},
                              {"backend", c.hotkeys.backend}}},
      {"overlay", toml::table{{"corner", corner_name(c.overlay.corner)},
                              {"update", update},
                              {"during_benchmark", during},
                              {"scale", c.overlay.scale},
                              {"show_frametime", c.overlay.show_frametime},
                              {"color_normal", color_str(c.overlay.color_normal)},
                              {"color_recording", color_str(c.overlay.color_recording)},
                              {"color_benchmark", color_str(c.overlay.color_benchmark)}}},
      {"benchmark", toml::table{{"folder", c.benchmark.folder},
                                {"stop_after_s", int64_t(c.benchmark.stop_after_s)},
                                {"logs", logs},
                                {"timing", c.benchmark.timing}}},
      {"screenshot", toml::table{{"folder", c.screenshot.folder},
                                 {"format", fmt},
                                 {"jpeg_quality", c.screenshot.jpeg_quality},
                                 {"include_overlay", c.screenshot.include_overlay},
                                 {"repeat_every_s", c.screenshot.repeat_every_s}}},
      {"video", toml::table{{"folder", c.video.folder},
                            {"fps", c.video.fps},
                            {"scale", c.video.scale},
                            {"encoder", c.video.encoder},
                            {"quality", c.video.quality},
                            {"container", c.video.container},
                            {"split_every_mb", c.video.split_every_mb},
                            {"lock_fps", c.video.lock_fps},
                            {"capture_cursor", c.video.capture_cursor},
                            {"loop_seconds", c.video.loop_seconds},
                            {"loop_storage", c.video.loop_storage},
                            {"backpressure", c.video.backpressure}}},
      {"stats_export", toml::table{{"enabled", c.stats_export.enabled}}},
      {"audio", toml::table{{"game", c.audio.game},
                            {"system", c.audio.system},
                            {"microphone", c.audio.microphone},
                            {"microphone_device", c.audio.microphone_device},
                            {"push_to_talk", c.audio.push_to_talk},
                            {"mix", c.audio.mix},
                            {"channels", c.audio.channels},
                            {"codec", c.audio.codec}}},
      {"filters", toml::table{{"exclude", str_array(c.filters.exclude)}, {"include", str_array(c.filters.include)}}},
  };
  std::ostringstream ss;
  ss << t << "\n";
  return ss.str();
}

bool auto_mode_accepts(const Config &cfg, const std::string &exe_base, const std::string &game, bool wine,
                       bool steam_game) {
  if (is_excluded(cfg, exe_base, game)) return false;
  if (wine || steam_game) return true;
  for (const auto &n : cfg.filters.include)
    if (n == exe_base || n == game) return true;
  return false;
}

bool is_excluded(const Config &cfg, const std::string &exe_base, const std::string &game) {
  auto matches = [&](const std::string &name) { return name == exe_base || name == game; };
  if (!cfg.filters.include.empty()) {
    for (const auto &n : cfg.filters.include)
      if (matches(n)) return false;
    return true;
  }
  for (const auto &n : cfg.filters.exclude)
    if (matches(n)) return true;
  return false;
}

}  // namespace framux
