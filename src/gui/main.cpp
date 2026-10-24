// framux-gui: janela de configuração e ícone na bandeja (§C.8). O
// monitoramento em si é do framuxd e da camada; a janela só configura e
// mostra o estado, então fechá-la não para nada.
//
// uso: framux-gui [--autostart] [--snapshot <pasta>]

#include <QApplication>
#include <QCommandLineParser>
#include <QDialog>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTimer>

#include "common/flatpak.h"
#include "common/paths.h"
#include "framux_build_config.h"
#include "gui/main_window.h"
#include "gui/settings.h"
#include "gui/theme.h"
#include "gui/widgets.h"

using namespace framux::gui;

namespace {

QString instance_socket() {
  const std::string dir = framux::runtime_dir();
  if (dir.empty()) return "framux-gui";
  QDir().mkpath(QString::fromStdString(dir));
  return QString::fromStdString(dir) + "/gui.sock";
}

// Já existe uma janela aberta? Pede para ela aparecer e sai.
bool forward_to_running(const QString &name) {
  QLocalSocket s;
  s.connectToServer(name);
  if (!s.waitForConnected(300)) return false;
  s.write("show\n");
  s.waitForBytesWritten(300);
  return true;
}

// Captura cada aba (usado nos testes e na documentação).
int snapshot(MainWindow &w, const QString &dir) {
  QDir().mkpath(dir);
  w.show();
  const char *names[] = {"general", "fps", "screenshots"};
  for (int i = 0; i < 3; ++i) {
    w.select_tab(i);
    QApplication::processEvents();
    if (!w.grab().save(QString("%1/%2.png").arg(dir, names[i]))) return 1;
  }
  // A caixa de "Parar o Framux": captura e fecha com "Não".
  QTimer::singleShot(200, [dir] {
    for (QWidget *top : QApplication::topLevelWidgets())
      if (top->objectName() == "confirm" && top->isVisible()) {
        top->grab().save(dir + "/confirm.png");
        static_cast<QDialog *>(top)->reject();
      }
  });
  confirm(&w, "Parar o Framux?", "Contador, atalhos e screenshots param até você abrir o Framux de novo.");
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  QApplication::setApplicationName("framux");
  QApplication::setApplicationDisplayName("Framux");
  QApplication::setApplicationVersion(FRAMUX_VERSION);
  QGuiApplication::setDesktopFileName("io.github.framux");

  QCommandLineParser args;
  args.addHelpOption();
  args.addVersionOption();
  QCommandLineOption autostart("autostart", "Início automático da sessão (respeita \"Iniciar minimizado\").");
  QCommandLineOption snap("snapshot", "Salva imagens das abas em <pasta> e sai.", "pasta");
  args.addOption(autostart);
  args.addOption(snap);
  args.process(app);

  const bool snapshot_mode = args.isSet(snap);
  const QString sock = instance_socket();
  if (!snapshot_mode && forward_to_running(sock)) return 0;

  ConfigStore store;
  apply_theme();
  if (!snapshot_mode) {
    start_daemon();
    // Pacote atualizado: renova as cópias usadas pelo Steam Flatpak.
    std::string err;
    framux::flatpak::sync(&err);
  }

  MainWindow window(store);
  if (snapshot_mode) return snapshot(window, args.value(snap));

  QLocalServer server;
  QLocalServer::removeServer(sock);
  server.listen(sock);
  QObject::connect(&server, &QLocalServer::newConnection, [&] {
    while (QLocalSocket *c = server.nextPendingConnection()) {
      QObject::connect(c, &QLocalSocket::readyRead, c, [c, &window] {
        c->readAll();
        window.present(false);
        c->disconnectFromServer();
      });
      QObject::connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
    }
  });

  const bool minimized = args.isSet(autostart) && store.get().general.start_minimized;
  // Minimizado sem bandeja não há o que mostrar: o daemon já foi iniciado.
  if (minimized && !window.tray_active()) return 0;
  window.present(minimized);

  QObject::connect(&app, &QApplication::aboutToQuit, [&store] { store.flush(); });
  return app.exec();
}
