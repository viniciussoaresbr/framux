#include "gui/main_window.h"

#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDesktopServices>
#include <QDir>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScreen>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <cstdlib>

#include "common/flatpak.h"
#include "common/video_rate.h"
#include "gui/theme.h"

namespace framux::gui {

namespace {

constexpr const char *kBus = "io.github.framux.Daemon1";
constexpr const char *kPath = "/io/github/framux/Daemon1";
constexpr int kPollMs = 1000;

// Cartão com título: a unidade visual de cada grupo de opções.
QFrame *card(const QString &title, QWidget *parent, QVBoxLayout **body) {
  auto *f = new QFrame(parent);
  f->setObjectName("card");
  auto *v = new QVBoxLayout(f);
  v->setContentsMargins(16, 12, 16, 14);
  v->setSpacing(8);
  auto *t = new QLabel(title, f);
  t->setObjectName("cardTitle");
  v->addWidget(t);
  *body = v;
  return f;
}

// Página com rolagem: quando um aviso aparece e falta altura, surge a barra
// em vez de o layout espremer caixas e botões abaixo do tamanho deles.
QWidget *scrollable(QWidget *page) {
  page->setObjectName("pageBody");
  auto *area = new QScrollArea;
  area->setObjectName("page");
  area->setWidget(page);
  area->setWidgetResizable(true);
  area->setFrameShape(QFrame::NoFrame);
  area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  area->viewport()->setAutoFillBackground(false);
  return area;
}

QLabel *hint(const QString &text, QWidget *parent, const char *name = "hint") {
  auto *l = new QLabel(text, parent);
  l->setObjectName(name);
  l->setWordWrap(true);
  return l;
}

QFormLayout *form() {
  auto *f = new QFormLayout;
  f->setContentsMargins(0, 0, 0, 0);
  f->setHorizontalSpacing(14);
  f->setVerticalSpacing(8);
  f->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  f->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
  return f;
}

QString api_label(const QString &api, const QString &engine) {
  QString s = api == "vulkan" ? "Vulkan" : api.startsWith("gl") ? "OpenGL" : api;
  const QString e = engine.toLower();
  if (e.contains("dxvk"))
    s += " (DXVK)";
  else if (e.contains("vkd3d"))
    s += " (VKD3D)";
  else if (e.contains("wined3d"))
    s += " (WineD3D)";
  return s;
}

QString check(bool ok, const QString &text) {
  return QString("<span style='color:%1'>●</span> %2").arg(ok ? "#3ec46d" : "#e5534b", text);
}

}  // namespace

MainWindow::MainWindow(ConfigStore &store) : store_(store) {
  setWindowTitle("Framux");
  setWindowIcon(app_icon());

  auto *central = new QWidget(this);
  auto *v = new QVBoxLayout(central);
  v->setContentsMargins(14, 10, 14, 14);
  v->setSpacing(10);

  tabs_ = new NavBar(central);
  tabs_->add("Geral", NavBar::Icon::General);
  tabs_->add("FPS", NavBar::Icon::Fps);
  tabs_->add("Gravações", NavBar::Icon::Recordings);
  tabs_->add("Capturas", NavBar::Icon::Screenshots);
  // Nome à esquerda e abas centralizadas na janela: as duas colunas laterais
  // têm a mesma largura.
  auto *top = new QGridLayout;
  top->setContentsMargins(0, 0, 0, 0);
  top->addWidget(new Brand(central), 0, 0, Qt::AlignLeft | Qt::AlignVCenter);
  top->addWidget(tabs_, 0, 1, Qt::AlignCenter);
  top->setColumnStretch(0, 1);
  top->setColumnStretch(2, 1);
  v->addLayout(top);

  auto *status = new QWidget(central);
  status->setObjectName("statusBar");
  status->setAttribute(Qt::WA_StyledBackground);
  auto *sh = new QHBoxLayout(status);
  sh->setContentsMargins(12, 8, 12, 8);
  status_dot_ = new QLabel("●", status);
  status_text_ = new QLabel(status);
  status_text_->setTextFormat(Qt::RichText);
  sh->addWidget(status_dot_);
  sh->addWidget(status_text_, 1);
  v->addWidget(status);

  pages_ = new QStackedWidget(central);
  for (QWidget *page : {build_general(), build_fps(), build_recordings(), build_screenshots()})
    pages_->addWidget(scrollable(page));
  v->addWidget(pages_, 1);
  tabs_->on_change = [this](int i) { pages_->setCurrentIndex(i); };

  setCentralWidget(central);
  resize(800, 700);

  build_tray();
  load_from_config();
  store_.on_external_change([this] { load_from_config(); });

  set_status("#888", "Procurando o Framux…");
  connect(&poll_timer_, &QTimer::timeout, this, [this] { poll_status(); });
  poll_timer_.start(kPollMs);
  poll_status();
}

void MainWindow::select_tab(int index) { tabs_->set_current(index); }

// ---------------------------------------------------------------------------
// Geral: opções à esquerda, ambiente à direita.

QWidget *MainWindow::build_general() {
  auto *page = new QWidget;
  auto *grid = new QHBoxLayout(page);
  grid->setContentsMargins(0, 0, 0, 0);
  grid->setSpacing(12);

  auto *left = new QVBoxLayout;
  left->setSpacing(12);

  QVBoxLayout *b;
  QFrame *act = card("Ativação", page, &b);
  auto_activate_ = new QCheckBox("Ativar automaticamente nos jogos (Steam, Proton e Wine — Vulkan)", act);
  auto_activate_->setObjectName("autoActivate");
  auto_hint_ = hint("", act, "warn");
  auto_hint_->hide();
  b->addWidget(auto_activate_);
  b->addWidget(auto_hint_);
  if (flatpak::steam_installed()) {
    flatpak_ = new QCheckBox("Integrar ao Steam Flatpak (camada e daemon visíveis no sandbox)", act);
    flatpak_->setObjectName("flatpakSteam");
    flatpak_hint_ = hint("", act, "warn");
    flatpak_hint_->hide();
    b->addWidget(flatpak_);
    b->addWidget(flatpak_hint_);
  }
  b->addWidget(hint("Jogos OpenGL nativos e outros fora da Steam: use nas opções de inicialização", act));
  auto *cmd_row = new QHBoxLayout;
  run_cmd_ = new QLabel(act);
  run_cmd_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto *copy = new QPushButton("Copiar", act);
  cmd_row->addWidget(run_cmd_, 1);
  cmd_row->addWidget(copy);
  b->addLayout(cmd_row);
  left->addWidget(act);
  connect(copy, &QPushButton::clicked, this, [this, copy] {
    QGuiApplication::clipboard()->setText(launch_command());
    copy->setText("Copiado");
    QTimer::singleShot(1500, copy, [copy] { copy->setText("Copiar"); });
  });
  connect(auto_activate_, &QCheckBox::toggled, this, [this](bool on) {
    if (loading_) return;
    QString err;
    std::string ferr;
    if (!set_auto_activation(on, &err)) {
      QMessageBox::warning(this, "Framux", "Não consegui alterar a ativação automática:\n" + err);
      loading_ = true;
      auto_activate_->setChecked(!on);
      loading_ = false;
    } else if (!flatpak::set_auto(on, &ferr)) {
      QMessageBox::warning(this, "Framux", "Não consegui alterar o Steam Flatpak:\n" + QString::fromStdString(ferr));
    }
    update_environment();
  });
  if (flatpak_) {
    connect(flatpak_, &QCheckBox::toggled, this, [this](bool on) {
      if (loading_) return;
      std::string err;
      const bool ok = on ? flatpak::enable(auto_activation_enabled(), &err) : flatpak::disable(&err);
      if (!ok) {
        QMessageBox::warning(this, "Framux",
                             "Não consegui alterar a integração com o Steam Flatpak:\n" + QString::fromStdString(err));
        loading_ = true;
        flatpak_->setChecked(flatpak::status().enabled);
        loading_ = false;
      }
      flatpak_restart_ = true;
      update_environment();
    });
  }

  QFrame *start = card("Inicialização", page, &b);
  open_on_login_ = new QCheckBox("Abrir o Framux ao entrar na sessão", start);
  start_minimized_ = new QCheckBox("Iniciar minimizado", start);
  tray_check_ = new QCheckBox("Ícone na bandeja (fechar a janela mantém o Framux rodando)", start);
  tray_hint_ = hint("", start, "warn");
  tray_hint_->hide();
  b->addWidget(open_on_login_);
  b->addWidget(start_minimized_);
  b->addWidget(tray_check_);
  b->addWidget(tray_hint_);
  b->addStretch(1);
  // Sem a seção Aparência, Inicialização ocupa o resto da coluna.
  left->addWidget(start, 1);
  connect(open_on_login_, &QCheckBox::toggled, this, [this](bool on) {
    if (loading_) return;
    QString err;
    if (!set_open_on_login(on, &err)) QMessageBox::warning(this, "Framux", err);
    store_.edit([on](Config &c) { c.general.start_on_login = on; });
  });
  connect(start_minimized_, &QCheckBox::toggled, this, [this](bool on) {
    if (!loading_) store_.edit([on](Config &c) { c.general.start_minimized = on; });
  });
  connect(tray_check_, &QCheckBox::toggled, this, [this](bool on) {
    if (loading_) return;
    store_.edit([on](Config &c) { c.general.tray = on; });
    update_tray();
  });

  // Coluna da direita: ambiente.
  QFrame *env = card("Ambiente", page, &b);
  env->setMinimumWidth(220);
  env->setMaximumWidth(280);
  env_session_ = new QLabel(env);
  env_layer64_ = new QLabel(env);
  env_layer32_ = new QLabel(env);
  env_hotkeys_ = new QLabel(env);
  env_hotkeys_->setWordWrap(true);
  env_daemon_ = new QLabel(env);
  env_auto_ = new QLabel(env);
  env_auto_->setWordWrap(true);
  env_flatpak_ = new QLabel(env);
  for (QLabel *l : {env_session_, env_layer64_, env_layer32_, env_hotkeys_, env_daemon_, env_auto_, env_flatpak_}) {
    l->setTextFormat(Qt::RichText);
    b->addWidget(l);
  }
  b->addStretch();
  auto *stop = new QPushButton("Parar o Framux", env);
  stop->setToolTip("Encerra o monitoramento, os atalhos e esta janela até o Framux ser aberto de novo");
  b->addWidget(stop);
  connect(stop, &QPushButton::clicked, this, [this] {
    if (!confirm(this, "Parar o Framux?", "Contador, atalhos e screenshots param até você abrir o Framux de novo."))
      return;
    store_.flush();
    stop_daemon();
    qApp->quit();
  });

  grid->addLayout(left, 1);
  grid->addWidget(env);
  return page;
}

// ---------------------------------------------------------------------------
// FPS: prévia clicável do overlay + benchmark.

QWidget *MainWindow::build_fps() {
  auto *page = new QWidget;
  auto *v = new QVBoxLayout(page);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(12);

  QVBoxLayout *b;
  QFrame *ov = card("Overlay", page, &b);
  auto *row = new QHBoxLayout;
  row->setSpacing(18);
  auto *left = new QVBoxLayout;
  preview_ = new CornerPreview(ov);
  preview_->setObjectName("cornerPreview");
  left->addWidget(preview_);
  auto *corner_hint = hint("Clique em um canto da prévia para posicionar o contador", ov);
  corner_hint->setWordWrap(false);
  left->addWidget(corner_hint);
  row->addLayout(left, 3);

  auto *f = form();
  overlay_key_ = new HotkeyButton(ov);
  overlay_key_->setObjectName("overlayKey");
  f->addRow("Mostrar/ocultar", hotkey_row(overlay_key_, ov));
  overlay_corner_key_ = new HotkeyButton(ov);
  overlay_corner_key_->setObjectName("overlayCornerKey");
  overlay_corner_key_->setToolTip(
      "Leva o contador ao próximo canto: superior esquerdo → superior direito → "
      "inferior direito → inferior esquerdo");
  f->addRow("Alternar canto", hotkey_row(overlay_corner_key_, ov));
  color_ = new ColorButton(ov);
  f->addRow("Cor do contador", color_);
  frametime_ = new QCheckBox("Mostrar frame time (ms)", ov);
  f->addRow("", frametime_);
  auto *upd = new QWidget(ov);
  auto *ul = new QVBoxLayout(upd);
  ul->setContentsMargins(0, 0, 0, 0);
  update_smooth_ = new QRadioButton("Suave (janela de 1 s)", upd);
  update_once_ = new QRadioButton("Uma vez por segundo", upd);
  ul->addWidget(update_smooth_);
  ul->addWidget(update_once_);
  f->addRow("Atualizar", upd);
  row->addLayout(f, 2);
  b->addLayout(row);
  v->addWidget(ov);

  preview_->on_corner = [this](ipc::Corner c) { store_.edit([c](Config &cfg) { cfg.overlay.corner = c; }); };
  overlay_key_->on_change = [this](const std::string &h) {
    const QString err = check_hotkey(h, &Config::Hotkeys::overlay);
    if (err.isEmpty()) store_.edit([h](Config &c) { c.hotkeys.overlay = h; });
    return err;
  };
  overlay_corner_key_->on_change = [this](const std::string &h) {
    const QString err = check_hotkey(h, &Config::Hotkeys::overlay_corner);
    if (err.isEmpty()) store_.edit([h](Config &c) { c.hotkeys.overlay_corner = h; });
    return err;
  };
  color_->on_change = [this](Rgb c) {
    store_.edit([c](Config &cfg) { cfg.overlay.color_normal = c; });
    preview_->set_state(store_.get().overlay.corner, c, store_.get().overlay.show_frametime);
  };
  connect(frametime_, &QCheckBox::toggled, this, [this](bool on) {
    if (loading_) return;
    store_.edit([on](Config &c) { c.overlay.show_frametime = on; });
    preview_->set_state(store_.get().overlay.corner, store_.get().overlay.color_normal, on);
  });
  connect(update_once_, &QRadioButton::toggled, this, [this](bool on) {
    if (!loading_) store_.edit([on](Config &c) { c.overlay.update = on ? FpsMode::OncePerSecond : FpsMode::Smooth; });
  });

  QFrame *bench = card("Benchmark", page, &b);
  f = form();
  bench_key_ = new HotkeyButton(bench);
  bench_key_->setObjectName("benchKey");
  f->addRow("Iniciar/parar", hotkey_row(bench_key_, bench));
  auto *stop_row = new QWidget(bench);
  auto *sl = new QHBoxLayout(stop_row);
  sl->setContentsMargins(0, 0, 0, 0);
  bench_stop_ = new QCheckBox("Parar após", stop_row);
  bench_stop_s_ = new QSpinBox(stop_row);
  bench_stop_s_->setRange(1, 3600);
  bench_stop_s_->setSuffix(" s");
  sl->addWidget(bench_stop_);
  sl->addWidget(bench_stop_s_);
  sl->addStretch();
  f->addRow("Duração", stop_row);
  auto *logs = new QWidget(bench);
  auto *lg = new QGridLayout(logs);
  lg->setContentsMargins(0, 0, 0, 0);
  lg->setHorizontalSpacing(18);
  log_frametimes_ = new QCheckBox("Frame times", logs);
  log_fps_ = new QCheckBox("FPS por segundo", logs);
  log_summary_ = new QCheckBox("Resumo (min/máx/média, 1% low)", logs);
  lg->addWidget(log_frametimes_, 0, 0);
  lg->addWidget(log_fps_, 0, 1);
  lg->addWidget(log_summary_, 1, 0, 1, 2);
  lg->setColumnStretch(2, 1);
  f->addRow("Salvar", logs);
  bench_folder_ = new FolderRow(bench);
  f->addRow("Pasta", bench_folder_);
  b->addLayout(f);
  v->addWidget(bench);
  v->addStretch();

  bench_key_->on_change = [this](const std::string &h) {
    const QString err = check_hotkey(h, &Config::Hotkeys::benchmark);
    if (err.isEmpty()) store_.edit([h](Config &c) { c.hotkeys.benchmark = h; });
    return err;
  };
  auto save_stop = [this] {
    if (loading_) return;
    const uint32_t s = bench_stop_->isChecked() ? uint32_t(bench_stop_s_->value()) : 0;
    bench_stop_s_->setEnabled(bench_stop_->isChecked());
    store_.edit([s](Config &c) { c.benchmark.stop_after_s = s; });
  };
  connect(bench_stop_, &QCheckBox::toggled, this, save_stop);
  connect(bench_stop_s_, &QSpinBox::valueChanged, this, save_stop);
  auto bind_log = [this](QCheckBox *box, bool Config::Benchmark::*field) {
    connect(box, &QCheckBox::toggled, this, [this, field](bool on) {
      if (!loading_) store_.edit([field, on](Config &c) { c.benchmark.*field = on; });
    });
  };
  bind_log(log_frametimes_, &Config::Benchmark::log_frametimes);
  bind_log(log_fps_, &Config::Benchmark::log_fps);
  bind_log(log_summary_, &Config::Benchmark::log_summary);
  bench_folder_->on_change = [this](const std::string &d) { store_.edit([d](Config &c) { c.benchmark.folder = d; }); };
  return page;
}

// ---------------------------------------------------------------------------
// Gravações (aba Movies do FRAPS): vídeo, áudio e recentes.

namespace {

// Valores de [video] encoder, na ordem do combo, e o nome do daemon para
// cada um (StatusJson "encoders"). cpu: disputa os núcleos com o jogo.
struct EncoderChoice {
  const char *value;
  const char *label;
  bool cpu;
};
constexpr EncoderChoice kEncoders[] = {
    {"auto", "Automático", false},
    {"av1_hw", "AV1 (placa de vídeo)", false},
    {"av1_sw", "AV1 (processador, SVT-AV1)", true},
    {"h264_hw", "H.264 (placa de vídeo)", false},
    {"x264", "H.264 (processador, x264)", true},
    {"utvideo", "Ut Video sem perda (processador)", true},
};
constexpr const char *kQualities[][2] = {{"low", "Baixa (arquivos menores)"},
                                         {"medium", "Média"},
                                         {"high", "Alta"},
                                         {"lossless", "Sem perda (para edição, arquivos enormes)"}};

}  // namespace

QWidget *MainWindow::build_recordings() {
  auto *page = new QWidget;
  auto *v = new QVBoxLayout(page);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(12);

  QVBoxLayout *b;
  QFrame *opts = card("Gravação", page, &b);
  auto *f = form();
  video_key_ = new HotkeyButton(opts);
  video_key_->setObjectName("videoKey");
  video_key_->setToolTip("Pressione dentro do jogo para começar a gravar; pressione de novo para parar");
  f->addRow("Iniciar/parar", hotkey_row(video_key_, opts));

  auto *fps = new QWidget(opts);
  auto *fl = new QHBoxLayout(fps);
  fl->setContentsMargins(0, 0, 0, 0);
  fl->setSpacing(14);
  video_fps_ = new QButtonGroup(this);
  for (int n : {30, 60}) {
    auto *rb = new QRadioButton(QString::number(n), fps);
    rb->setObjectName(QString("videoFps%1").arg(n));
    video_fps_->addButton(rb, n);
    fl->addWidget(rb);
  }
  auto *custom = new QRadioButton("Outro", fps);
  custom->setObjectName("videoFpsCustom");
  video_fps_->addButton(custom, 0);
  fl->addWidget(custom);
  video_fps_custom_ = new QSpinBox(fps);
  video_fps_custom_->setObjectName("videoFpsValue");
  video_fps_custom_->setRange(1, 240);
  video_fps_custom_->setSuffix(" fps");
  fl->addWidget(video_fps_custom_);
  fl->addStretch();
  f->addRow("FPS do vídeo", fps);

  auto *size = new QWidget(opts);
  auto *zl = new QHBoxLayout(size);
  zl->setContentsMargins(0, 0, 0, 0);
  zl->setSpacing(14);
  video_full_ = new QRadioButton("Tamanho cheio", size);
  video_half_ = new QRadioButton("Metade", size);
  video_half_->setToolTip("Metade da largura e da altura: arquivos ~4× menores e menos carga na gravação");
  auto *size_group = new QButtonGroup(this);
  size_group->addButton(video_full_);
  size_group->addButton(video_half_);
  zl->addWidget(video_full_);
  zl->addWidget(video_half_);
  zl->addStretch();
  f->addRow("Resolução", size);

  auto *enc = new QWidget(opts);
  auto *el = new QVBoxLayout(enc);
  el->setContentsMargins(0, 0, 0, 0);
  el->setSpacing(4);
  video_encoder_ = new QComboBox(enc);
  video_encoder_->setObjectName("videoEncoder");
  video_encoder_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  video_encoder_hint_ = hint("", enc);
  video_encoder_hint_->setObjectName("videoEncoderHint");
  video_encoder_hint_->setWordWrap(false);  // uma linha, como a estimativa de tamanho
  video_encoder_hint_->hide();
  fill_encoders();
  el->addWidget(video_encoder_, 0, Qt::AlignLeft);
  el->addWidget(video_encoder_hint_);
  f->addRow("Codificador", enc);

  video_quality_ = new QComboBox(opts);
  video_quality_->setObjectName("videoQuality");
  for (const auto &q : kQualities) video_quality_->addItem(q[1], QString(q[0]));
  video_quality_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  auto *qual = new QWidget(opts);
  auto *ql = new QVBoxLayout(qual);
  ql->setContentsMargins(0, 0, 0, 0);
  ql->setSpacing(4);
  ql->addWidget(video_quality_, 0, Qt::AlignLeft);
  video_size_hint_ = hint("", qual);
  video_size_hint_->setObjectName("videoSizeHint");
  video_size_hint_->setWordWrap(false);  // uma linha; com quebra o formulário corta o texto
  ql->addWidget(video_size_hint_);
  f->addRow("Qualidade", qual);

  video_mp4_ = new QCheckBox("Converter para MP4 ao terminar", opts);
  video_mp4_->setObjectName("videoMp4");
  video_mp4_->setToolTip(
      "A gravação é sempre feita em MKV, que sobrevive se o jogo travar. Ao parar, o arquivo é "
      "convertido para MP4 (sem recodificar), mais fácil de compartilhar e editar.");
  f->addRow("Arquivo", video_mp4_);
  video_folder_ = new FolderRow(opts);
  f->addRow("Pasta", video_folder_);
  b->addLayout(f);
  v->addWidget(opts);

  QFrame *audio = card("Áudio", page, &b);
  audio_game_ = new QCheckBox("Gravar o som do jogo", audio);
  audio_game_->setObjectName("audioGame");
  b->addWidget(audio_game_);
  b->addWidget(
      hint("Só o áudio do processo do jogo, pelo PipeWire: músicas, chamadas e avisos de outros "
           "programas ficam de fora.",
           audio));
  v->addWidget(audio);

  QFrame *rec = card("Recentes", page, &b);
  recent_videos_ = new RecentVideos(rec);
  b->addWidget(recent_videos_);
  v->addWidget(rec);
  v->addStretch();

  video_key_->on_change = [this](const std::string &h) {
    const QString err = check_hotkey(h, &Config::Hotkeys::video);
    if (err.isEmpty()) store_.edit([h](Config &c) { c.hotkeys.video = h; });
    return err;
  };
  auto save_fps = [this] {
    const int id = video_fps_->checkedId();
    video_fps_custom_->setEnabled(id == 0);
    if (loading_ || id < 0) return;
    const int n = id == 0 ? video_fps_custom_->value() : id;
    store_.edit([n](Config &c) { c.video.fps = n; });
    update_encoder_hint();
  };
  connect(video_fps_, &QButtonGroup::idToggled, this, [save_fps](int, bool on) {
    if (on) save_fps();
  });
  connect(video_fps_custom_, &QSpinBox::valueChanged, this, save_fps);
  connect(video_half_, &QRadioButton::toggled, this, [this](bool on) {
    update_encoder_hint();
    if (!loading_) store_.edit([on](Config &c) { c.video.scale = on ? "half" : "full"; });
  });
  connect(video_encoder_, &QComboBox::currentIndexChanged, this, [this] {
    update_encoder_hint();
    if (loading_) return;
    const std::string e = video_encoder_->currentData().toString().toStdString();
    store_.edit([e](Config &c) { c.video.encoder = e; });
  });
  connect(video_quality_, &QComboBox::currentIndexChanged, this, [this] {
    update_encoder_hint();
    if (loading_) return;
    const std::string q = video_quality_->currentData().toString().toStdString();
    store_.edit([q](Config &c) { c.video.quality = q; });
  });
  connect(video_mp4_, &QCheckBox::toggled, this, [this](bool on) {
    if (!loading_) store_.edit([on](Config &c) { c.video.container = on ? "mp4" : "mkv"; });
  });
  video_folder_->on_change = [this](const std::string &d) {
    store_.edit([d](Config &c) { c.video.folder = d; });
    recent_videos_->set_folder(expanded_folder(d));
  };
  connect(audio_game_, &QCheckBox::toggled, this, [this](bool on) {
    if (!loading_) store_.edit([on](Config &c) { c.audio.game = on; });
  });
  return page;
}

void MainWindow::fill_encoders() {
  // Só "Automático" e o que o daemon encontrou; sem o daemon, todos. Um
  // encoder do config que sumiu aparece como automático, sem regravar o
  // config: a gravação cai no automático do mesmo jeito.
  const bool was_loading = loading_;
  loading_ = true;
  video_encoder_->clear();
  for (const auto &e : kEncoders)
    if (encoders_.isEmpty() || e.value == QLatin1String("auto") || encoders_.contains(e.value))
      video_encoder_->addItem(e.label, QString(e.value));
  video_encoder_->setCurrentIndex(
      std::max(0, video_encoder_->findData(QString::fromStdString(store_.get().video.encoder))));
  loading_ = was_loading;
}

void MainWindow::update_encoder_hint() {
  const QString quality = video_quality_->currentData().toString();
  const bool lossless = quality == "lossless";
  video_encoder_->setEnabled(!lossless);
  QString used = lossless ? "utvideo" : video_encoder_->currentData().toString();
  QString text;
  if (used == "auto" && !encoders_.isEmpty()) {
    used = encoders_.first();
    text = "Vai usar: " + video_encoder_->itemText(video_encoder_->findData(used)) + ".";
  }
  // Uma linha só: com quebra o formulário corta o texto. O detalhe vai na dica.
  bool cpu = false;
  for (const auto &e : kEncoders) cpu |= e.cpu && used == e.value;
  if (cpu)
    text = QString("<span style='color:#e8912d'>%1 no processador: pode derrubar o FPS do jogo.</span>")
               .arg(lossless ? "Sem perda grava em Ut Video," : "Codifica");
  video_encoder_hint_->setText(text);
  video_encoder_hint_->setToolTip(encoders_missing_);
  video_encoder_hint_->setVisible(!text.isEmpty());

  // Estimativa pela tela principal (o jogo costuma estar em tela cheia): o
  // teto de taxa do gravador, em MB por minuto.
  QSize px(1920, 1080);
  if (const QScreen *sc = QGuiApplication::primaryScreen()) px = sc->size() * sc->devicePixelRatio();
  if (video_half_->isChecked()) px /= 2;
  const int fps = video_fps_->checkedId() > 0 ? video_fps_->checkedId() : video_fps_custom_->value();
  const QString where = QString("%1×%2 a %3 fps").arg(px.width()).arg(px.height()).arg(fps);
  QString size;
  if (lossless || used == "utvideo") {
    // Ut Video fica em ~0,6 dos bits do RGB cru em cenas de jogo.
    const double gb = double(px.width()) * px.height() * 24 * fps * 0.6 / 8 * 60 / 1e9;
    size = QString("~%1 GB por minuto em %2").arg(gb, 0, 'f', 1).arg(where);
  } else {
    // Sem o daemon (used = "auto"), conta como H.264: a estimativa maior.
    const uint64_t cap = video_max_bitrate(used.startsWith("av1"), quality.toStdString(), px.width(), px.height(), fps);
    size = QString("Até ~%1 MB por minuto em %2 (teto de %3 Mbit/s)")
               .arg(qRound(double(cap) / 8 * 60 / 1e6))
               .arg(where)
               .arg(qRound(double(cap) / 1e6));
  }
  video_size_hint_->setText(size);
}

// ---------------------------------------------------------------------------
// Screenshots: formulário + recentes.

QWidget *MainWindow::build_screenshots() {
  auto *page = new QWidget;
  auto *v = new QVBoxLayout(page);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(12);

  QVBoxLayout *b;
  QFrame *opts = card("Capturas de Tela", page, &b);
  auto *f = form();
  shot_key_ = new HotkeyButton(opts);
  shot_key_->setObjectName("shotKey");
  f->addRow("Capturar", hotkey_row(shot_key_, opts));

  auto *fmt = new QWidget(opts);
  auto *fl = new QHBoxLayout(fmt);
  fl->setContentsMargins(0, 0, 0, 0);
  fl->setSpacing(14);
  format_ = new QButtonGroup(this);
  const std::pair<const char *, ImageFormat> formats[] = {
      {"PNG", ImageFormat::Png}, {"JPG", ImageFormat::Jpeg}, {"BMP", ImageFormat::Bmp}, {"TGA", ImageFormat::Tga}};
  for (const auto &[name, value] : formats) {
    auto *rb = new QRadioButton(name, fmt);
    rb->setObjectName(QString("format") + name);
    format_->addButton(rb, int(value));
    fl->addWidget(rb);
  }
  fl->addStretch();
  f->addRow("Formato", fmt);

  auto *q = new QWidget(opts);
  auto *ql = new QHBoxLayout(q);
  ql->setContentsMargins(0, 0, 0, 0);
  jpeg_quality_ = new QSlider(Qt::Horizontal, q);
  jpeg_quality_->setRange(10, 100);
  jpeg_quality_->setMaximumWidth(260);
  jpeg_quality_value_ = new QLabel(q);
  jpeg_quality_value_->setMinimumWidth(30);
  ql->addWidget(jpeg_quality_);
  ql->addWidget(jpeg_quality_value_);
  ql->addStretch();
  f->addRow("Qualidade JPG", q);
  shot_form_ = f;

  shot_overlay_ = new QCheckBox("Incluir o contador de FPS na imagem", opts);
  f->addRow("", shot_overlay_);
  auto *rep = new QWidget(opts);
  auto *rl = new QHBoxLayout(rep);
  rl->setContentsMargins(0, 0, 0, 0);
  shot_repeat_ = new QCheckBox("Repetir a cada", rep);
  shot_repeat_s_ = new QSpinBox(rep);
  shot_repeat_s_->setRange(1, 3600);
  shot_repeat_s_->setSuffix(" s");
  rl->addWidget(shot_repeat_);
  rl->addWidget(shot_repeat_s_);
  rl->addWidget(hint("(o atalho liga/desliga)", rep));
  rl->addStretch();
  f->addRow("Série", rep);
  shot_folder_ = new FolderRow(opts);
  f->addRow("Pasta", shot_folder_);
  b->addLayout(f);
  v->addWidget(opts);

  QFrame *rec = card("Recentes", page, &b);
  recent_ = new RecentShots(rec);
  b->addWidget(recent_);
  v->addWidget(rec);
  v->addStretch();

  shot_key_->on_change = [this](const std::string &h) {
    const QString err = check_hotkey(h, &Config::Hotkeys::screenshot);
    if (err.isEmpty()) store_.edit([h](Config &c) { c.hotkeys.screenshot = h; });
    return err;
  };
  connect(format_, &QButtonGroup::idToggled, this, [this](int id, bool on) {
    if (!on) return;
    shot_form_->setRowVisible(jpeg_quality_->parentWidget(), id == int(ImageFormat::Jpeg));
    if (!loading_) store_.edit([id](Config &c) { c.screenshot.format = ImageFormat(id); });
  });
  connect(jpeg_quality_, &QSlider::valueChanged, this, [this](int val) {
    jpeg_quality_value_->setText(QString::number(val));
    if (!loading_) store_.edit([val](Config &c) { c.screenshot.jpeg_quality = val; });
  });
  connect(shot_overlay_, &QCheckBox::toggled, this, [this](bool on) {
    if (!loading_) store_.edit([on](Config &c) { c.screenshot.include_overlay = on; });
  });
  auto save_repeat = [this] {
    if (loading_) return;
    const double s = shot_repeat_->isChecked() ? shot_repeat_s_->value() : 0;
    shot_repeat_s_->setEnabled(shot_repeat_->isChecked());
    store_.edit([s](Config &c) { c.screenshot.repeat_every_s = s; });
  };
  connect(shot_repeat_, &QCheckBox::toggled, this, save_repeat);
  connect(shot_repeat_s_, &QSpinBox::valueChanged, this, save_repeat);
  shot_folder_->on_change = [this](const std::string &d) {
    store_.edit([d](Config &c) { c.screenshot.folder = d; });
    recent_->set_folder(expanded_folder(d));
  };
  return page;
}

// ---------------------------------------------------------------------------

void MainWindow::load_from_config() {
  loading_ = true;
  const Config &c = store_.get();

  auto_activate_->setChecked(auto_activation_enabled());
  open_on_login_->setChecked(c.general.start_on_login);
  start_minimized_->setChecked(c.general.start_minimized);
  tray_check_->setChecked(c.general.tray);

  preview_->set_state(c.overlay.corner == ipc::Corner::Hidden ? ipc::Corner::TopLeft : c.overlay.corner,
                      c.overlay.color_normal, c.overlay.show_frametime);
  overlay_key_->set_hotkey(c.hotkeys.overlay);
  overlay_corner_key_->set_hotkey(c.hotkeys.overlay_corner);
  color_->set_color(c.overlay.color_normal);
  frametime_->setChecked(c.overlay.show_frametime);
  (c.overlay.update == FpsMode::OncePerSecond ? update_once_ : update_smooth_)->setChecked(true);
  bench_key_->set_hotkey(c.hotkeys.benchmark);
  bench_stop_->setChecked(c.benchmark.stop_after_s > 0);
  bench_stop_s_->setValue(c.benchmark.stop_after_s > 0 ? int(c.benchmark.stop_after_s) : 60);
  bench_stop_s_->setEnabled(c.benchmark.stop_after_s > 0);
  log_frametimes_->setChecked(c.benchmark.log_frametimes);
  log_fps_->setChecked(c.benchmark.log_fps);
  log_summary_->setChecked(c.benchmark.log_summary);
  bench_folder_->set_folder(c.benchmark.folder);

  video_key_->set_hotkey(c.hotkeys.video);
  const int fps_id = c.video.fps == 30 || c.video.fps == 60 ? c.video.fps : 0;
  video_fps_->button(fps_id)->setChecked(true);
  video_fps_custom_->setValue(c.video.fps);
  video_fps_custom_->setEnabled(fps_id == 0);
  (c.video.scale == "half" ? video_half_ : video_full_)->setChecked(true);
  fill_encoders();
  video_quality_->setCurrentIndex(std::max(0, video_quality_->findData(QString::fromStdString(c.video.quality))));
  video_mp4_->setChecked(c.video.container == "mp4");
  video_folder_->set_folder(c.video.folder);
  recent_videos_->set_folder(expanded_folder(c.video.folder));
  audio_game_->setChecked(c.audio.game);
  update_encoder_hint();

  shot_key_->set_hotkey(c.hotkeys.screenshot);
  if (QAbstractButton *rb = format_->button(int(c.screenshot.format))) rb->setChecked(true);
  jpeg_quality_->setValue(c.screenshot.jpeg_quality);
  jpeg_quality_value_->setText(QString::number(c.screenshot.jpeg_quality));
  shot_form_->setRowVisible(jpeg_quality_->parentWidget(), c.screenshot.format == ImageFormat::Jpeg);
  shot_overlay_->setChecked(c.screenshot.include_overlay);
  shot_repeat_->setChecked(c.screenshot.repeat_every_s > 0);
  shot_repeat_s_->setValue(c.screenshot.repeat_every_s > 0 ? int(c.screenshot.repeat_every_s) : 10);
  shot_repeat_s_->setEnabled(c.screenshot.repeat_every_s > 0);
  shot_folder_->set_folder(c.screenshot.folder);
  recent_->set_folder(expanded_folder(c.screenshot.folder));

  loading_ = false;
  update_environment();
  update_tray();
}

QString MainWindow::check_hotkey(const std::string &h, const std::string Config::Hotkeys::*self) const {
  if (h.empty()) return {};
  const auto &hk = store_.get().hotkeys;
  const std::pair<const std::string Config::Hotkeys::*, const char *> others[] = {
      {&Config::Hotkeys::benchmark, "benchmark"},
      {&Config::Hotkeys::overlay, "overlay"},
      {&Config::Hotkeys::overlay_corner, "canto do overlay"},
      {&Config::Hotkeys::screenshot, "screenshot"},
      {&Config::Hotkeys::video, "atalho de gravação"},
  };
  for (const auto &[field, name] : others)
    if (field != self && hk.*field == h) return QString("Já usado no %1").arg(name);
  return {};
}

// ---------------------------------------------------------------------------
// Status do jogo ativo (D-Bus, sem acordar o daemon se ele estiver parado).

void MainWindow::poll_status() {
  QDBusConnection bus = QDBusConnection::sessionBus();
  const bool up = bus.isConnected() && bus.interface()->isServiceRegistered(kBus);
  if (up != daemon_up_) {
    daemon_up_ = up;
    update_environment();
  }
  if (!up) {
    set_status("#e5534b", "Framux parado: o monitoramento volta quando um jogo for aberto com ele ativo");
    return;
  }
  auto msg = QDBusMessage::createMethodCall(kBus, kPath, kBus, "StatusJson");
  auto *w = new QDBusPendingCallWatcher(bus.asyncCall(msg, 800), this);
  connect(w, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
    QDBusPendingReply<QString> r = *w;
    if (!r.isError()) apply_status(r.value().toUtf8());
    w->deleteLater();
  });
}

void MainWindow::apply_status(const QByteArray &json) {
  const QJsonObject o = QJsonDocument::fromJson(json).object();
  QStringList encoders;
  for (const QJsonValue &e : o.value("encoders").toArray()) encoders << e.toString();
  const QString missing = o.value("encoders_missing").toString();
  if (encoders != encoders_ || missing != encoders_missing_) {
    encoders_ = encoders;
    encoders_missing_ = missing;
    fill_encoders();
    update_encoder_hint();
  }
  const QString backend = o.value("hotkey_backend").toString();
  if (backend != hotkey_backend_) {
    hotkey_backend_ = backend;
    update_environment();
  }
  QJsonObject active;
  int count = 0;
  for (const QJsonValue &s : o.value("sessions").toArray()) {
    ++count;
    if (s.toObject().value("active").toBool() || active.isEmpty()) active = s.toObject();
  }
  if (active.isEmpty()) {
    set_status("#8a8b91", "Nenhum jogo em execução");
    if (tray_) {
      tray_->setToolTip("Framux: nenhum jogo em execução");
      tray_->setIcon(app_icon());
    }
    return;
  }
  QString text = QString("<b>%1</b> — %2 fps · %3")
                     .arg(active.value("game").toString().toHtmlEscaped())
                     .arg(active.value("fps").toInt())
                     .arg(api_label(active.value("api").toString(), active.value("engine").toString()));
  const QString gpu = active.value("gpu").toString();
  if (!gpu.isEmpty()) text += " · " + gpu.toHtmlEscaped();
  const bool bench = active.value("benchmark").toBool();
  if (bench) text += " · <span style='color:#30d060'>benchmark em andamento</span>";
  const bool recording = active.value("recording").toBool();
  if (recording) {
    const int secs = active.value("recording_s").toInt();
    text += QString(" · <span style='color:#ff5050'>● gravando %1:%2</span>")
                .arg(secs / 60, 2, 10, QChar('0'))
                .arg(secs % 60, 2, 10, QChar('0'));
  }
  if (count > 1) text += QString(" · +%1").arg(count - 1);
  set_status("#3ec46d", text);
  if (tray_) {
    tray_->setToolTip(QString("Framux: %1 — %2 fps%3")
                          .arg(active.value("game").toString())
                          .arg(active.value("fps").toInt())
                          .arg(recording ? " (gravando)" : ""));
    tray_->setIcon(app_icon(recording ? 2 : bench ? 1 : 0));
  }
}

void MainWindow::set_status(const QString &dot_color, const QString &text) {
  status_dot_->setStyleSheet(QString("color:%1;").arg(dot_color));
  status_text_->setText(text);
}

void MainWindow::update_environment() {
  const QString type = qEnvironmentVariable("XDG_SESSION_TYPE");
  const QString desk = qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':').value(0);
  env_session_->setText(QString("Sessão: <b>%1</b>%2")
                            .arg(type == "wayland" ? "Wayland"
                                 : type == "x11"   ? "X11"
                                                   : type,
                                 desk.isEmpty() ? QString() : " · " + desk.toHtmlEscaped()));
  const LayerInstall li = installed_layers();
  env_layer64_->setText(check(li.x86_64, "Camada Vulkan 64 bits"));
  env_layer32_->setText(check(li.i686, "Camada Vulkan 32 bits"));
  QString hk = "—";
  if (hotkey_backend_ == "x11")
    hk = "globais (X11)";
  else if (hotkey_backend_ == "ingame")
    hk = "lidas dentro do jogo";
  else if (!hotkey_backend_.isEmpty())
    hk = hotkey_backend_;
  env_hotkeys_->setText("Atalhos: " + hk);
  env_daemon_->setText(check(daemon_up_, daemon_up_ ? "Daemon ativo" : "Daemon parado"));

  const bool on = auto_activation_enabled(), relogin = auto_activation_needs_relogin();
  env_auto_->setText(!on       ? "Modo automático: desligado"
                     : relogin ? "Modo automático: <span style='color:#e8912d'>vale após novo login</span>"
                               : check(true, "Modo automático ativo"));
  auto_hint_->setText("Saia e entre de novo na sessão para valer nos jogos abertos pela Steam.");
  auto_hint_->setVisible(relogin);

  const flatpak::Status fp = flatpak::status();
  if (flatpak_) {
    loading_ = true;
    flatpak_->setChecked(fp.enabled);
    loading_ = false;
    QString warn;
    if (fp.enabled && !fp.layer32)
      warn =
          "Sem a camada de 32 bits: jogos 32 bits (ex.: Left 4 Dead 2) ficam sem contador. Instale "
          "framux-layer.i686.";
    if (flatpak_restart_) warn += QString(warn.isEmpty() ? "" : " ") + "Feche e abra o Steam para aplicar.";
    flatpak_hint_->setText(warn);
    flatpak_hint_->setVisible(!warn.isEmpty());
  }
  env_flatpak_->setVisible(fp.steam);
  env_flatpak_->setText(!fp.enabled ? "Steam Flatpak: <span style='color:#e8912d'>não integrado</span>"
                                    : check(fp.layer32, fp.layer32 ? "Steam Flatpak: integrado (64 e 32 bits)"
                                                                   : "Steam Flatpak: integrado (só 64 bits)"));
  const QString cmd = launch_command();
  run_cmd_->setToolTip(cmd);
  // O caminho do Flatpak é longo: mostra só o fim; Copiar leva o completo.
  run_cmd_->setText("<code>" + (fp.enabled ? "…/framux/bin/framux-run %command%" : cmd).toHtmlEscaped() + "</code>");
}

QString MainWindow::launch_command() const {
  return flatpak::status().enabled ? QString::fromStdString(flatpak::run_command())
                                   : QStringLiteral("framux run %command%");
}

// ---------------------------------------------------------------------------
// Bandeja e fechar

void MainWindow::build_tray() {
  if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
  tray_ = new QSystemTrayIcon(app_icon(), this);
  tray_->setToolTip("Framux");
  auto *menu = new QMenu(this);
  menu->addAction("Abrir o Framux", this, [this] { present(false); });
  menu->addSeparator();
  menu->addAction("Abrir pasta de benchmarks", this, [this] {
    const QString d = expanded_folder(store_.get().benchmark.folder);
    QDir().mkpath(d);
    QDesktopServices::openUrl(QUrl::fromLocalFile(d));
  });
  menu->addAction("Abrir pasta de vídeos", this, [this] {
    const QString d = expanded_folder(store_.get().video.folder);
    QDir().mkpath(d);
    QDesktopServices::openUrl(QUrl::fromLocalFile(d));
  });
  menu->addAction("Abrir pasta de screenshots", this, [this] {
    const QString d = expanded_folder(store_.get().screenshot.folder);
    QDir().mkpath(d);
    QDesktopServices::openUrl(QUrl::fromLocalFile(d));
  });
  menu->addSeparator();
  menu->addAction("Parar o Framux", this, [this] {
    store_.flush();
    stop_daemon();
    qApp->quit();
  });
  tray_->setContextMenu(menu);
  connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
    if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) {
      if (isVisible() && !isMinimized())
        hide();
      else
        present(false);
    }
  });
}

void MainWindow::update_tray() {
  const bool want = store_.get().general.tray;
  if (tray_) tray_->setVisible(want);
  if (!QSystemTrayIcon::isSystemTrayAvailable() && want) {
    tray_hint_->setText(
        "Bandeja indisponível nesta sessão (no GNOME, instale a extensão AppIndicator). Sem ela, fechar a "
        "janela só a fecha; o monitoramento continua.");
    tray_hint_->show();
  } else {
    tray_hint_->hide();
  }
  qApp->setQuitOnLastWindowClosed(!tray_active());
}

void MainWindow::present(bool minimized) {
  if (minimized && tray_active()) return;
  show();
  setWindowState((windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
  raise();
  activateWindow();
}

void MainWindow::closeEvent(QCloseEvent *e) {
  store_.flush();
  if (tray_active()) {
    hide();
    e->ignore();
    if (!told_about_tray_) {
      told_about_tray_ = true;
      tray_->showMessage("Framux continua rodando", "Clique no ícone da bandeja para abrir a janela.", app_icon(),
                         4000);
    }
    return;
  }
  e->accept();
}

}  // namespace framux::gui
