// Janela principal (§C.8): abas Geral, FPS e Capturas no topo, linha
// de status do jogo ativo e ícone na bandeja.
#pragma once

#include <QCheckBox>
#include <QLabel>
#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QTimer>

#include "gui/settings.h"
#include "gui/widgets.h"

class QButtonGroup;
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
  // Abre a aba pelo índice (0 Geral, 1 FPS, 2 Capturas).
  void select_tab(int index);

 protected:
  void closeEvent(QCloseEvent *e) override;

 private:
  QWidget *build_general();
  QWidget *build_fps();
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
