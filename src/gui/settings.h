// Estado persistente da GUI: config.toml (lido e gravado pela janela) e a
// integração com a sessão (modo automático, abrir ao entrar, daemon).
#pragma once

#include <QFileSystemWatcher>
#include <QString>
#include <QTimer>

#include <functional>
#include <vector>

#include "common/config.h"

namespace framux::gui {

// Config do arquivo principal. Cada alteração é gravada após um curto
// intervalo; o framuxd percebe pelo inotify e repassa aos jogos abertos.
class ConfigStore {
 public:
  ConfigStore();

  const Config &get() const { return cfg_; }
  // Altera e agenda a gravação.
  void edit(const std::function<void(Config &)> &fn);
  // Grava agora o que estiver pendente (ao sair).
  void flush();
  // Chamado quando o arquivo muda por fora (editor de texto, outra GUI).
  void on_external_change(std::function<void()> fn) { external_ = std::move(fn); }

  QString path() const { return path_; }

 private:
  void save();
  void reload_if_external();

  Config cfg_;
  QString path_;
  QTimer save_timer_;
  QFileSystemWatcher watcher_;
  QByteArray last_written_;
  std::function<void()> external_;
};

// Modo automático: FRAMUX=1 e FRAMUX_AUTO=1 em ~/.config/environment.d,
// lidos pelo systemd --user e pela sessão gráfica no próximo login.
bool auto_activation_enabled();
bool set_auto_activation(bool on, QString *err);
// Ligado no arquivo mas ainda não no ambiente desta sessão.
bool auto_activation_needs_relogin();

// Entrada em ~/.config/autostart que abre a GUI ao entrar na sessão.
bool set_open_on_login(bool on, QString *err);

// Daemon via systemd --user (ativação por socket). Iniciar também habilita o
// socket, para o monitoramento voltar sozinho no próximo login.
void start_daemon();
void stop_daemon();

// Manifestos da camada Vulkan instalados para 64 e 32 bits.
struct LayerInstall {
  bool x86_64 = false;
  bool i686 = false;
};
LayerInstall installed_layers();

// Pasta da config já expandida ("~", "$XDG_PICTURES_DIR"…).
QString expanded_folder(const std::string &folder);
// Inverso parcial: o home vira "~" para o arquivo ficar portátil.
std::string folder_for_config(const QString &abs);

}  // namespace framux::gui
