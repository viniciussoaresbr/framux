// GUI (§C.8): cliques e teclas simulados gravam o config.toml esperado.
// Roda com QT_QPA_PLATFORM=offscreen, HOME e XDG_CONFIG_HOME temporários
// (definidos aqui) e sem barramento de sessão, para não tocar no usuário.

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "common/config.h"
#include "gui/main_window.h"
#include "gui/settings.h"
#include "gui/widgets.h"

using namespace framux;

namespace {

int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "FALHOU %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

template <typename T>
T *find(QWidget &root, const char *name) {
  // findChild<T> exige Q_OBJECT; os widgets do Framux não usam moc.
  T *w = dynamic_cast<T *>(root.findChild<QWidget *>(name));
  if (!w) std::fprintf(stderr, "widget %s não encontrado\n", name);
  return w;
}

}  // namespace

int main(int argc, char **argv) {
  QTemporaryDir tmp;
  if (!tmp.isValid()) return 1;
  qputenv("HOME", tmp.path().toUtf8());
  qputenv("XDG_CONFIG_HOME", (tmp.path() + "/cfg").toUtf8());
  qputenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent");
  qunsetenv("FRAMUX_CONFIG");
  qunsetenv("FRAMUX_AUTO");
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");

  QApplication app(argc, argv);
  gui::ConfigStore store;
  gui::MainWindow w(store);
  w.show();
  QApplication::processEvents();

  // Canto: clique no quadrante inferior direito da prévia.
  w.select_tab(1);
  auto *preview = find<gui::CornerPreview>(w, "cornerPreview");
  if (!preview) return 1;
  QApplication::processEvents();
  // A miniatura 16:9 fica centralizada no widget: clica a 1/4 do centro.
  const int sw = std::min(preview->width(), preview->height() * 16 / 9), sh = sw * 9 / 16;
  QTest::mouseClick(preview, Qt::LeftButton, {}, preview->rect().center() + QPoint(sw / 4, sh / 4));

  // Atalho por teclado: Ctrl+F8 no overlay.
  auto *overlay = find<gui::HotkeyButton>(w, "overlayKey");
  if (!overlay) return 1;
  QTest::mouseClick(overlay, Qt::LeftButton);
  QTest::keyClick(overlay, Qt::Key_F8, Qt::ControlModifier);
  CHECK(overlay->hotkey() == "Ctrl+F8");

  // Atalho de canto: padrão Shift+F12; repetir o do overlay é recusado.
  auto *corner_key = find<gui::HotkeyButton>(w, "overlayCornerKey");
  if (!corner_key) return 1;
  CHECK(corner_key->hotkey() == "Shift+F12");
  QTest::mouseClick(corner_key, Qt::LeftButton);
  QTest::keyClick(corner_key, Qt::Key_F8, Qt::ControlModifier);
  CHECK(corner_key->hotkey() == "Shift+F12");

  // Esc cancela sem alterar.
  QTest::mouseClick(overlay, Qt::LeftButton);
  QTest::keyClick(overlay, Qt::Key_Escape);
  CHECK(overlay->hotkey() == "Ctrl+F8");

  // Botão do meio do mouse no benchmark.
  auto *bench = find<gui::HotkeyButton>(w, "benchKey");
  if (!bench) return 1;
  QTest::mouseClick(bench, Qt::LeftButton);
  QTest::mouseClick(bench, Qt::MiddleButton);
  CHECK(bench->hotkey() == "Mouse2");

  // Gravações: F9 por padrão; FPS "Outro" com 45, metade, MP4 e sem áudio.
  w.select_tab(2);
  auto *video = find<gui::HotkeyButton>(w, "videoKey");
  if (!video) return 1;
  CHECK(video->hotkey() == "F9");
  QTest::mouseClick(video, Qt::LeftButton);
  QTest::keyClick(video, Qt::Key_F10);  // já é do screenshot
  CHECK(video->hotkey() == "F9");
  CHECK(find<QRadioButton>(w, "videoFps60") && find<QRadioButton>(w, "videoFps60")->isChecked());
  if (auto *custom = find<QRadioButton>(w, "videoFpsCustom")) custom->setChecked(true);
  if (auto *spin = find<QSpinBox>(w, "videoFpsValue")) spin->setValue(45);
  if (auto *mp4 = find<QCheckBox>(w, "videoMp4")) mp4->setChecked(true);
  if (auto *audio = find<QCheckBox>(w, "audioGame")) audio->setChecked(false);
  if (auto *quality = find<QComboBox>(w, "videoQuality")) quality->setCurrentIndex(quality->findData("medium"));

  // Atalho repetido é recusado.
  w.select_tab(3);
  auto *shot = find<gui::HotkeyButton>(w, "shotKey");
  if (!shot) return 1;
  QTest::mouseClick(shot, Qt::LeftButton);
  QTest::keyClick(shot, Qt::Key_F8, Qt::ControlModifier);
  CHECK(shot->hotkey() == "F10");

  auto *jpg = find<QRadioButton>(w, "formatJPG");
  if (jpg) QTest::mouseClick(jpg, Qt::LeftButton);

  w.select_tab(0);
  if (auto *autobox = find<QCheckBox>(w, "autoActivate")) autobox->setChecked(true);

  store.flush();
  std::vector<std::string> warnings;
  const Config c = load_config("", &warnings);
  CHECK(warnings.empty());
  CHECK(c.overlay.corner == ipc::Corner::BottomRight);
  CHECK(c.hotkeys.overlay == "Ctrl+F8");
  CHECK(c.hotkeys.benchmark == "Mouse2");
  CHECK(c.hotkeys.screenshot == "F10");
  CHECK(c.screenshot.format == ImageFormat::Jpeg);
  CHECK(c.hotkeys.video == "F9");
  CHECK(c.video.fps == 45);
  CHECK(c.video.container == "mp4");
  CHECK(c.video.quality == "medium");
  CHECK(!c.audio.game);

  QFile env(tmp.path() + "/.config/environment.d/90-framux.conf");
  CHECK(env.open(QIODevice::ReadOnly) && env.readAll().contains("FRAMUX_AUTO=1"));
  CHECK(gui::auto_activation_needs_relogin());

  // Sem a seção Aparência: nem seletor de tema nem opção de notificação.
  CHECK(!w.findChild<QWidget *>("theme"));

  // Caixa de confirmação: "Não" recusa, "Sim" aceita.
  for (const char *button : {"confirmNo", "primary"}) {
    QTimer::singleShot(100, [button] {
      for (QWidget *top : QApplication::topLevelWidgets())
        if (top->objectName() == "confirm" && top->isVisible())
          if (auto *b = dynamic_cast<QPushButton *>(top->findChild<QWidget *>(button))) b->click();
    });
    const bool ok = gui::confirm(&w, "Parar o Framux?", "Teste");
    CHECK(ok == (std::string(button) == "primary"));
  }

  // Mudança feita por fora (editor de texto) chega à janela.
  {
    QFile f(store.path());
    CHECK(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write("[overlay]\ncorner = \"top-right\"\n");
  }
  for (int i = 0; i < 40 && store.get().overlay.corner != ipc::Corner::TopRight; ++i) QTest::qWait(50);
  CHECK(store.get().overlay.corner == ipc::Corner::TopRight);

  if (failures == 0) std::printf("ok\n");
  return failures ? 1 : 0;
}
