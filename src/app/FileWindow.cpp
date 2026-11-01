#include <QEnterEvent>
#include <QPainter>
#include <QVariantAnimation>
#include "app/FileWindow.h"
#include "app/RoomSigns.h"
#include <QPainterPath>
#include "ui/Pie.h"
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

namespace hm::files {

namespace {
const QRegularExpression PROJ_RE("\\.(hairmesh|json)$", QRegularExpression::CaseInsensitiveOption);
const QRegularExpression THUMB_RE("\"thumb\":\"(data:image/[a-z]+;base64,[A-Za-z0-9+/=]+)\"");
QString fmtTime(qint64 t) { return t ? QLocale().toString(QDateTime::fromMSecsSinceEpoch(t), "d MMM yyyy, HH:mm") : QString(); }
QPixmap pixOf(const QString& dataUrl) {
  QPixmap p; int c = dataUrl.indexOf(','); if (c < 0) return p;
  p.loadFromData(QByteArray::fromBase64(dataUrl.mid(c + 1).toLatin1())); return p;
}
QString headOf(const QString& path, qint64 n) { QFile f(path); return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.read(n)) : QString(); }
}  // namespace

QString appDataDir() { QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation); QDir().mkpath(d); return d; }
// an installed copy (Setup: <root>\System\HairHarpy.exe next to Presets, Autosaves, Projects — the HTML version's
// layout): its data folders are there; a copy run from anywhere else — the user's app data
QString dataDir(const QString& sub) {
  QDir app(QCoreApplication::applicationDirPath());
  if (app.exists("Presets") && app.exists("Autosaves") && app.exists("Projects")) return app.filePath(sub);
  return QDir(appDataDir()).filePath(sub);
}
QString baseName(const QString& file) { QString n = QFileInfo(file).fileName(); n.remove(PROJ_RE); return n.isEmpty() ? QStringLiteral("project") : n; }

std::vector<Recent> recentList() {
  std::vector<Recent> out; QSettings s; int n = s.beginReadArray("recent");
  for (int i = 0; i < n; i++) { s.setArrayIndex(i); out.push_back({s.value("path").toString(), s.value("name").toString(), s.value("thumb").toString(), s.value("time").toLongLong()}); }
  s.endArray(); return out;
}
static void writeRecent(const std::vector<Recent>& l) {
  QSettings s; s.beginWriteArray("recent", int(l.size()));
  for (int i = 0; i < int(l.size()); i++) { s.setArrayIndex(i); s.setValue("path", l[size_t(i)].path); s.setValue("name", l[size_t(i)].name); s.setValue("thumb", l[size_t(i)].thumb); s.setValue("time", l[size_t(i)].time); }
  s.endArray();
}
void addRecent(const QString& path, const QString& thumb) {
  auto l = recentList(); QString th = thumb; std::vector<Recent> out;
  for (auto& e : l) { if (QFileInfo(e.path) == QFileInfo(path) || e.path.compare(path, Qt::CaseInsensitive) == 0) { if (th.isEmpty()) th = e.thumb; continue; } out.push_back(e); }
  out.insert(out.begin(), {path, QFileInfo(path).fileName(), th, QDateTime::currentMSecsSinceEpoch()});
  if (out.size() > 50) out.resize(50);   // (many kept — the projects' page scrolls)
  writeRecent(out);
}
void removeRecent(const QString& path) { auto l = recentList(); std::vector<Recent> out; for (auto& e : l) if (e.path != path) out.push_back(e); writeRecent(out); }
QString fileThumb(const QString& path) { auto m = THUMB_RE.match(headOf(path, 600000)); return m.hasMatch() ? m.captured(1) : QString(); }

AutosaveSettings autosaveSettings() {
  QSettings s; AutosaveSettings a;
  a.on = s.value("autosave/on", true).toBool(); a.minutes = s.value("autosave/minutes", 10).toInt(); a.copies = s.value("autosave/copies", 5).toInt();
  a.dir = s.value("autosave/dir", dataDir("Autosaves")).toString();
  return a;
}
void setAutosaveSettings(const AutosaveSettings& a) {
  QSettings s; s.setValue("autosave/on", a.on); s.setValue("autosave/minutes", a.minutes); s.setValue("autosave/copies", a.copies); s.setValue("autosave/dir", a.dir);
}
QString presetsDir() { return QSettings().value("presets/dir", dataDir("Presets")).toString(); }
void setPresetsDir(const QString& d) { QSettings().setValue("presets/dir", d); }
QString examplesDir() {
  QDir app(QCoreApplication::applicationDirPath());
  for (const QString& c : {app.filePath("Examples"), app.filePath("../../Examples")}) if (QDir(c).exists()) return QDir(c).absolutePath();
  return {};
}

namespace {

// a card: the preview, the name, a line under it (subWarn — in red: the file gone); ✕ — remove
constexpr int CARD_W = 214, CARD_H = 262, THUMB = 202, COLS = 5;
QFrame* card(const QString& thumb, const QString& name, const QString& sub, const std::function<void()>& click, const std::function<void()>& x = {}, const QString& xTip = {}, bool subWarn = false) {
  class Card : public QFrame { public: std::function<void()> fn; protected: void mouseReleaseEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton && rect().contains(e->pos()) && fn) fn(); } };
  auto* c = new Card; c->fn = click; c->setObjectName("fcard"); c->setFixedSize(CARD_W, CARD_H); c->setCursor(Qt::PointingHandCursor); c->setToolTip(name);
  auto* v = new QVBoxLayout(c); v->setContentsMargins(6, 6, 6, 6); v->setSpacing(3);
  auto* th = new QLabel; th->setFixedSize(THUMB, THUMB); th->setAlignment(Qt::AlignCenter); th->setObjectName("fth");
  QPixmap p = pixOf(thumb); if (!p.isNull()) th->setPixmap(p.scaled(THUMB, THUMB, Qt::KeepAspectRatio, Qt::SmoothTransformation)); else { th->setText("∿"); th->setStyleSheet("font-size:44px;color:#555"); }
  if (subWarn) { auto* e = new QGraphicsOpacityEffect(th); e->setOpacity(0.35); th->setGraphicsEffect(e); }   // (the file gone: its picture faded)
  QFont nf = c->font(); nf.setPointSizeF(10); auto* nm = new QLabel(QFontMetrics(nf).elidedText(name, Qt::ElideRight, THUMB)); nm->setFont(nf); nm->setStyleSheet("color:#e0e0e0");
  auto* dt = new QLabel(sub); dt->setStyleSheet(subWarn ? "color:#d0705a;font-size:11px" : "color:#888;font-size:11px");
  v->addWidget(th); v->addWidget(nm); v->addWidget(dt);
  if (x) { auto* b = new QPushButton("✕", c); b->setToolTip(xTip.isEmpty() ? QObject::tr("Remove") : xTip); b->setFixedSize(22, 22); b->move(CARD_W - 30, 8); QObject::connect(b, &QPushButton::clicked, c, x); }
  return c;
}
// an empty place in the grid: the rows kept whole
QFrame* emptyCard() { auto* c = new QFrame; c->setObjectName("fempty"); c->setFixedSize(CARD_W, CARD_H); return c; }
// the shelf hanging under the rooms' band: a trapezoid of its colour, the buttons on it
class Shelf : public QWidget {
public:
  explicit Shelf(QWidget* parent = nullptr) : QWidget(parent) {}
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing); const double w = width(), h = height(), s = h * 0.9;   // (s: the slant of its sides)
    QPolygonF poly{QPointF(0, -1), QPointF(w, -1), QPointF(w - s, h - 0.5), QPointF(s, h - 0.5)};
    p.setPen(Qt::NoPen); p.setBrush(QColor(0x26, 0x26, 0x26)); p.drawPolygon(poly);
    p.setPen(QPen(QColor(0x3a, 0x3a, 0x3a), 1)); p.drawLine(QPointF(0, 0), QPointF(s, h - 0.5)); p.drawLine(QPointF(s, h - 0.5), QPointF(w - s, h - 0.5)); p.drawLine(QPointF(w - s, h - 0.5), QPointF(w, 0));
  }
};
// a room to start in: its picture (growing a little under the pointer), its name and what it is for; click — chosen
class RoomTile : public QWidget {
public:
  RoomTile(const QString& image, const QString& name, const QString& sub, std::function<void()> click) : img_(QPixmap(image)), name_(name), sub_(sub), click_(std::move(click)) {
    setFixedSize(240, 266); setCursor(Qt::PointingHandCursor);   // (no tooltip: the text is under it already)
    anim_ = new QVariantAnimation(this); anim_->setDuration(160); anim_->setEasingCurve(QEasingCurve::OutCubic);
    QObject::connect(anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) { k_ = v.toDouble(); update(); });
  }
protected:
  void enterEvent(QEnterEvent*) override { grow(1.12); }
  void leaveEvent(QEvent*) override { grow(1.0); }
  void mouseReleaseEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton && rect().contains(e->pos()) && click_) click_(); }
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::SmoothPixmapTransform);
    const double s = 148 * k_; QRectF r((width() - s) / 2, 102 - s / 2, s, s);   // (room above it for the grown picture and its glow)
    if (k_ > 1.001) { p.setPen(Qt::NoPen); p.setBrush(QColor(0x4a, 0x7f, 0xd6, int(90 * (k_ - 1) / 0.12))); p.drawRoundedRect(r.adjusted(-6, -6, 6, 6), 30 * k_, 30 * k_); }   // (a glow behind it)
    p.drawPixmap(r, img_, img_.rect());
    QFont f = font(); f.setPointSizeF(10.5); f.setBold(true); p.setFont(f); p.setPen(QColor(0xe8, 0xe8, 0xe8));
    p.drawText(QRectF(0, 188, width(), 22), Qt::AlignCenter, name_);
    f.setPointSizeF(8); f.setBold(false); p.setFont(f); p.setPen(QColor(0x9a, 0x9a, 0x9a));
    p.drawText(QRectF(6, 212, width() - 12, 54), Qt::AlignHCenter | Qt::AlignTop | Qt::TextWordWrap, sub_);
  }
private:
  void grow(double to) { anim_->stop(); anim_->setStartValue(k_); anim_->setEndValue(to); anim_->start(); }
  QPixmap img_; QString name_, sub_; std::function<void()> click_; QVariantAnimation* anim_; double k_ = 1;
};

QLabel* fhint(const QString& t) { auto* l = new QLabel(t); l->setWordWrap(true); l->setStyleSheet("color:#8a8a8a"); return l; }

class FileWindow : public QDialog {
public:
  // (the whole window's inside: a frameless window of its own laid over the main one's — the viewport is a native window,
  // nothing drawn inside the main window covers it — following it as it moves and resizes)
  FileWindow(QWidget* parent, const Host& h) : QDialog(parent, Qt::Tool | Qt::FramelessWindowHint), host_(h) {
    setWindowTitle(QObject::tr("File")); setObjectName("fwin"); setAttribute(Qt::WA_StyledBackground);   // (kept: closing hides it)
    setStyleSheet("#fwin{background:#1e1e1e} #fcard{background:#232323;border:1px solid #3a3a3a;border-radius:6px} #fcard:hover{border-color:#6a8fd0} #fth{background:#1a1a1a;border-radius:4px}"
                  " #fempty{background:#212121;border:1px solid #2c2c2c;border-radius:6px}"
                  " #frooms{background:#262626} #fscroll, #fscroll > QWidget, #fscroll > QWidget > QWidget{background:transparent}"
                  " QPushButton[ftab=\"true\"]{padding:5px 14px;background:#333;border:1px solid #3c3c3c;border-radius:3px;color:#ccc} QPushButton[ftab=\"true\"]:hover{border-color:#5a5a5a;color:#eee}"
                  " QPushButton[ftab=\"true\"][on=\"true\"]{background:#2f5296;border-color:#4a7fd6;color:#fff}"
                  " #fscroll QScrollBar:vertical{background:transparent;width:10px;margin:4px 2px 4px 0} #fscroll QScrollBar::handle:vertical{background:rgba(255,255,255,40);border-radius:4px;min-height:40px} #fscroll QScrollBar::handle:vertical:hover{background:rgba(255,255,255,80)} #fscroll QScrollBar::add-line:vertical, #fscroll QScrollBar::sub-line:vertical{height:0} #fscroll QScrollBar::add-page:vertical, #fscroll QScrollBar::sub-page:vertical{background:transparent}"
                  " QPushButton#fclose{background:transparent;border:none;color:#888;font-size:16px} QPushButton#fclose:hover{color:#eee}");
    auto* v = new QVBoxLayout(this); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
    // ＋ New project: the room it starts in — the band across the top
    auto* rooms = new QFrame; rooms->setObjectName("frooms"); auto* rv = new QVBoxLayout(rooms); rv->setContentsMargins(12, 40, 12, 18); rv->setSpacing(2); v->addWidget(rooms); band_ = rooms;
    auto* t = new QLabel(QObject::tr("New project — choose the room to start in")); t->setAlignment(Qt::AlignCenter); t->setStyleSheet("color:#e0e0e0;font-size:14px;font-weight:600"); rv->addWidget(t);
    auto* st = new QLabel(QObject::tr("Both rooms are in every project — switch between them any time from the top bar")); st->setAlignment(Qt::AlignCenter); st->setStyleSheet("color:#8a8a8a;font-size:11px"); rv->addWidget(st);
    auto* row = new QHBoxLayout; row->setSpacing(24); row->addStretch(1); rv->addSpacing(6); rv->addLayout(row);
    auto pick = [this](const QString& room) { return [this, room] { if (host_.newProject) host_.newProject(room); }; };
    row->addWidget(new RoomTile(":/img/room_hair.png", QObject::tr("Hair Room"), QObject::tr("Curves, walls, cocoons and crowns — stylized hair built over the head and converted to geometry"), pick("hair")));
    row->addWidget(new RoomTile(":/img/room_topo.png", QObject::tr("Topology Room"), QObject::tr("Meshes: retopology over a high-poly model, vertex normals, topology clean-up, UV"), pick("topo")));
    row->addStretch(1);
    auto* x = new QPushButton("✕", rooms); x->setObjectName("fclose"); x->setFixedSize(30, 30); x->setCursor(Qt::PointingHandCursor); x->setToolTip(QObject::tr("Close (Esc)"));
    QObject::connect(x, &QPushButton::clicked, this, [this] { close(); }); closeBtn_ = x;
    // the shelf under the band: Open… and the tabs, one look
    auto* shelf = new Shelf; auto* head = new QHBoxLayout(shelf); head->setContentsMargins(58, 6, 58, 12); head->setSpacing(6);
    auto* o = new QPushButton(QObject::tr("📂 Open…")); o->setProperty("ftab", true); o->setToolTip(QObject::tr("Open a project file from the disk"));
    QObject::connect(o, &QPushButton::clicked, this, [this] { if (host_.openDialog) host_.openDialog(); });   // (the start screen stays behind the dialog — gone only when a project opens)
    head->addWidget(o);
    for (auto [k, lab] : {std::pair<const char*, const char*>{"recent", "Recent projects"}, {"examples", "Examples"}, {"autosave", "Autosaves"}, {"migrate", "Migrate"}}) {
      auto* b = new QPushButton(QObject::tr(lab)); b->setProperty("ftab", true); b->setProperty("key", k); tabs_.push_back(b); head->addWidget(b);
      QObject::connect(b, &QPushButton::clicked, this, [this, k] { tab_ = k; QSettings().setValue("fileWindow/tab", tab_); render(); });
    }
    shelf->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);   // (its size from the buttons as styled)
    v->addWidget(shelf, 0, Qt::AlignHCenter);
    scroll_ = new QScrollArea; scroll_->setObjectName("fscroll"); scroll_->setWidgetResizable(true); scroll_->setFrameShape(QFrame::NoFrame); v->addSpacing(14);
    auto* sw = new QHBoxLayout; sw->setContentsMargins(10, 0, 10, 0); sw->addWidget(scroll_); v->addLayout(sw, 1);
    if (parent) parent->installEventFilter(this);
  }
  void show(const QString& tab) { if (!tab.isEmpty() && tab != "new") tab_ = tab; build(); fit(); QDialog::show(); raise(); activateWindow(); }
  // (over the main window's inside, wherever it is)
  void fit() { QWidget* p = parentWidget(); if (!p) { resize(1200, 860); return; }
    const QRect r(p->mapToGlobal(QPoint(0, 0)), p->size()); setGeometry(r); if (closeBtn_) closeBtn_->move(r.width() - 40, 8); }
protected:
  // (behind the projects: the two rooms' signs, large and faint, not mirrored — the hair's bigger and lower at the left,
  // the topology's smaller and higher at the right — each fading out towards the middle)
  void paintEvent(QPaintEvent* e) override {
    QDialog::paintEvent(e); QPainter p(this); p.setRenderHint(QPainter::SmoothPixmapTransform);
    const int top = band_ ? band_->geometry().bottom() : 0, h = height() - top; if (h <= 0) return;
    p.setClipRect(QRect(0, top + 1, width(), h));
    // (a sign drawn at r, its strength along the line from `from` (full) to `to` (none), in r's own fractions)
    auto sign = [&](const float* pts, const int* loops, int nLoops, const QRectF& r, QPointF from, QPointF to, double peak) {
      if (r.width() < 2) return;
      QImage im(r.size().toSize(), QImage::Format_ARGB32_Premultiplied); im.fill(Qt::transparent);
      { QPainter q(&im); q.setRenderHint(QPainter::Antialiasing);   // (its outlines — sharp at any size)
        QPainterPath path; path.setFillRule(Qt::OddEvenFill); const double W = im.width(), H = im.height(); int k = 0;
        for (int l = 0; l < nLoops; l++) { for (int i = 0; i < loops[l]; i++, k++) { const QPointF q2(pts[2 * k] * W, pts[2 * k + 1] * H); if (i == 0) path.moveTo(q2); else path.lineTo(q2); } path.closeSubpath(); }
        q.fillPath(path, QColor(0xe8, 0xe8, 0xe8));
        QLinearGradient g(QPointF(from.x() * im.width(), from.y() * im.height()), QPointF(to.x() * im.width(), to.y() * im.height()));
        g.setColorAt(0, QColor(0, 0, 0, 255)); g.setColorAt(0.55, QColor(0, 0, 0, 110)); g.setColorAt(1, QColor(0, 0, 0, 0));
        q.setCompositionMode(QPainter::CompositionMode_DestinationIn); q.fillRect(im.rect(), g); }
      p.setOpacity(peak); p.drawImage(r.topLeft(), im); };
    const double sh = std::max(h * 1.3, width() * 0.46), st = std::max(h * 0.82, width() * 0.3);
    sign(signs::kHair, signs::kHairLoops, signs::kHairLoopCount, QRectF(-sh * 0.34, top + h * 0.2, sh, sh), QPointF(0.15, 0.85), QPointF(0.85, 0.2), 0.17);
    sign(signs::kTopo, signs::kTopoLoops, signs::kTopoLoopCount, QRectF(width() - st * 0.72, top - st * 0.12, st, st), QPointF(0.85, 0.15), QPointF(0.15, 0.9), 0.14);
  }
public:
  bool eventFilter(QObject* o, QEvent* e) override {
    if (o == parentWidget() && isVisible() && (e->type() == QEvent::Move || e->type() == QEvent::Resize || e->type() == QEvent::WindowStateChange)) fit();
    return QDialog::eventFilter(o, e); }
  // (a re-render asked from a button inside the page: after its click is over — the page is replaced)
  void render() { QTimer::singleShot(0, this, [this] { build(); }); }
  void build() {
    for (auto* b : tabs_) { b->setProperty("on", b->property("key").toString() == tab_); b->style()->polish(b); }
    // (the page as wide as its grid of five, in the middle of the window)
    auto* outer = new QWidget; auto* oh = new QHBoxLayout(outer); oh->setContentsMargins(0, 0, 0, 0); oh->addStretch(1);
    auto* w = new QWidget; w->setFixedWidth(COLS * CARD_W + (COLS - 1) * 12 + 4); oh->addWidget(w); oh->addStretch(1);
    auto* v = new QVBoxLayout(w); v->setContentsMargins(2, 6, 2, 2); v->setSpacing(8);
    auto bar = [&]() { auto* r = new QHBoxLayout; r->setSpacing(6); v->addLayout(r); return r; };
    std::vector<QGridLayout*> grids;
    auto grid = [&]() { auto* g = new QGridLayout; g->setSpacing(12); g->setAlignment(Qt::AlignLeft | Qt::AlignTop); v->addLayout(g); grids.push_back(g); return g; };
    auto put = [](QGridLayout* g, QWidget* c) { int n = g->count(); g->addWidget(c, n / COLS, n % COLS); };
    // (the rows kept whole with empty places — Recent: two rows at least)
    auto pad = [&](QGridLayout* g, int rows) { const int n = g->count(), want = std::max(rows * COLS, (n + COLS - 1) / COLS * COLS); for (int i = n; i < want; i++) put(g, emptyCard()); };
    auto bottomBtn = [](const QString& text, const QString& tip) { auto* b = new QPushButton(text); b->setProperty("ftab", true); b->setToolTip(tip); b->setMinimumSize(190, 40); b->setStyleSheet("font-size:13px"); return b; };
    if (tab_ == "recent") {
      auto l = recentList();
      if (l.empty()) v->addWidget(fhint(QObject::tr("No recent projects yet — open or save one.")));
      else { auto* g = grid(); int gone = 0; for (auto& e : l) { QString p = e.path; const bool here = QFileInfo::exists(p); gone += !here;
          put(g, card(e.thumb, e.name, here ? fmtTime(e.time) : QObject::tr("Not found — moved or deleted"), [this, p] { if (!QFileInfo::exists(p)) { ui::toast(QObject::tr("The file is not there any more — moved or deleted?")); return; } if (host_.open && host_.open(p)) close(); },
                      [this, p] { removeRecent(p); render(); }, QObject::tr("Remove from the list (the file stays)"), !here)); }
        pad(g, 2);
        // (under the grid: the list emptied, or only the files gone taken off it — the files themselves stay)
        v->addSpacing(40); auto* r = bar(); r->addStretch(1);
        auto* all = bottomBtn(QObject::tr("Hide all"), QObject::tr("Take every project off this list — the files stay where they are"));
        QObject::connect(all, &QPushButton::clicked, this, [this] { if (QMessageBox::question(this, QObject::tr("Hide all"), QObject::tr("Take every project off the recent list? The files stay on the disk.")) != QMessageBox::Yes) return; writeRecent({}); render(); });
        auto* miss = bottomBtn(QObject::tr("Hide missing"), QObject::tr("Take off the list the projects whose files are gone (moved or deleted)"));
        miss->setEnabled(gone > 0);
        QObject::connect(miss, &QPushButton::clicked, this, [this] { std::vector<Recent> keep; for (auto& e : recentList()) if (QFileInfo::exists(e.path)) keep.push_back(e); writeRecent(keep); render(); });
        r->addWidget(all); r->addSpacing(40); r->addWidget(miss); r->addStretch(1); }
    } else if (tab_ == "examples") {
      bar()->addWidget(fhint(QObject::tr("Finished hairstyles made in the program — open one to see how it is built (curves, walls, crown, guides, conversion settings). It opens as a copy: Save asks where to keep yours.")), 1);
      exampleCards(v, grid, put, false);
    } else if (tab_ == "autosave") {
      auto* b = bar(); auto* s = new QPushButton(QObject::tr("⏱ Autosave settings…")); QObject::connect(s, &QPushButton::clicked, this, [this] { if (host_.autosaveSettings) host_.autosaveSettings(); render(); }); b->addWidget(s);
      AutosaveSettings a = autosaveSettings();
      b->addWidget(fhint(QObject::tr("The newest 5 copies in \"%1\". Opening one doesn't change it — save the project to keep it.").arg(a.dir)), 1);
      QFileInfoList fl = QDir(a.dir).entryInfoList({"*_autosave_*.hairmesh", "*_autosave_*.json"}, QDir::Files, QDir::Time);
      if (fl.isEmpty()) v->addWidget(fhint(QObject::tr("No autosaves in the folder yet.")));
      else { auto* g = grid(); for (int i = 0; i < std::min<int>(5, int(fl.size())); i++) { QString p = fl[i].absoluteFilePath(), n = fl[i].fileName();
          put(g, card(fileThumb(p), n, fmtTime(fl[i].lastModified().toMSecsSinceEpoch()), [this, p, n] {
            QString name = QString(n).remove(QRegularExpression("_autosave_\\d+\\.(hairmesh|json)$", QRegularExpression::CaseInsensitiveOption)) + ".hairmesh";
            if (host_.openCopy && host_.openCopy(p, name, true)) { close(); ui::toast(QObject::tr("Opened the autosave %1 — save it to keep it").arg(n)); } })); } }
    } else {
      // presets (collapsible, closed by default)
      bool open = QSettings().value("presets/open", false).toBool();
      auto* sh = new QPushButton((open ? "▾ " : "▸ ") + QObject::tr("Presets")); sh->setFlat(true); sh->setStyleSheet("text-align:left;font-weight:bold"); v->addWidget(sh);
      QObject::connect(sh, &QPushButton::clicked, this, [this, open] { QSettings().setValue("presets/open", !open); render(); });
      if (open) {
        v->addWidget(fhint(QObject::tr("Built in — the examples' hair (as one group):")));
        exampleCards(v, grid, put, true);
        v->addWidget(fhint(QObject::tr("Your presets:")));
        QString pd = presetsDir();
        auto* r1 = bar(); r1->addWidget(fhint(QObject::tr("Presets folder:"))); auto* pl = new QLabel(pd); pl->setStyleSheet("color:#ddd"); r1->addWidget(pl, 1);
        auto* ch = new QPushButton(QObject::tr("Choose folder…")); ch->setToolTip(QObject::tr("The folder where all presets are kept (one .hairmesh file each)")); r1->addWidget(ch);
        QObject::connect(ch, &QPushButton::clicked, this, [this, pd] { QString d = QFileDialog::getExistingDirectory(this, QObject::tr("Presets folder"), pd); if (!d.isEmpty()) { setPresetsDir(d); render(); } });
        auto* r2 = bar(); auto* cp = new QPushButton(QObject::tr("＋ Create preset")); cp->setToolTip(QObject::tr("Saves the group selected in the scene as a preset, with a picture of it"));
        QString gn = host_.presetGroupName ? host_.presetGroupName() : QString();
        r2->addWidget(cp); r2->addWidget(fhint(gn.isEmpty() ? QObject::tr("select a group in the scene first") : QObject::tr("from the group \"%1\"").arg(gn)), 1);
        QObject::connect(cp, &QPushButton::clicked, this, [this, pd] { if (host_.createPreset && host_.createPreset(pd)) render(); });
        QFileInfoList fl = QDir(pd).entryInfoList({"*.hairmesh", "*.json"}, QDir::Files, QDir::Name);
        std::vector<std::tuple<QString, QString, QString>> ps;   // path, name, thumb
        for (auto& fi : fl) { QString h = headOf(fi.absoluteFilePath(), 600000); if (!h.contains("\"preset\":true")) continue;
          auto nm = QRegularExpression("\"name\":\"((?:[^\"\\\\]|\\\\.)*)\"").match(h); auto th = THUMB_RE.match(h);
          ps.emplace_back(fi.absoluteFilePath(), nm.hasMatch() ? nm.captured(1) : baseName(fi.fileName()), th.hasMatch() ? th.captured(1) : QString()); }
        std::sort(ps.begin(), ps.end(), [](auto& a, auto& b) { return std::get<1>(a).localeAwareCompare(std::get<1>(b)) < 0; });
        if (ps.empty()) v->addWidget(fhint(QObject::tr("No presets in the folder yet.")));
        else { auto* g = grid(); for (auto& [p, n, t] : ps) { QString path = p, fileName = QFileInfo(p).fileName();
            put(g, card(t, n, fileName, [this, path] { if (host_.migrate && host_.migrate(path, QString(), false)) close(); },
                        [this, path, fileName] { if (QMessageBox::question(this, QObject::tr("Delete preset"), QObject::tr("Delete the preset file \"%1\"?").arg(fileName)) != QMessageBox::Yes) return;
                          if (!QFile::remove(path)) ui::toast(QObject::tr("Could not delete the file")); render(); }, QObject::tr("Delete the preset file"))); } }
      }
      auto* b = bar(); auto* o = new QPushButton(QObject::tr("📂 Choose file…")); o->setProperty("primary", true);
      QObject::connect(o, &QPushButton::clicked, this, [this] { if (host_.migrateDialog) host_.migrateDialog(); });
      b->addWidget(o); b->addWidget(fhint(QObject::tr("The project's curves, NURBS surfaces and modifiers come into a new group named after the file (its groups inside kept). A file can also be dropped onto the window.")), 1);
      auto* h4 = new QLabel(QObject::tr("Recent projects")); h4->setStyleSheet("font-weight:bold;color:#ccc"); v->addWidget(h4);
      auto l = recentList();
      if (l.empty()) v->addWidget(fhint(QObject::tr("No recent projects yet.")));
      else { auto* g = grid(); for (auto& e : l) { QString p = e.path;
          put(g, card(e.thumb, e.name, fmtTime(e.time), [this, p] { if (!QFileInfo::exists(p)) { ui::toast(QObject::tr("The file is not there any more — moved or deleted?")); return; } if (host_.migrate && host_.migrate(p, QString(), true)) close(); })); } }
    }
    for (auto* g : grids) if (g != grids.front() || tab_ != "recent") pad(g, 1);   // (Recent's grid: padded above, before its buttons)
    v->addStretch(1);
    scroll_->setWidget(outer);
  }

private:
  template <class G, class P> void exampleCards(QVBoxLayout* v, G grid, P put, bool migrate) {
    QString d = examplesDir();
    QFileInfoList fl = d.isEmpty() ? QFileInfoList() : QDir(d).entryInfoList({"*.hairmesh", "*.json"}, QDir::Files, QDir::Name);
    if (d.isEmpty() || fl.isEmpty()) { v->addWidget(fhint(QObject::tr("No examples in the program's Examples folder."))); return; }
    auto* g = grid();
    for (auto& fi : fl) {
      QString p = fi.absoluteFilePath(), nm = baseName(fi.fileName()).remove(QRegularExpression("^ExampleProject[-_ ]*", QRegularExpression::CaseInsensitiveOption)); if (nm.isEmpty()) nm = baseName(fi.fileName());
      QString file = fi.fileName();
      put(g, card(fileThumb(p), nm, migrate ? QObject::tr("Example") : QObject::tr("Example project"), [this, p, nm, file, migrate] {
        if (migrate) { if (host_.migrate && host_.migrate(p, nm, true)) close(); return; }
        if (host_.openCopy && host_.openCopy(p, file, false)) { close(); ui::toast(QObject::tr("Example \"%1\" opened — Save keeps your own copy").arg(nm)); } }));
    }
  }
  Host host_; QString tab_ = QSettings().value("fileWindow/tab", "recent").toString();
  std::vector<QPushButton*> tabs_;
  QScrollArea* scroll_; QWidget* closeBtn_ = nullptr; QWidget* band_ = nullptr;
};

QPointer<FileWindow> g_fw;

}  // namespace

void openFileWindow(QWidget* parent, const Host& host, const QString& tab) {
  if (!g_fw) g_fw = new FileWindow(parent, host);
  g_fw->show(tab);
}
void closeFileWindow() { if (g_fw) g_fw->close(); }

void openAutosaveDialog(QWidget* parent, const QString& projectBase, const QString& lastMsg, const std::function<void()>& saveNow) {
  QDialog d(parent); d.setWindowTitle(QObject::tr("Autosave")); auto* v = new QVBoxLayout(&d);
  AutosaveSettings a = autosaveSettings();
  auto* on = new QCheckBox(QObject::tr("Enabled")); on->setChecked(a.on); v->addWidget(on);
  auto row = [&](const QString& lab, QWidget* w) { auto* h = new QHBoxLayout; auto* l = new QLabel(lab); l->setMinimumWidth(170); h->addWidget(l); h->addWidget(w, 1); v->addLayout(h); };
  auto* mi = new QSpinBox; mi->setRange(1, 240); mi->setValue(a.minutes); row(QObject::tr("Every (minutes)"), mi);
  auto* cp = new QSpinBox; cp->setRange(1, 50); cp->setValue(a.copies); row(QObject::tr("Copies (overwritten in turn)"), cp);
  auto* fr = new QHBoxLayout; auto* fl = new QLabel(QObject::tr("Folder")); fl->setMinimumWidth(170); auto* fn = new QLabel(a.dir); fn->setWordWrap(true); fn->setStyleSheet("color:#ddd");
  auto* ch = new QPushButton(QObject::tr("Choose folder…")); fr->addWidget(fl); fr->addWidget(fn, 1); fr->addWidget(ch); v->addLayout(fr);
  auto* note = new QLabel(QObject::tr("Files: %1_autosave_1…%2.hairmesh.").arg(projectBase).arg(a.copies)); note->setStyleSheet("color:#8a8a8a"); v->addWidget(note);
  if (!lastMsg.isEmpty()) { auto* m = new QLabel(lastMsg); m->setStyleSheet("color:#e0b060"); m->setWordWrap(true); v->addWidget(m); }
  auto save = [&] { a.on = on->isChecked(); a.minutes = mi->value(); a.copies = cp->value(); setAutosaveSettings(a); note->setText(QObject::tr("Files: %1_autosave_1…%2.hairmesh.").arg(projectBase).arg(a.copies)); };
  QObject::connect(on, &QCheckBox::toggled, &d, save); QObject::connect(mi, &QSpinBox::valueChanged, &d, save); QObject::connect(cp, &QSpinBox::valueChanged, &d, save);
  QObject::connect(ch, &QPushButton::clicked, &d, [&] { QString x = QFileDialog::getExistingDirectory(&d, QObject::tr("Autosave folder"), a.dir); if (!x.isEmpty()) { a.dir = x; fn->setText(x); save(); } });
  auto* bar = new QHBoxLayout; bar->addStretch(1);
  auto* now = new QPushButton(QObject::tr("Save a copy now")); auto* cl = new QPushButton(QObject::tr("Close"));
  QObject::connect(now, &QPushButton::clicked, &d, [&] { save(); if (saveNow) saveNow(); });
  QObject::connect(cl, &QPushButton::clicked, &d, &QDialog::accept);
  bar->addWidget(now); bar->addWidget(cl); v->addLayout(bar);
  d.exec();
}

}  // namespace hm::files
