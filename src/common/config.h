// Configuração TOML (§C.6): ~/.config/framux/config.toml + overrides por jogo
// em ~/.config/framux/games/<nome>.toml. FRAMUX_CONFIG aponta outro arquivo.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "common/fps_counter.h"
#include "common/ipc_proto.h"

namespace framux {

struct Rgb {
  uint8_t r = 0, g = 0, b = 0;
  bool operator==(const Rgb &) const = default;
};

enum class DuringBenchmark { HideAfter1s, Show, Hide };
enum class ImageFormat { Png, Jpeg, Bmp, Tga };

struct Config {
  struct General {
    bool start_minimized = false;
    bool start_on_login = false;
    bool tray = true;
    // org.freedesktop.Notifications ao salvar arquivos. Sem opção na janela
    // (sempre ligado); os testes desligam pela config.
    bool notifications = true;
  } general;

  struct Hotkeys {
    std::string benchmark = "F11";
    std::string overlay = "F12";               // mostra/oculta no canto da config
    std::string overlay_corner = "Shift+F12";  // alterna o canto do overlay
    std::string screenshot = "F10";
    std::string video = "F9";
    std::string loop_save;
    std::string push_to_talk;
    std::string backend = "auto";  // auto | portal | x11 | ingame | evdev
  } hotkeys;

  struct Overlay {
    ipc::Corner corner = ipc::Corner::TopLeft;
    FpsMode update = FpsMode::Smooth;
    DuringBenchmark during_benchmark = DuringBenchmark::HideAfter1s;
    double scale = 1.0;
    bool show_frametime = false;
    Rgb color_normal{0xC6, 0xF4, 0x32};  // verde-limão da logo
    Rgb color_recording{0xFF, 0x30, 0x30};
    Rgb color_benchmark{0x30, 0xFF, 0x60};
  } overlay;

  struct Benchmark {
    std::string folder = "~/.local/share/framux/benchmarks";
    uint32_t stop_after_s = 0;
    bool log_frametimes = true;
    bool log_fps = true;
    bool log_summary = true;
    std::string timing = "present";  // present | displayed
  } benchmark;

  struct Screenshot {
    std::string folder = "$XDG_PICTURES_DIR/framux";
    ImageFormat format = ImageFormat::Png;
    int jpeg_quality = 92;
    bool include_overlay = false;
    double repeat_every_s = 0;
  } screenshot;

  // Gravação de vídeo (§B.7). Loop, split, lock_fps e cursor são lidos e
  // validados mas ainda não têm efeito.
  struct Video {
    std::string folder = "$XDG_VIDEOS_DIR/framux";
    int fps = 60;
    std::string scale = "full";     // full | half
    std::string encoder = "auto";   // auto | av1_hw | av1_sw | h264_hw | x264 | utvideo
    std::string quality = "high";   // low | medium | high | lossless (força utvideo)
    std::string container = "mkv";  // mkv | mp4 (grava em MKV e faz remux ao parar)
    int split_every_mb = 0;
    bool lock_fps = false;
    bool capture_cursor = true;
    int loop_seconds = 30;
    std::string loop_storage = "ram";
    std::string backpressure = "drop";
  } video;

  struct StatsExport {
    bool enabled = true;
  } stats_export;

  struct Audio {
    bool game = true;
    int game_volume = 100;  // % (0..100), só na gravação
    bool system = false;
    bool microphone = false;
    std::string microphone_device = "default";  // node.name no PipeWire ou "default"
    int microphone_volume = 100;
    bool push_to_talk = false;
    // Com jogo e microfone: além da trilha mixada, uma trilha de cada.
    bool separate_tracks = false;
    std::string channels = "stereo";
    std::string codec = "opus";
  } audio;

  struct Filters {
    std::vector<std::string> exclude{"steamwebhelper", "firefox",   "chrome", "chromium", "thunderbird",
                                     "Xwayland",       "gamescope", "framux", "framuxd",  "obs"};
    std::vector<std::string> include;
  } filters;
};

// Aplica o TOML em `text` sobre `cfg`. Chaves desconhecidas e valores
// inválidos geram avisos e mantêm o valor anterior. Devolve false se o
// texto não for TOML válido (cfg fica inalterado).
bool apply_config_toml(Config &cfg, const std::string &text, const std::string &origin,
                       std::vector<std::string> *warnings);

// Caminho do arquivo principal (FRAMUX_CONFIG ou config_dir()/config.toml).
std::string main_config_path();
std::string game_config_path(const std::string &config_name);

// Padrões + arquivo principal + override do jogo (se config_name não vazio).
Config load_config(const std::string &config_name, std::vector<std::string> *warnings);

std::string config_to_toml(const Config &cfg);

const char *corner_name(ipc::Corner c);
const char *image_format_ext(ImageFormat f);

// Verdadeiro se o processo deve ser ignorado segundo [filters].
bool is_excluded(const Config &cfg, const std::string &exe_base, const std::string &game);

// Modo automático (FRAMUX_AUTO=1, ligado pela GUI): a camada é carregada em
// todo app Vulkan da sessão, mas só age em jogos: lançados pela Steam
// (`steam_game`), rodando no Wine/Proton ou listados em [filters] include.
bool auto_mode_accepts(const Config &cfg, const std::string &exe_base, const std::string &game, bool wine,
                       bool steam_game);

}  // namespace framux
