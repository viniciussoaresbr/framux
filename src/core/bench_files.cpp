#include "core/bench_files.h"

#include <cctype>
#include <cinttypes>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "common/paths.h"
#include "core/naming.h"

namespace framux {

namespace {

std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char *f, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

std::string json_escape(const std::string &s) {
  std::string out;
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20)
          out += fmt("\\u%04x", c);
        else
          out += char(c);
    }
  }
  return out;
}

// ISO 8601 local com deslocamento "-03:00".
std::string iso_local(std::time_t t) {
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[40];
  std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S%z", &tm);
  std::string s = buf;
  if (s.size() >= 5) s.insert(s.size() - 2, ":");
  return s;
}

std::string human_time(std::time_t t) {
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[40];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

// Camada de tradução que gerou os quadros (DXVK, VKD3D-Proton, WineD3D) ou
// "native". Outros nomes de engine (Unreal, vkcube…) contam como nativos.
std::string layer_name(const std::string &engine) {
  std::string e;
  for (char c : engine) e += char(std::tolower((unsigned char)c));
  for (const char *known : {"dxvk", "vkd3d", "wined3d", "zink"})
    if (e.find(known) != std::string::npos) return known;
  return "native";
}

}  // namespace

std::string format_native_frametimes(const std::vector<BenchFrame> &frames) {
  std::string out = "frame,present_ns,frametime_ms,displayed_ns,width,height\n";
  out.reserve(out.size() + frames.size() * 48);
  for (size_t i = 0; i < frames.size(); ++i) {
    const double ft = i == 0 ? 0.0 : double(frames[i].present_ns - frames[i - 1].present_ns) / 1e6;
    out += fmt("%zu,%" PRIu64 ",%.3f,,%u,%u\n", i + 1, frames[i].present_ns, ft, frames[i].width, frames[i].height);
  }
  return out;
}

std::string format_native_fps(const BenchStats &s) {
  std::string out = "second,frames\n";
  for (size_t i = 0; i < s.fps_per_second.size(); ++i) out += fmt("%zu,%u\n", i + 1, s.fps_per_second[i]);
  return out;
}

std::string format_summary_json(const BenchMeta &m, const BenchStats &s, const std::string &resolution) {
  std::string j = "{";
  j += "\"game\":\"" + json_escape(m.game) + "\",";
  j += "\"start\":\"" + iso_local(m.start_time) + "\",";
  j += fmt("\"duration_ms\":%" PRIu64 ",", s.duration_ms());
  j += fmt("\"frames\":%" PRIu64 ",", s.frames);
  j += fmt("\"avg_fps\":%.3f,\"min_fps\":%u,\"max_fps\":%u,", s.avg_fps, s.min_fps, s.max_fps);
  j += fmt("\"frametime_ms\":{\"avg\":%.3f,\"p50\":%.3f,\"p90\":%.3f,\"p95\":%.3f,\"p99\":%.3f},", s.ft_avg_ms,
           s.ft_p50_ms, s.ft_p90_ms, s.ft_p95_ms, s.ft_p99_ms);
  j += fmt("\"low_1pct_fps\":%.3f,\"low_01pct_fps\":%.3f,", s.low_1pct_fps, s.low_01pct_fps);
  j += "\"low_definition\":\"mean_of_worst\",";
  j += "\"api\":\"" + json_escape(m.api) + "\",";
  j += "\"layer\":\"" + json_escape(layer_name(m.engine)) + "\",";
  j += "\"gpu\":\"" + json_escape(m.gpu) + "\",";
  j += "\"driver\":\"" + json_escape(m.driver) + "\",";
  j += "\"resolution\":\"" + json_escape(resolution) + "\",";
  j += "\"timing\":\"" + json_escape(m.timing) + "\",";
  j += fmt("\"dropped_records\":%" PRIu64, m.dropped_records);
  j += "}\n";
  return j;
}

std::string format_log_line(const BenchMeta &m, const BenchStats &s) {
  return human_time(m.start_time) + " - " + m.game +
         fmt(" - Frames: %" PRIu64 " - Time: %" PRIu64
             "ms - Avg: %.3f - Min: %u - Max: %u - 1%% low: %.1f - 0.1%% low: %.1f\n",
             s.frames, s.duration_ms(), s.avg_fps, s.min_fps, s.max_fps, s.low_1pct_fps, s.low_01pct_fps);
}

namespace {

bool write_new(const std::string &dir, const std::string &stem, const std::string &suffix, const std::string &data,
               BenchWriteResult &r, std::string *path_out = nullptr) {
  std::string path, err;
  const int fd = create_unique(dir, stem, suffix, &path, &err);
  if (fd < 0 || !write_all_close(fd, data, &err)) {
    if (r.error.empty()) r.error = err;
    return false;
  }
  r.files.push_back(path);
  if (path_out) *path_out = path;
  return true;
}

}  // namespace

BenchWriteResult write_benchmark(const std::string &folder, const BenchMeta &meta,
                                 const std::vector<BenchFrame> &frames, const BenchOptions &opt) {
  BenchWriteResult r;
  std::vector<uint64_t> t;
  t.reserve(frames.size());
  for (const auto &f : frames) t.push_back(f.present_ns);
  r.stats = compute_stats(t);

  if (!make_dirs(folder, &r.error)) return r;
  const std::string game = sanitize_filename(meta.game);
  const std::string stem = game + " " + file_timestamp(meta.start_time);
  const std::string resolution =
      frames.empty() ? std::string() : fmt("%ux%u", frames.back().width, frames.back().height);

  if (opt.frametimes) write_new(folder, stem, " frametimes.csv", format_native_frametimes(frames), r);
  if (opt.fps) write_new(folder, stem, " fps.csv", format_native_fps(r.stats), r);
  if (opt.summary)
    write_new(folder, stem, " summary.json", format_summary_json(meta, r.stats, resolution), r, &r.summary_path);
  std::string err;
  if (append_file(folder + "/benchmarks.log", format_log_line(meta, r.stats), &err))
    r.files.push_back(folder + "/benchmarks.log");
  else if (r.error.empty())
    r.error = err;
  return r;
}

}  // namespace framux
