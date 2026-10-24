#include "gui/theme.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QStyleFactory>

namespace framux::gui {

namespace {

// Destaque com o verde-limão da logo.
const QColor kAccent(0xC6, 0xF4, 0x32);

QPalette dark_palette() {
  QPalette p;
  const QColor window(0x1f, 0x20, 0x24), base(0x16, 0x17, 0x1a), text(0xe8, 0xe8, 0xea), mid(0x3a, 0x3b, 0x41);
  p.setColor(QPalette::Window, window);
  p.setColor(QPalette::WindowText, text);
  p.setColor(QPalette::Base, base);
  p.setColor(QPalette::AlternateBase, QColor(0x26, 0x27, 0x2c));
  p.setColor(QPalette::ToolTipBase, QColor(0x2a, 0x2b, 0x30));
  p.setColor(QPalette::ToolTipText, text);
  p.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x8b, 0x91));
  p.setColor(QPalette::Text, text);
  p.setColor(QPalette::Button, QColor(0x2b, 0x2c, 0x32));
  p.setColor(QPalette::ButtonText, text);
  p.setColor(QPalette::BrightText, Qt::white);
  p.setColor(QPalette::Light, QColor(0x45, 0x46, 0x4d));
  p.setColor(QPalette::Midlight, QColor(0x36, 0x37, 0x3d));
  p.setColor(QPalette::Mid, mid);
  p.setColor(QPalette::Dark, QColor(0x12, 0x12, 0x15));
  p.setColor(QPalette::Shadow, Qt::black);
  p.setColor(QPalette::Highlight, kAccent);
  p.setColor(QPalette::HighlightedText, QColor(0x16, 0x19, 0x1e));
  p.setColor(QPalette::Link, kAccent);
  p.setColor(QPalette::Accent, kAccent);
  for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
    p.setColor(QPalette::Disabled, role, QColor(0x6c, 0x6d, 0x73));
  return p;
}

QString stylesheet() {
  const QString card = "#26272c", border = "#34353b", accent = "#C6F432", muted = "#9a9ba1";
  const QString button = "#2e2f35", button_hover = "#36373e";
  // QString::arg com vários argumentos não é posicional: preenche os
  // marcadores do menor para o maior. Por isso %1…%6 sem lacunas.
  return QString(R"(
QFrame#card { background: %1; border: 1px solid %2; border-radius: 10px; }
QLabel#cardTitle { font-weight: 700; font-size: 11pt; }
QLabel#muted, QLabel#hint { color: %4; }
QLabel#warn { color: #e8912d; }
QWidget#statusBar { background: %1; border: 1px solid %2; border-radius: 8px; }
QPushButton, QToolButton#clearKey {
  padding: 5px 14px; border-radius: 6px; border: 1px solid %2; background: %5;
}
QToolButton#clearKey { padding: 5px 6px; }
QPushButton:hover, QToolButton#clearKey:hover { background: %6; border-color: %3; }
QPushButton:pressed { background: %2; }
QPushButton:disabled { color: %4; }
QPushButton[capturing="true"] { border: 2px solid %3; }
QListWidget { background: transparent; border: none; }
QScrollArea#page, QWidget#pageBody { background: transparent; border: none; }
QDialog#confirm { background: %1; }
QLabel#confirmTitle { font-weight: 700; font-size: 12pt; }
QLabel#confirmText { color: %4; }
QDialog#confirm QPushButton { min-width: 96px; padding: 7px 18px; border-radius: 8px; }
QPushButton#primary { background: %3; border-color: %3; color: #16191E; font-weight: 700; }
QPushButton#primary:hover { background: #D4FA5C; border-color: #D4FA5C; }
QPushButton#primary:pressed { background: #AFD924; border-color: #AFD924; }
)")
      .arg(card, border, accent, muted, button, button_hover);
}

}  // namespace

void apply_theme() {
  QApplication::setStyle(QStyleFactory::create("Fusion"));
  QApplication::setPalette(dark_palette());
  qApp->setStyleSheet(stylesheet());
}

QIcon app_icon(int state) {
  QIcon icon;
  for (int size : {16, 24, 32, 48, 64, 128, 256, 512}) {
    QPixmap pm(QString(":/framux/framux-%1.png").arg(size));
    if (state == 1) {
      QPainter p(&pm);
      p.setRenderHint(QPainter::Antialiasing);
      const qreal d = size * 0.36, m = size * 0.02;
      p.setPen(QPen(QColor(0x16, 0x19, 0x1e), qMax(1.0, size / 32.0)));
      p.setBrush(QColor(0x30, 0xFF, 0x60));
      p.drawEllipse(QRectF(size - d - m, size - d - m, d, d));
    }
    icon.addPixmap(pm);
  }
  return icon;
}

QPixmap wordmark(int height, const QColor &text, qreal dpr) {
  static const QImage src(":/framux/wordmark.png");
  QImage img =
      src.scaledToHeight(qRound(height * dpr), Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
  // As letras são quase brancas e sem saturação; o ponto vermelho fica como está.
  for (int y = 0; y < img.height(); ++y) {
    auto *row = reinterpret_cast<QRgb *>(img.scanLine(y));
    for (int x = 0; x < img.width(); ++x) {
      const QColor c = QColor::fromRgba(row[x]);
      if (c.alpha() && c.hsvSaturation() < 60) row[x] = qRgba(text.red(), text.green(), text.blue(), c.alpha());
    }
  }
  QPixmap pm = QPixmap::fromImage(img);
  pm.setDevicePixelRatio(dpr);
  return pm;
}

}  // namespace framux::gui
