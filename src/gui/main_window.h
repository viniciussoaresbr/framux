// Janela principal (§C.8): abas Geral, FPS, Gravações e Capturas no topo, linha
// de status do jogo ativo e ícone na bandeja.
#pragma once

#include <QCheckBox>
#include <QLabel>
#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QTimer>

#include <memory>

#include "gui/audio_devices.h"
#include "gui/settings.h"
#include "gui/widgets.h"

class QButtonGroup;
class QComboBox;
class QFormLayout;
class QRadioButton;
class QSlider;
class QSpinBox;
class QStackedWidget;

namespace framux::gui {

class MainWindow : public QMainWindow {
 public:
  explicit MainWindow(ConfigStore &store);

  // Mostra a janela (ou só a bandeja, se `minimized` e houver bandeja).
  void present(bool minimized);
  bool tray_active() const { return tray_ && tray_->isVisible(); }
  // Abre a aba pelo índice (0 Geral, 1 FPS, 2 Gravações, 3 Capturas).
  void select_tab(int index);

 protected:
  void closeEvent(QCloseEvent *e) override;

 private:
  QWidget *build_general();
  QWidget *build_fps();
  QWidget *build_recordings();
  QWidget *build_screenshots();
  void build_tray();
  void update_tray();

  // Copia a config para os controles (início e mudanças externas).
  void load_from_config();
  // Recusa atalho repetido: devolve a mensagem de erro ou vazio.
  QString check_hotkey(const std::string &h, const std::string Config::Hotkeys::*self) const;

  void poll_status();
  void apply_status(const QByteArray &json);
  void set_status(const QString &dot_color, const QString &text);
  void update_environment();
  // Encoder escolhido × encoders que o daemon encontrou nesta máquina.
  void update_encoder_hint();
  // Itens do combo de encoder: só os que o daemon encontrou.
  void fill_encoders();
  // Combo de microfones: padrão do sistema e as fontes do PipeWire.
  void fill_microphones();
  // Habilita volumes, microfone e trilhas conforme o que vai ser gravado.
  void update_audio_controls();
  // Liga o medidor só com a aba Gravações à vista e o microfone marcado.
  void update_meter();
  // Opção de inicialização para jogos OpenGL (a do Flatpak, se integrado).
  QString launch_command() const;

  ConfigStore &store_;
  bool loading_ = false;

  NavBar *tabs_ = nullptr;
  QStackedWidget *pages_ = nullptr;
  QLabel *status_dot_ = nullptr;
  QLabel *status_text_ = nullptr;
  QTimer poll_timer_;
  bool daemon_up_ = false;
  QString hotkey_backend_;

  // Geral
  QCheckBox *auto_activate_ = nullptr;
  QLabel *auto_hint_ = nullptr;
  QCheckBox *flatpak_ = nullptr;  // só com o Steam Flatpak instalado
  QLabel *flatpak_hint_ = nullptr;
  QLabel *run_cmd_ = nullptr;
  bool flatpak_restart_ = false;  // mudou nesta execução: avisa para reiniciar o Steam
  QCheckBox *open_on_login_ = nullptr;
  QCheckBox *start_minimized_ = nullptr;
  QCheckBox *tray_check_ = nullptr;
  QLabel *tray_hint_ = nullptr;
  QLabel *env_session_ = nullptr;
  QLabel *env_layer64_ = nullptr;
  QLabel *env_layer32_ = nullptr;
  QLabel *env_hotkeys_ = nullptr;
  QLabel *env_daemon_ = nullptr;
  QLabel *env_auto_ = nullptr;
  QLabel *env_flatpak_ = nullptr;

  // FPS
  CornerPreview *preview_ = nullptr;
  HotkeyButton *overlay_key_ = nullptr;
  HotkeyButton *overlay_corner_key_ = nullptr;
  ColorButton *color_ = nullptr;
  QCheckBox *frametime_ = nullptr;
  QRadioButton *update_smooth_ = nullptr;
  QRadioButton *update_once_ = nullptr;
  HotkeyButton *bench_key_ = nullptr;
  QCheckBox *bench_stop_ = nullptr;
  QSpinBox *bench_stop_s_ = nullptr;
  QCheckBox *log_frametimes_ = nullptr;
  QCheckBox *log_fps_ = nullptr;
  QCheckBox *log_summary_ = nullptr;
  FolderRow *bench_folder_ = nullptr;

  // Gravações
  HotkeyButton *video_key_ = nullptr;
  QButtonGroup *video_fps_ = nullptr;  // 30, 60 ou 0 (outro)
  QSpinBox *video_fps_custom_ = nullptr;
  QRadioButton *video_full_ = nullptr;
  QRadioButton *video_half_ = nullptr;
  QComboBox *video_encoder_ = nullptr;
  QLabel *video_encoder_hint_ = nullptr;
  QLabel *video_size_hint_ = nullptr;  // estimativa de MB por minuto
  QComboBox *video_quality_ = nullptr;
  QCheckBox *video_mp4_ = nullptr;
  FolderRow *video_folder_ = nullptr;
  QCheckBox *audio_game_ = nullptr;
  QSlider *audio_game_volume_ = nullptr;
  QCheckBox *audio_mic_ = nullptr;
  QComboBox *audio_mic_device_ = nullptr;
  QSlider *audio_mic_volume_ = nullptr;
  LevelMeter *audio_mic_level_ = nullptr;
  QCheckBox *audio_separate_ = nullptr;
  QFormLayout *audio_form_ = nullptr;
  std::unique_ptr<AudioDevices> audio_devices_;
  uint64_t audio_devices_rev_ = 0;
  QTimer meter_timer_;
  RecentVideos *recent_videos_ = nullptr;
  QStringList encoders_;      // do StatusJson; vazio = daemon parado/desconhecido
  QString encoders_missing_;  // explicação do que falta (ex.: RPM Fusion)

  // Screenshots
  HotkeyButton *shot_key_ = nullptr;
  QButtonGroup *format_ = nullptr;
  QFormLayout *shot_form_ = nullptr;  // para mostrar a qualidade só no JPG
  QSlider *jpeg_quality_ = nullptr;
  QLabel *jpeg_quality_value_ = nullptr;
  QCheckBox *shot_overlay_ = nullptr;
  QCheckBox *shot_repeat_ = nullptr;
  QSpinBox *shot_repeat_s_ = nullptr;
  FolderRow *shot_folder_ = nullptr;
  RecentShots *recent_ = nullptr;

  QSystemTrayIcon *tray_ = nullptr;
  bool told_about_tray_ = false;
};

}  // namespace framux::gui
