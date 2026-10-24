// Arquivos de benchmark: formato nativo (§C.7.2)
// (§C.7.4). As funções format_* são puras, para os testes com fixtures.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "core/bench_stats.h"

namespace framux {

struct BenchFrame {
  uint64_t present_ns;
  uint32_t width, height;
};

struct BenchMeta {
  std::string game;
  std::time_t start_time = 0;  // relógio de parede no início (nome dos arquivos)
  std::string api;             // vulkan | gl_glx | gl_egl
  std::string engine;          // DXVK, vkd3d, wined3d… (vazio = nativo)
  std::string gpu;
  std::string driver;
  std::string timing = "present";
  uint64_t dropped_records = 0;
};

struct BenchOptions {
  bool frametimes = true;
  bool fps = true;
  bool summary = true;
};

struct BenchWriteResult {
  BenchStats stats;
  std::vector<std::string> files;
  std::string summary_path;
  std::string error;  // vazio = sucesso
};

// Nativo
std::string format_native_frametimes(const std::vector<BenchFrame> &frames);
std::string format_native_fps(const BenchStats &s);
std::string format_summary_json(const BenchMeta &m, const BenchStats &s, const std::string &resolution);
std::string format_log_line(const BenchMeta &m, const BenchStats &s);

// Grava os arquivos em `folder` (criado se preciso).
BenchWriteResult write_benchmark(const std::string &folder, const BenchMeta &meta,
                                 const std::vector<BenchFrame> &frames, const BenchOptions &opt);

}  // namespace framux
