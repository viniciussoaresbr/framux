#include "gui/settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>

#include <cstdlib>

#include "common/paths.h"
#include "framux_build_config.h"

namespace framux::gui {

namespace {

constexpr int kSaveDelayMs = 300;

QString env_d_path() { return QString::fromStdString(home_dir()) + "/.config/environment.d/90-framux.conf"; }

QString autostart_path() {
  const char *xdg = std::getenv("XDG_CONFIG_HOME");
  const QString base = xdg && *xdg ? QString(xdg) : QString::fromStdString(home_dir()) + "/.config";
  return base + "/autostart/io.github.framux.desktop";
}

bool write_file(const QString &path, const QByteArray &data, QString *err) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QSaveFile f(path);
  if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
    if (err) *err = f.errorString();
    return false;
  }
  return true;
}

bool remove_file(const QString &path, QString *err) {
  QFile f(path);
  if (!f.exists() || f.remove()) return true;
  if (err) *err = f.errorString();
  return false;
}

void systemctl(const QStringList &args) { QProcess::startDetached("systemctl", QStringList{"--user"} + args); }

}  // namespace

ConfigStore::ConfigStore() : path_(QString::fromStdString(main_config_path())) {
  cfg_ = load_config("", nullptr);
  save_timer_.setSingleShot(true);
  save_timer_.setInterval(kSaveDelayMs);
  QObject::connect(&save_timer_, &QTimer::timeout, [this] { save(); });

  // Observa o diretório: gravações atômicas trocam o inode do arquivo.
  const QString dir = QFileInfo(path_).absolutePath();
  QDir().mkpath(dir);
  watcher_.addPath(dir);
  if (QFile::exists(path_)) watcher_.addPath(path_);
  QObject::connect(&watcher_, &QFileSystemWatcher::directoryChanged, [this] { reload_if_external(); });
  QObject::connect(&watcher_, &QFileSystemWatcher::fileChanged, [this] { reload_if_external(); });
}

void ConfigStore::edit(const std::function<void(Config &)> &fn) {
  fn(cfg_);
  save_timer_.start();
}

void ConfigStore::flush() {
  if (save_timer_.isActive()) {
    save_timer_.stop();
    save();
  }
}

void ConfigStore::save() {
  const QByteArray data = QByteArray("# Gerado pela janela do Framux; pode ser editado à mão.\n") +
                          QByteArray::fromStdString(config_to_toml(cfg_));
  last_written_ = data;
  write_file(path_, data, nullptr);
  if (!watcher_.files().contains(path_)) watcher_.addPath(path_);
}

void ConfigStore::reload_if_external() {
  if (save_timer_.isActive()) return;  // a edição local prevalece
  QFile f(path_);
  if (!f.open(QIODevice::ReadOnly)) return;
  const QByteArray data = f.readAll();
  if (data == last_written_) return;
  last_written_ = data;
  cfg_ = load_config("", nullptr);
  if (!watcher_.files().contains(path_)) watcher_.addPath(path_);
  if (external_) external_();
}

bool auto_activation_enabled() { return QFile::exists(env_d_path()); }

bool set_auto_activation(bool on, QString *err) {
  if (!on) return remove_file(env_d_path(), err);
  return write_file(env_d_path(),
                    "# Gerado pelo Framux (Geral → Ativar automaticamente nos jogos).\n"
                    "# Carrega a camada Vulkan em jogos da Steam/Proton/Wine sem `framux run`.\n"
                    "FRAMUX=1\n"
                    "FRAMUX_AUTO=1\n",
                    err);
}

bool auto_activation_needs_relogin() {
  const char *a = std::getenv("FRAMUX_AUTO");
  return auto_activation_enabled() && !(a && QByteArray(a) == "1");
}

bool set_open_on_login(bool on, QString *err) {
  if (!on) return remove_file(autostart_path(), err);
  const QByteArray exec = QByteArray(FRAMUX_BINDIR) + "/framux-gui --autostart";
  return write_file(autostart_path(),
                    "[Desktop Entry]\n"
                    "Type=Application\n"
                    "Name=Framux\n"
                    "Exec=" +
                        exec +
                        "\n"
                        "Icon=io.github.framux\n"
                        "X-GNOME-Autostart-enabled=true\n"
                        "NoDisplay=true\n",
                    err);
}

void start_daemon() {
  QFile::remove(QString::fromStdString(stopped_path()));
  systemctl({"enable", "--now", "framuxd.socket"});
}

void stop_daemon() {
  // Sem a marca, a camada carregada pelo modo automático seguiria
  // desenhando o contador sozinha (modo sem daemon).
  if (const std::string p = stopped_path(); !p.empty()) write_file(QString::fromStdString(p), {}, nullptr);
  systemctl({"stop", "framuxd.socket", "framuxd.service"});
}

LayerInstall installed_layers() {
  QStringList dirs;
  const char *xdg = std::getenv("XDG_DATA_HOME");
  dirs << (xdg && *xdg ? QString(xdg) : QString::fromStdString(home_dir()) + "/.local/share") << QString(FRAMUX_DATADIR)
       << "/usr/share" << "/etc";
  LayerInstall li;
  for (const QString &d : dirs) {
    const QString base = d + "/vulkan/implicit_layer.d/";
    li.x86_64 |= QFile::exists(base + "framux_x86_64.json");
    li.i686 |= QFile::exists(base + "framux_i686.json");
  }
  return li;
}

QString expanded_folder(const std::string &folder) { return QString::fromStdString(expand_path(folder)); }

std::string folder_for_config(const QString &abs) {
  const QString home = QDir::homePath();
  if (abs == home) return "~";
  if (abs.startsWith(home + "/")) return ("~" + abs.mid(home.size())).toStdString();
  return abs.toStdString();
}

}  // namespace framux::gui
