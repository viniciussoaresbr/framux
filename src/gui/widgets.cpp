#include "gui/widgets.h"

#include <X11/keysym.h>

#include <QApplication>
#include <QColorDialog>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <thread>

#include "common/hotkey.h"
#include "common/overlay.h"
#include "gui/settings.h"
#include "gui/theme.h"

namespace framux::gui {

namespace {

constexpr int kBrandName = 22;  // altura do nome, px

}  // namespace

bool confirm(QWidget *parent, const QString &title, const QString &text) {
  QDialog d(parent);
  d.setObjectName("confirm");
  d.setWindowTitle("Framux");
  d.setModal(true);
  auto *v = new QVBoxLayout(&d);
  v->setContentsMargins(24, 22, 24, 20);
  v->setSpacing(10);
  auto *t = new QLabel(title, &d);
  t->setObjectName("confirmTitle");
  auto *m = new QLabel(text, &d);
  m->setObjectName("confirmText");
  m->setWordWrap(true);
  m->setMinimumWidth(360);
  v->addWidget(t);
  v->addWidget(m);
  v->addSpacing(10);
  auto *buttons = new QHBoxLayout;
  buttons->setSpacing(10);
  buttons->addStretch();
  auto *no = new QPushButton("Não", &d);
  no->setObjectName("confirmNo");
  auto *yes = new QPushButton("Sim", &d);
  yes->setObjectName("primary");
  yes->setDefault(true);
  buttons->addWidget(no);
  buttons->addWidget(yes);
  v->addLayout(buttons);
  QObject::connect(no, &QPushButton::clicked, &d, &QDialog::reject);
  QObject::connect(yes, &QPushButton::clicked, &d, &QDialog::accept);
  d.setFixedSize(d.sizeHint());
  return d.exec() == QDialog::Accepted;
}

Brand::Brand(QWidget *parent) : QWidget(parent) {
  setObjectName("brand");
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

QSize Brand::sizeHint() const {
  const QSize name = wordmark(kBrandName, Qt::white, 1.0).size();
  return {name.width(), name.height() + 4};
}

void Brand::paintEvent(QPaintEvent *) {
  QPainter p(this);
  const qreal dpr = devicePixelRatioF();
  const QColor text = palette().color(QPalette::WindowText);
  if (name_.isNull() || name_color_ != text.rgb() || name_dpr_ != dpr) {
    name_ = wordmark(kBrandName, text, dpr);
    name_color_ = text.rgb();
    name_dpr_ = dpr;
  }
  const QSizeF name = name_.deviceIndependentSize();
  p.drawPixmap(QPointF(0, (height() - name.height()) / 2), name_);
}

// ---------------------------------------------------------------------------
// NavBar

namespace {

constexpr int kNavItemW = 112, kNavItemH = 60, kNavPad = 6;
constexpr int kPillW = 52, kPillH = 28;

struct NavColors {
  QColor bg, border, pill, hover, icon_on, icon_off, text_on, text_off;
};

// Cores da logo: verde-limão #C6F432 sobre o cinza escuro do tema.
NavColors nav_colors() {
  return {QColor(0x26, 0x27, 0x2c), QColor(0x34, 0x35, 0x3b), QColor(0xC6, 0xF4, 0x32, 46), QColor(255, 255, 255, 14),
          QColor(0xC6, 0xF4, 0x32), QColor(0x8e, 0x92, 0x9b), QColor(0xF5, 0xF6, 0xF1),     QColor(0x8e, 0x92, 0x9b)};
}

// Ícones de traço desenhados num quadro de 20×20 centrado em `c`.
void draw_nav_icon(QPainter &p, NavBar::Icon icon, QPointF c, const QColor &color) {
  p.save();
  p.translate(c - QPointF(10, 10));
  QPen pen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
  p.setPen(pen);
  p.setBrush(Qt::NoBrush);
  switch (icon) {
    case NavBar::Icon::General:  // controles deslizantes
      for (int i = 0; i < 3; ++i) {
        const qreal y = 4 + i * 6, knob = i == 1 ? 13 : 6;
        p.drawLine(QPointF(2, y), QPointF(18, y));
        p.setBrush(color);
        p.drawEllipse(QPointF(knob, y), 2.2, 2.2);
        p.setBrush(Qt::NoBrush);
      }
      break;
    case NavBar::Icon::Fps: {  // velocímetro
      p.drawArc(QRectF(1.5, 3, 17, 17), 0, 180 * 16);
      p.drawLine(QPointF(1.5, 11.5), QPointF(18.5, 11.5));
      p.drawLine(QPointF(10, 11.5), QPointF(14.5, 6));
      p.setBrush(color);
      p.drawEllipse(QPointF(10, 11.5), 1.8, 1.8);
      break;
    }
    case NavBar::Icon::Recordings: {  // filmadora
      p.drawRoundedRect(QRectF(1.5, 5, 12, 10), 2.5, 2.5);
      const QPointF lens[] = {{13.5, 8.5}, {18.5, 5.5}, {18.5, 14.5}, {13.5, 11.5}};
      p.drawPolygon(lens, 4);
      p.setBrush(color);
      p.drawEllipse(QPointF(5.5, 8.5), 1.4, 1.4);
      break;
    }
    case NavBar::Icon::Screenshots:  // câmera
      p.drawRoundedRect(QRectF(1.5, 5, 17, 12), 2.5, 2.5);
      p.drawLine(QPointF(6.5, 5), QPointF(8, 2.5));
      p.drawLine(QPointF(8, 2.5), QPointF(12, 2.5));
      p.drawLine(QPointF(12, 2.5), QPointF(13.5, 5));
      p.drawEllipse(QPointF(10, 11), 3.3, 3.3);
      break;
  }
  p.restore();
}

}  // namespace

NavBar::NavBar(QWidget *parent) : QWidget(parent) {
  setObjectName("navBar");
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  setMouseTracking(true);
  setFocusPolicy(Qt::TabFocus);
  setCursor(Qt::PointingHandCursor);
}

void NavBar::add(const QString &text, Icon icon) {
  items_.push_back({text, icon});
  updateGeometry();
  update();
}

void NavBar::set_current(int index) {
  if (index < 0 || index >= int(items_.size()) || index == current_) return;
  current_ = index;
  update();
  if (on_change) on_change(index);
}

QSize NavBar::sizeHint() const { return {kNavPad * 2 + kNavItemW * int(items_.size()), kNavPad * 2 + kNavItemH}; }

QRect NavBar::item_rect(int i) const { return {kNavPad + i * kNavItemW, kNavPad, kNavItemW, kNavItemH}; }

int NavBar::item_at(QPoint pos) const {
  for (int i = 0; i < int(items_.size()); ++i)
    if (item_rect(i).contains(pos)) return i;
  return -1;
}

void NavBar::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const NavColors c = nav_colors();
  p.setPen(QPen(c.border, 1));
  p.setBrush(c.bg);
  p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14, 14);

  QFont f = font();
  for (int i = 0; i < int(items_.size()); ++i) {
    const QRect r = item_rect(i);
    const bool on = i == current_;
    const QRectF pill(r.center().x() - kPillW / 2.0 + 0.5, r.top() + 5, kPillW, kPillH);
    if (on || i == hover_) {
      p.setPen(Qt::NoPen);
      p.setBrush(on ? c.pill : c.hover);
      p.drawRoundedRect(pill, kPillH / 2.0, kPillH / 2.0);
    }
    draw_nav_icon(p, items_[i].icon, pill.center(), on ? c.icon_on : c.icon_off);
    f.setWeight(on ? QFont::Bold : QFont::Medium);
    p.setFont(f);
    p.setPen(on ? c.text_on : c.text_off);
    p.drawText(QRect(r.left(), int(pill.bottom()) + 3, r.width(), r.bottom() - int(pill.bottom()) - 3),
               Qt::AlignHCenter | Qt::AlignTop, items_[i].text);
  }
  if (key_focus_) {
    p.setPen(QPen(c.icon_on, 1.5));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(item_rect(current_)).adjusted(4, 2, -4, -2), 10, 10);
  }
}

void NavBar::mousePressEvent(QMouseEvent *e) {
  if (e->button() == Qt::LeftButton) set_current(item_at(e->position().toPoint()));
}

void NavBar::mouseMoveEvent(QMouseEvent *e) {
  const int h = item_at(e->position().toPoint());
  if (h != hover_) {
    hover_ = h;
    update();
  }
}

void NavBar::leaveEvent(QEvent *) {
  hover_ = -1;
  update();
}

void NavBar::focusInEvent(QFocusEvent *e) {
  key_focus_ = e->reason() == Qt::TabFocusReason || e->reason() == Qt::BacktabFocusReason;
  update();
}

void NavBar::focusOutEvent(QFocusEvent *) {
  key_focus_ = false;
  update();
}

void NavBar::keyPressEvent(QKeyEvent *e) {
  if (e->key() == Qt::Key_Left)
    set_current(current_ - 1);
  else if (e->key() == Qt::Key_Right)
    set_current(current_ + 1);
  else
    QWidget::keyPressEvent(e);
}

namespace {

// Tecla do Qt → keysym X11 da sintaxe da config. 0 = não suportada.
uint32_t keysym_for(const QKeyEvent *e) {
  const int k = e->key();
  const bool keypad = e->modifiers() & Qt::KeypadModifier;
  if (k >= Qt::Key_A && k <= Qt::Key_Z) return XK_a + uint32_t(k - Qt::Key_A);
  if (k >= Qt::Key_0 && k <= Qt::Key_9) return (keypad ? XK_KP_0 : XK_0) + uint32_t(k - Qt::Key_0);
  if (k >= Qt::Key_F1 && k <= Qt::Key_F24) return XK_F1 + uint32_t(k - Qt::Key_F1);
  switch (k) {
    case Qt::Key_Tab: return XK_Tab;
    case Qt::Key_Space: return XK_space;
    case Qt::Key_Return: return XK_Return;
    case Qt::Key_Enter: return keypad ? XK_KP_Enter : XK_Return;
    case Qt::Key_Backspace: return XK_BackSpace;
    case Qt::Key_Insert: return XK_Insert;
    case Qt::Key_Delete: return XK_Delete;
    case Qt::Key_Home: return XK_Home;
    case Qt::Key_End: return XK_End;
    case Qt::Key_PageUp: return XK_Prior;
    case Qt::Key_PageDown: return XK_Next;
    case Qt::Key_Left: return XK_Left;
    case Qt::Key_Right: return XK_Right;
    case Qt::Key_Up: return XK_Up;
    case Qt::Key_Down: return XK_Down;
    case Qt::Key_Print: return XK_Print;
    case Qt::Key_ScrollLock: return XK_Scroll_Lock;
    case Qt::Key_Pause: return XK_Pause;
    case Qt::Key_Menu: return XK_Menu;
    case Qt::Key_QuoteLeft: return XK_grave;
    case Qt::Key_Minus: return keypad ? XK_KP_Subtract : XK_minus;
    case Qt::Key_Equal: return XK_equal;
    case Qt::Key_BracketLeft: return XK_bracketleft;
    case Qt::Key_BracketRight: return XK_bracketright;
    case Qt::Key_Backslash: return XK_backslash;
    case Qt::Key_Semicolon: return XK_semicolon;
    case Qt::Key_Apostrophe: return XK_apostrophe;
    case Qt::Key_Comma: return XK_comma;
    case Qt::Key_Period: return keypad ? XK_KP_Decimal : XK_period;
    case Qt::Key_Slash: return keypad ? XK_KP_Divide : XK_slash;
    case Qt::Key_Asterisk: return XK_KP_Multiply;
    case Qt::Key_Plus: return XK_KP_Add;
    default: return 0;
  }
}

uint32_t mods_for(Qt::KeyboardModifiers m) {
  uint32_t out = 0;
  if (m & Qt::ControlModifier) out |= ipc::kModCtrl;
  if (m & Qt::ShiftModifier) out |= ipc::kModShift;
  if (m & Qt::AltModifier) out |= ipc::kModAlt;
  if (m & Qt::MetaModifier) out |= ipc::kModSuper;
  return out;
}

bool is_modifier_key(int k) {
  return k == Qt::Key_Control || k == Qt::Key_Shift || k == Qt::Key_Alt || k == Qt::Key_Meta || k == Qt::Key_AltGr ||
         k == Qt::Key_Super_L || k == Qt::Key_Super_R;
}

QColor qcolor(Rgb c) { return QColor(c.r, c.g, c.b); }

// "✕" desenhado: nem toda fonte tem o glifo.
QIcon cross_icon(QColor color) {
  QPixmap pm(32, 32);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(color, 3.2, Qt::SolidLine, Qt::RoundCap));
  p.drawLine(QPointF(9, 9), QPointF(23, 23));
  p.drawLine(QPointF(23, 9), QPointF(9, 23));
  return QIcon(pm);
}

}  // namespace

// ---------------------------------------------------------------------------
// HotkeyButton

HotkeyButton::HotkeyButton(QWidget *parent) : QPushButton(parent) {
  setMinimumWidth(150);
  setFocusPolicy(Qt::StrongFocus);
  setToolTip("Clique e pressione a tecla ou combinação (Esc cancela)");
  connect(this, &QPushButton::clicked, [this] { capturing_ ? stop_capture() : start_capture(); });
  refresh();
}

void HotkeyButton::set_hotkey(const std::string &h) {
  value_ = h;
  refresh();
}

void HotkeyButton::refresh() {
  if (capturing_) {
    setText("Pressione uma tecla…");
    setProperty("capturing", true);
  } else {
    setText(value_.empty() ? "Desativado" : QString::fromStdString(value_));
    setProperty("capturing", false);
  }
  style()->unpolish(this);
  style()->polish(this);
}

void HotkeyButton::start_capture() {
  capturing_ = true;
  setFocus();
  grabKeyboard();
  refresh();
}

void HotkeyButton::stop_capture() {
  if (!capturing_) return;
  capturing_ = false;
  releaseKeyboard();
  refresh();
}

void HotkeyButton::commit(const std::string &h) {
  stop_capture();
  if (h == value_) return;
  if (on_change) {
    const QString err = on_change(h);
    if (!err.isEmpty()) {
      setToolTip(err);
      setText(err);
      QTimer::singleShot(1800, this, [this] {
        setToolTip("Clique e pressione a tecla ou combinação (Esc cancela)");
        refresh();
      });
      return;
    }
  }
  value_ = h;
  refresh();
}

void HotkeyButton::keyPressEvent(QKeyEvent *e) {
  if (!capturing_) return QPushButton::keyPressEvent(e);
  if (is_modifier_key(e->key())) return;  // espera a tecla principal
  if (e->key() == Qt::Key_Escape && e->modifiers() == Qt::NoModifier) return stop_capture();
  Hotkey hk;
  hk.keysym = keysym_for(e);
  hk.mods = mods_for(e->modifiers());
  const std::string text = hk.keysym ? format_hotkey(hk) : "";
  if (text.empty() || !parse_hotkey(text)) {
    setText("Tecla não suportada");
    return;
  }
  commit(text);
}

void HotkeyButton::mousePressEvent(QMouseEvent *e) {
  if (!capturing_) return QPushButton::mousePressEvent(e);
  // Numeração X11: 2 = meio, 8/9 = laterais (voltar/avançar).
  int button = 0;
  switch (e->button()) {
    case Qt::MiddleButton: button = 2; break;
    case Qt::BackButton: button = 8; break;
    case Qt::ForwardButton: button = 9; break;
    default: return stop_capture();  // clique comum cancela
  }
  Hotkey hk;
  hk.keysym = kMouseKeysymBase + uint32_t(button);
  hk.mods = mods_for(e->modifiers());
  commit(format_hotkey(hk));
}

void HotkeyButton::focusOutEvent(QFocusEvent *e) {
  stop_capture();
  QPushButton::focusOutEvent(e);
}

QWidget *hotkey_row(HotkeyButton *button, QWidget *parent) {
  auto *w = new QWidget(parent);
  auto *l = new QHBoxLayout(w);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(6);
  auto *clear = new QToolButton(w);
  clear->setObjectName("clearKey");
  clear->setIcon(cross_icon(button->palette().color(QPalette::ButtonText)));
  clear->setToolTip("Desativar este atalho");
  QObject::connect(clear, &QToolButton::clicked, button, [button] {
    if (button->hotkey().empty()) return;
    if (button->on_change && !button->on_change("").isEmpty()) return;
    button->set_hotkey("");
  });
  l->addWidget(button);
  l->addWidget(clear);
  l->addStretch();
  return w;
}

// ---------------------------------------------------------------------------
// ColorButton

LevelMeter::LevelMeter(QWidget *parent) : QWidget(parent) {
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  setMaximumWidth(260);
}

void LevelMeter::push(float peak) {
  const float db = peak > 0 ? 20.0f * std::log10(peak) : -60.0f;
  const float target = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
  const float next = std::max(target, level_ - 0.04f);
  if (next == level_) return;
  level_ = next;
  update();
}

void LevelMeter::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0x16, 0x17, 0x1a));
  p.drawRoundedRect(r, 4, 4);
  if (level_ <= 0) return;
  const qreal w = r.width() * level_;
  QLinearGradient g(r.topLeft(), r.topRight());
  g.setColorAt(0, QColor(0x3e, 0xc4, 0x6d));
  g.setColorAt(0.78, QColor(0x3e, 0xc4, 0x6d));  // -12 dB
  g.setColorAt(0.8, QColor(0xe8, 0xc5, 0x2d));
  g.setColorAt(0.95, QColor(0xe8, 0xc5, 0x2d));  // -3 dB
  g.setColorAt(0.97, QColor(0xe5, 0x53, 0x4b));
  QPainterPath clip;
  clip.addRoundedRect(r, 4, 4);
  p.setClipPath(clip);
  p.fillRect(QRectF(r.left(), r.top(), w, r.height()), g);
}

ColorButton::ColorButton(QWidget *parent) : QPushButton(parent) {
  setMinimumWidth(130);
  connect(this, &QPushButton::clicked, [this] {
    const QColor c = QColorDialog::getColor(qcolor(color_), window(), "Cor do contador");
    if (!c.isValid()) return;
    color_ = Rgb{uint8_t(c.red()), uint8_t(c.green()), uint8_t(c.blue())};
    refresh();
    if (on_change) on_change(color_);
  });
}

void ColorButton::set_color(Rgb c) {
  color_ = c;
  refresh();
}

void ColorButton::refresh() {
  QPixmap swatch(28, 16);
  swatch.fill(Qt::transparent);
  QPainter p(&swatch);
  p.setRenderHint(QPainter::Antialiasing);
  p.setPen(QPen(QColor(0, 0, 0, 90), 1));
  p.setBrush(qcolor(color_));
  p.drawRoundedRect(QRectF(0.5, 0.5, 27, 15), 4, 4);
  p.end();
  setIcon(QIcon(swatch));
  setIconSize(swatch.size());
  setText(qcolor(color_).name().toUpper());
}

// ---------------------------------------------------------------------------
// CornerPreview

CornerPreview::CornerPreview(QWidget *parent) : QWidget(parent) {
  setMouseTracking(true);
  setCursor(Qt::PointingHandCursor);
  setToolTip("Clique em um canto para posicionar o contador");
  QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
  sp.setHeightForWidth(true);
  setSizePolicy(sp);
  // Com a janela larga a prévia cresceria e empurraria o Benchmark para
  // baixo; a miniatura 16:9 fica centralizada nessa altura.
  setMaximumHeight(200);
}

void CornerPreview::set_state(ipc::Corner corner, Rgb color, bool frametime) {
  corner_ = corner;
  color_ = color;
  frametime_ = frametime;
  update();
}

QRect CornerPreview::screen_rect() const {
  int w = width(), h = w * 9 / 16;
  if (h > height()) {
    h = height();
    w = h * 16 / 9;
  }
  return QRect((width() - w) / 2, (height() - h) / 2, w, h);
}

ipc::Corner CornerPreview::corner_at(QPoint p) const {
  const QRect r = screen_rect();
  if (!r.contains(p)) return ipc::Corner::Hidden;
  const bool right = p.x() > r.center().x(), bottom = p.y() > r.center().y();
  if (bottom) return right ? ipc::Corner::BottomRight : ipc::Corner::BottomLeft;
  return right ? ipc::Corner::TopRight : ipc::Corner::TopLeft;
}

void CornerPreview::paintEvent(QPaintEvent *) {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QRect r = screen_rect();
  QPainterPath clip;
  clip.addRoundedRect(QRectF(r), 8, 8);
  p.setClipPath(clip);

  // "Cena de jogo" genérica: céu, sol e montanhas.
  QLinearGradient sky(r.topLeft(), r.bottomLeft());
  sky.setColorAt(0, QColor(0x1d, 0x2b, 0x53));
  sky.setColorAt(0.65, QColor(0x7e, 0x25, 0x53));
  sky.setColorAt(1, QColor(0xff, 0x77, 0x4d));
  p.fillRect(r, sky);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0xff, 0xcc, 0x66, 200));
  p.drawEllipse(QPointF(r.left() + r.width() * 0.62, r.top() + r.height() * 0.62), r.width() * 0.09, r.width() * 0.09);
  QPainterPath hills;
  hills.moveTo(r.bottomLeft());
  hills.lineTo(r.left(), r.top() + r.height() * 0.72);
  hills.lineTo(r.left() + r.width() * 0.25, r.top() + r.height() * 0.55);
  hills.lineTo(r.left() + r.width() * 0.45, r.top() + r.height() * 0.75);
  hills.lineTo(r.left() + r.width() * 0.7, r.top() + r.height() * 0.6);
  hills.lineTo(r.right(), r.top() + r.height() * 0.78);
  hills.lineTo(r.bottomRight());
  hills.closeSubpath();
  p.setBrush(QColor(0x14, 0x10, 0x2a));
  p.drawPath(hills);

  if (hover_ != ipc::Corner::Hidden && hover_ != corner_) {
    QRect q(r.topLeft(), r.size() / 2);
    if (hover_ == ipc::Corner::TopRight || hover_ == ipc::Corner::BottomRight) q.moveLeft(r.center().x());
    if (hover_ == ipc::Corner::BottomLeft || hover_ == ipc::Corner::BottomRight) q.moveTop(r.center().y());
    p.fillRect(q, QColor(255, 255, 255, 28));
  }

  // O contador sai do mesmo código do jogo. A tela virtual é pequena (o
  // tamanho mínimo do dígito) para o número ficar legível na miniatura.
  OverlayContent c;
  c.visible = true;
  c.corner = corner_;
  c.text = "74";
  if (frametime_) c.subtext = "13.5";
  c.color = color_;
  OverlayDraw d;
  constexpr int kVw = 384, kVh = 216;
  build_overlay(c, kVw, kVh, d);
  p.save();
  p.translate(r.topLeft());
  p.scale(r.width() / double(kVw), r.height() / double(kVh));
  p.setRenderHint(QPainter::Antialiasing, false);
  auto fill = [&](const std::vector<ORect> &rects, OColor col) {
    const QColor qc = QColor::fromRgbF(col.r, col.g, col.b, col.a);
    for (const ORect &o : rects) p.fillRect(QRect(o.x, o.y, o.w, o.h), qc);
  };
  fill(d.outline, d.outline_color);
  fill(d.text, d.text_color);
  p.restore();

  p.setClipping(false);
  p.setPen(QPen(palette().color(QPalette::Mid), 1));
  p.setBrush(Qt::NoBrush);
  p.drawRoundedRect(QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);
}

void CornerPreview::mousePressEvent(QMouseEvent *e) {
  const ipc::Corner c = corner_at(e->position().toPoint());
  if (c == ipc::Corner::Hidden || c == corner_) return;
  corner_ = c;
  update();
  if (on_corner) on_corner(c);
}

void CornerPreview::mouseMoveEvent(QMouseEvent *e) {
  const ipc::Corner c = corner_at(e->position().toPoint());
  if (c != hover_) {
    hover_ = c;
    update();
  }
}

void CornerPreview::leaveEvent(QEvent *) {
  hover_ = ipc::Corner::Hidden;
  update();
}

// ---------------------------------------------------------------------------
// FolderRow

FolderRow::FolderRow(QWidget *parent) : QWidget(parent) {
  auto *l = new QHBoxLayout(this);
  l->setContentsMargins(0, 0, 0, 0);
  l->setSpacing(6);
  label_ = new QLabel(this);
  label_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  label_->setMinimumWidth(120);
  label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  auto *change = new QPushButton("Alterar", this);
  auto *open = new QPushButton("Abrir", this);
  l->addWidget(label_, 1);
  l->addWidget(change);
  l->addWidget(open);
  connect(change, &QPushButton::clicked, [this] {
    const QString start = expanded_folder(folder_);
    const QString dir = QFileDialog::getExistingDirectory(window(), "Escolher pasta", start);
    if (dir.isEmpty()) return;
    set_folder(folder_for_config(dir));
    if (on_change) on_change(folder_);
  });
  connect(open, &QPushButton::clicked, [this] {
    const QString dir = expanded_folder(folder_);
    QDir().mkpath(dir);  // como no FRAPS, a pasta só nasce quando precisa
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
  });
}

void FolderRow::set_folder(const std::string &folder) {
  folder_ = folder;
  const QString abs = expanded_folder(folder);
  QString shown = abs;
  if (shown.startsWith(QDir::homePath())) shown = "~" + shown.mid(QDir::homePath().size());
  label_->setText(shown);
  label_->setToolTip(abs);
}

// ---------------------------------------------------------------------------
// RecentShots

namespace {
constexpr int kRecentCount = 8;
const QSize kThumb(160, 90);
}  // namespace

RecentShots::RecentShots(QWidget *parent) : QListWidget(parent) {
  setViewMode(QListView::IconMode);
  setFlow(QListView::LeftToRight);
  setWrapping(false);
  setMovement(QListView::Static);
  setIconSize(kThumb);
  setSpacing(6);
  setFixedHeight(kThumb.height() + 48);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setSelectionMode(QAbstractItemView::NoSelection);
  setCursor(Qt::PointingHandCursor);
  connect(this, &QListWidget::itemClicked, [](QListWidgetItem *it) {
    const QString path = it->data(Qt::UserRole).toString();
    if (!path.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
  });
  // O arquivo aparece antes de terminar de ser escrito: espera um pouco.
  connect(&watcher_, &QFileSystemWatcher::directoryChanged, this,
          [this] { QTimer::singleShot(400, this, [this] { refresh(); }); });
}

void RecentShots::set_folder(const QString &abs) {
  if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
  folder_ = abs;
  if (QDir(abs).exists()) watcher_.addPath(abs);
  refresh();
}

void RecentShots::refresh() {
  clear();
  QDir dir(folder_);
  if (dir.exists() && !watcher_.directories().contains(folder_)) watcher_.addPath(folder_);
  const QFileInfoList files =
      dir.entryInfoList({"*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tga"}, QDir::Files, QDir::Time);
  if (files.isEmpty()) {
    auto *it = new QListWidgetItem("Nenhum screenshot ainda — use o atalho dentro de um jogo", this);
    it->setFlags(Qt::NoItemFlags);
    return;
  }
  QPixmap placeholder(kThumb);
  placeholder.fill(palette().color(QPalette::AlternateBase));
  QStringList paths;
  for (int i = 0; i < files.size() && i < kRecentCount; ++i) {
    const QFileInfo &fi = files[i];
    auto *it = new QListWidgetItem(QIcon(placeholder), fi.lastModified().toString("dd/MM HH:mm:ss"), this);
    it->setData(Qt::UserRole, fi.absoluteFilePath());
    it->setToolTip(fi.fileName());
    paths << fi.absoluteFilePath();
  }
  // Decodificar PNGs grandes trava a janela: miniaturas numa thread.
  QPointer<RecentShots> self(this);
  std::thread([self, paths] {
    for (int i = 0; i < paths.size(); ++i) {
      QImageReader reader(paths[i]);
      reader.setAutoTransform(true);
      const QSize sz = reader.size();
      if (sz.isValid()) reader.setScaledSize(sz.scaled(kThumb, Qt::KeepAspectRatio));
      const QImage img = reader.read();
      if (img.isNull()) continue;
      QMetaObject::invokeMethod(
          qApp,
          [self, i, path = paths[i], img] {
            if (!self || i >= self->count()) return;
            QListWidgetItem *it = self->item(i);
            if (it->data(Qt::UserRole).toString() == path) it->setIcon(QIcon(QPixmap::fromImage(img)));
          },
          Qt::QueuedConnection);
    }
  }).detach();
}

// ---------------------------------------------------------------------------
// RecentVideos

RecentVideos::RecentVideos(QWidget *parent) : QListWidget(parent) {
  setSelectionMode(QAbstractItemView::NoSelection);
  setCursor(Qt::PointingHandCursor);
  setFixedHeight(5 * 26 + 8);
  connect(this, &QListWidget::itemClicked, [](QListWidgetItem *it) {
    const QString path = it->data(Qt::UserRole).toString();
    if (!path.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(path));
  });
  // Durante a gravação o tamanho muda a cada escrita: atualiza com folga.
  connect(&watcher_, &QFileSystemWatcher::directoryChanged, this,
          [this] { QTimer::singleShot(400, this, [this] { refresh(); }); });
}

void RecentVideos::set_folder(const QString &abs) {
  if (!watcher_.directories().isEmpty()) watcher_.removePaths(watcher_.directories());
  folder_ = abs;
  if (QDir(abs).exists()) watcher_.addPath(abs);
  refresh();
}

void RecentVideos::refresh() {
  clear();
  QDir dir(folder_);
  if (dir.exists() && !watcher_.directories().contains(folder_)) watcher_.addPath(folder_);
  const QFileInfoList files = dir.entryInfoList({"*.mkv", "*.mp4"}, QDir::Files, QDir::Time);
  if (files.isEmpty()) {
    auto *it = new QListWidgetItem("Nenhuma gravação ainda — use o atalho dentro de um jogo", this);
    it->setFlags(Qt::NoItemFlags);
    return;
  }
  const QLocale locale;
  for (int i = 0; i < files.size() && i < kRecentCount; ++i) {
    const QFileInfo &fi = files[i];
    auto *it = new QListWidgetItem(QString("%1   ·   %2   ·   %3")
                                       .arg(fi.completeBaseName(), fi.lastModified().toString("dd/MM HH:mm"),
                                            locale.formattedDataSize(fi.size(), 1, QLocale::DataSizeSIFormat)),
                                   this);
    it->setData(Qt::UserRole, fi.absoluteFilePath());
    it->setToolTip(fi.absoluteFilePath());
  }
}

}  // namespace framux::gui
