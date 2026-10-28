// Widgets da janela: captura de atalho, cor, prévia do overlay, pasta e
// miniaturas. Sem moc: notificações por callback.
#pragma once

#include <QFileSystemWatcher>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

#include <functional>
#include <string>
#include <vector>

#include "common/config.h"

namespace framux::gui {

// Caixa de confirmação nas cores do app, com "Sim" e "Não". Esc = Não. A
// barra da janela diz só "Framux"; `title` vai em destaque dentro da caixa.
bool confirm(QWidget *parent, const QString &title, const QString &text);

// Nome "Framux" à esquerda das abas, na cor do texto do tema.
class Brand : public QWidget {
 public:
  explicit Brand(QWidget *parent);
  QSize sizeHint() const override;

 protected:
  void paintEvent(QPaintEvent *) override;

 private:
  QPixmap name_;  // cache do nome para a cor e a escala atuais
  QRgb name_color_ = 0;
  qreal name_dpr_ = 0;
};

// Abas da janela: ícone e rótulo; a selecionada fica com o ícone numa
// pílula verde-limão (cores da logo). Setas ←/→ trocam com o foco.
class NavBar : public QWidget {
 public:
  enum class Icon { General, Fps, Recordings, Screenshots };

  explicit NavBar(QWidget *parent);
  void add(const QString &text, Icon icon);
  int current() const { return current_; }
  void set_current(int index);
  QSize sizeHint() const override;

  std::function<void(int)> on_change;

 protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void leaveEvent(QEvent *) override;
  void keyPressEvent(QKeyEvent *e) override;
  void focusInEvent(QFocusEvent *e) override;
  void focusOutEvent(QFocusEvent *e) override;

 private:
  struct Item {
    QString text;
    Icon icon;
  };
  QRect item_rect(int i) const;
  int item_at(QPoint pos) const;

  std::vector<Item> items_;
  int current_ = 0;
  int hover_ = -1;
  bool key_focus_ = false;  // contorno só quando o foco veio pelo teclado
};

// Botão "pressione a tecla" (§C.8). Esc cancela; teclas e botões do meio e
// laterais do mouse são aceitos, com Ctrl/Shift/Alt/Super.
class HotkeyButton : public QPushButton {
 public:
  explicit HotkeyButton(QWidget *parent = nullptr);

  void set_hotkey(const std::string &h);
  std::string hotkey() const { return value_; }
  // Recebe o novo valor; devolve uma mensagem de erro para recusá-lo.
  std::function<QString(const std::string &)> on_change;

 protected:
  void keyPressEvent(QKeyEvent *e) override;
  void mousePressEvent(QMouseEvent *e) override;
  void focusOutEvent(QFocusEvent *e) override;

 private:
  void start_capture();
  void stop_capture();
  void commit(const std::string &h);
  void refresh();

  std::string value_;
  bool capturing_ = false;
};

// Linha "atalho + botão desativar".
QWidget *hotkey_row(HotkeyButton *button, QWidget *parent);

class ColorButton : public QPushButton {
 public:
  explicit ColorButton(QWidget *parent = nullptr);
  void set_color(Rgb c);
  Rgb color() const { return color_; }
  std::function<void(Rgb)> on_change;

 private:
  void refresh();
  Rgb color_;
};

// Nível do microfone em dB (-60 a 0): verde, amarelo a partir de -12 dB e
// vermelho perto de estourar. Sobe na hora e desce devagar, como o medidor
// de uma mesa de som.
class LevelMeter : public QWidget {
 public:
  explicit LevelMeter(QWidget *parent = nullptr);
  // Pico (0..1) desde a última chamada; 0 com o medidor parado.
  void push(float peak);
  QSize sizeHint() const override { return {260, 10}; }

 protected:
  void paintEvent(QPaintEvent *) override;

 private:
  float level_ = 0;  // 0..1 na escala de dB
};

// Miniatura de uma tela com o contador desenhado pelo mesmo código do jogo.
// Clicar num quadrante escolhe o canto.
class CornerPreview : public QWidget {
 public:
  explicit CornerPreview(QWidget *parent = nullptr);
  void set_state(ipc::Corner corner, Rgb color, bool frametime);
  std::function<void(ipc::Corner)> on_corner;

  QSize sizeHint() const override { return {320, 180}; }
  QSize minimumSizeHint() const override { return {240, 135}; }
  bool hasHeightForWidth() const override { return true; }
  int heightForWidth(int w) const override { return w * 9 / 16; }

 protected:
  void paintEvent(QPaintEvent *) override;
  void mousePressEvent(QMouseEvent *e) override;
  void mouseMoveEvent(QMouseEvent *e) override;
  void leaveEvent(QEvent *) override;

 private:
  QRect screen_rect() const;
  ipc::Corner corner_at(QPoint p) const;

  ipc::Corner corner_ = ipc::Corner::TopLeft;
  ipc::Corner hover_ = ipc::Corner::Hidden;
  Rgb color_;
  bool frametime_ = false;
};

// "Pasta  ~/Imagens/framux  [Alterar] [Abrir]".
class FolderRow : public QWidget {
 public:
  explicit FolderRow(QWidget *parent = nullptr);
  void set_folder(const std::string &folder);
  std::function<void(const std::string &)> on_change;

 private:
  QLabel *label_;
  std::string folder_;
};

// Faixa com os screenshots mais recentes; clique abre a imagem.
class RecentShots : public QListWidget {
 public:
  explicit RecentShots(QWidget *parent = nullptr);
  void set_folder(const QString &abs);
  void refresh();

 private:
  QString folder_;
  QFileSystemWatcher watcher_;
};

// Lista dos vídeos mais recentes (nome, data e tamanho); clique abre o vídeo.
class RecentVideos : public QListWidget {
 public:
  explicit RecentVideos(QWidget *parent = nullptr);
  void set_folder(const QString &abs);
  void refresh();

 private:
  QString folder_;
  QFileSystemWatcher watcher_;
};

}  // namespace framux::gui
