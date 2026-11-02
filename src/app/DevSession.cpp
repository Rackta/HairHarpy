// The session recording (HM_DEVTOOLS): the session's record and its annotations — see DevSession.h. The record: one JSON object
// a line (t — seconds from the start, kind, its data) in <the program>/Sessions (when there) or <Documents>/HairHarpy Sessions, session_<date>/session.jsonl;
// an annotation's picture (the window with the drawing over it) beside it as note_<n>.png.
#ifdef HM_DEVTOOLS
#include "app/DevSession.h"
#include "app/CameraMenu.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "view/Viewport.h"
#include <QAbstractButton>
#include <QApplication>
#include <QGuiApplication>
#include <QMenu>
#include <QSysInfo>
#include <QWheelEvent>
#include <QWindow>
#include <QClipboard>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMessageBox>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStandardPaths>
#include <QTimer>
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <csignal>
#include <string>
#endif
#include <QVBoxLayout>

namespace hm {

namespace {
// the picture, drawn on with the mouse (strokes in the picture's own pixels), shown fitted into the widget
class DrawCanvas : public QWidget {
public:
  explicit DrawCanvas(const QImage& img) : img_(img) { setMinimumSize(480, 300); setCursor(Qt::CrossCursor); }
  QColor pen = QColor(0xff, 0x30, 0x30); int width = 4;
  void undo() { if (!strokes_.empty()) { strokes_.pop_back(); update(); } }
  void clear() { strokes_.clear(); update(); }
  QImage result() const { QImage o = img_.convertToFormat(QImage::Format_ARGB32); QPainter p(&o); p.setRenderHint(QPainter::Antialiasing); paintStrokes(p, 1); return o; }
  bool drawn() const { return !strokes_.empty(); }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this); p.fillRect(rect(), QColor(0x1e, 0x1e, 0x1e)); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::SmoothPixmapTransform);
    const QRectF r = fit(); p.drawImage(r, img_); p.translate(r.topLeft()); paintStrokes(p, r.width() / img_.width());
  }
  void mousePressEvent(QMouseEvent* e) override { strokes_.push_back({pen, width * img_.width() / std::max(1.0, fit().width()), {toImg(e->position())}}); update(); }   // (the width as on the screen, in the picture's pixels)
  void mouseMoveEvent(QMouseEvent* e) override { if (!(e->buttons() & Qt::LeftButton) || strokes_.empty()) return; strokes_.back().pts.push_back(toImg(e->position())); update(); }
private:
  struct Stroke { QColor c; double w; std::vector<QPointF> pts; };
  QRectF fit() const { const double s = std::min(width_() / img_.width(), height_() / img_.height()); const double w = img_.width() * s, h = img_.height() * s; return QRectF((width_() - w) / 2, (height_() - h) / 2, w, h); }
  double width_() const { return QWidget::width(); } double height_() const { return QWidget::height(); }
  QPointF toImg(const QPointF& p) const { const QRectF r = fit(); return (p - r.topLeft()) * (img_.width() / r.width()); }
  void paintStrokes(QPainter& p, double s) const {
    for (const Stroke& st : strokes_) { QPen pn(st.c, st.w * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin); p.setPen(pn);
      if (st.pts.size() == 1) { p.drawPoint(st.pts[0] * s); continue; } QPainterPath path(st.pts[0] * s); for (size_t i = 1; i < st.pts.size(); i++) path.lineTo(st.pts[i] * s); p.drawPath(path); } }
  QImage img_; std::vector<Stroke> strokes_;
};
#ifdef _WIN32
// a crash: crash_<time>.dmp (a minidump) and a "crash" line — the code, the place as an offset into the exe (read with the
// build's HairHarpy.map / .pdb), the last action — into the session's folder (not recording: the Sessions folder, the
// dump only). Fixed buffers only: the heap may be what broke
wchar_t g_crashDir[MAX_PATH] = L""; bool g_crashLog = false; char g_lastAct[96] = ""; ULONGLONG g_t0 = 0; volatile LONG g_dumped = 0;
void writeCrash(EXCEPTION_POINTERS* ep) {
  if (!g_crashDir[0] || InterlockedExchange(&g_dumped, 1)) return;
  CreateDirectoryW(g_crashDir, nullptr);   // (the Sessions folder is made only when something goes into it)
  wchar_t path[MAX_PATH + 40]; SYSTEMTIME st; GetLocalTime(&st);
  swprintf(path, MAX_PATH + 40, L"%ls\\crash_%02d%02d%02d.dmp", g_crashDir, st.wHour, st.wMinute, st.wSecond);
  HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mi{GetCurrentThreadId(), ep, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h, MINIDUMP_TYPE(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithThreadInfo), ep ? &mi : nullptr, nullptr, nullptr);
    CloseHandle(h);
  }
  if (!g_crashLog) return;
  const auto base = reinterpret_cast<unsigned long long>(GetModuleHandleW(nullptr));
  const auto at = ep && ep->ExceptionRecord ? reinterpret_cast<unsigned long long>(ep->ExceptionRecord->ExceptionAddress) : 0ULL;
  char line[512]; snprintf(line, sizeof line, "{\"kind\":\"crash\",\"important\":true,\"code\":\"0x%08lx\",\"exeOffset\":\"0x%llx\",\"after\":\"%s\",\"t\":%.3f}\n",
                           ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0xabul, at >= base ? at - base : at, g_lastAct, double(GetTickCount64() - g_t0) / 1000.0);
  for (const wchar_t* tail : {L"session.jsonl", L"important.jsonl"}) {
    wchar_t f[MAX_PATH + 40]; swprintf(f, MAX_PATH + 40, L"%ls\\%ls", g_crashDir, tail);
    if (FILE* o = _wfopen(f, L"ab")) { fputs(line, o); fclose(o); }
  }
}
LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) { writeCrash(ep); return EXCEPTION_CONTINUE_SEARCH; }
void onAbort(int) { writeCrash(nullptr); }
void setCrashDir(const QString& d, bool log) { const std::wstring w = QDir::toNativeSeparators(d).toStdWString(); wcsncpy(g_crashDir, w.c_str(), MAX_PATH - 1); g_crashLog = log; }
#endif
}  // namespace

DevSession::DevSession(QWidget* window, Viewport* vp, Scene* sc, std::function<QImage()> grabWindow, std::function<QString()> projectName)
    : QObject(window), win_(window), vp_(vp), sc_(sc), grab_(std::move(grabWindow)), name_(std::move(projectName)) {
  // (what the user does: the tools, the modes, the selections — each once as it changes)
  connect(vp_, &Viewport::toolChanged, this, [this](const QString& t) { if (on_ && t != lastTool_) { lastTool_ = t; event("tool", {{"tool", t}}); } });
  connect(vp_, &Viewport::topoState, this, [this] { if (!on_) return; const QString md = vp_->topoMode, pk = vp_->pickedInfo();
    if (md != lastMode_) { lastMode_ = md; event("mode", {{"mode", md}}); }
    if (pk != lastSel_) { lastSel_ = pk; event("picked", {{"picked", pk}}); } });
  connect(vp_, &Viewport::selectionChanged, this, [this](const std::vector<int>& items) { if (!on_) return; QJsonArray a;
    for (int id : items) if (const Item* it = sc_->item(id)) a.append(QJsonObject{{"id", id}, {"name", it->name}}); event("select", {{"items", a}}); });
  ui::toastHook() = [this](const QString& msg) { if (on_) event("message", {{"text", msg}}); };
  // (the window stopped: a tick late by more than 0.3 s — how long, after what)
  timer_ = new QTimer(this); timer_->setInterval(50);
  connect(timer_, &QTimer::timeout, this, [this] { const qint64 gap = tick_.restart(); if (on_ && gap > 300) event("stall", {{"ms", double(gap)}, {"after", lastAction_}}); });
#ifdef _WIN32
  setCrashDir(sessionsRoot(false), false); SetUnhandledExceptionFilter(crashFilter); signal(SIGABRT, onAbort);   // (a crash: its dump kept)
#endif
}

// a Sessions folder beside the program — a copy handed to a tester: the records there, sent back with it; else, or where the
// program's folder can't be written to, in Documents
QString DevSession::sessionsRoot(bool make) {
  QDir docs(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
  QString root = docs.filePath("HairHarpy Sessions");
  if (!QFileInfo(root).exists() && QFileInfo(docs.filePath("HairMeshCreator Sessions")).isDir()) docs.rename("HairMeshCreator Sessions", "HairHarpy Sessions");   // (the name before 2.0: its records kept)
  { const QString local = QDir(QCoreApplication::applicationDirPath()).filePath("Sessions");
    if (QFileInfo(local).isDir()) { QFile probe(QDir(local).filePath(".write_test")); if (probe.open(QIODevice::WriteOnly)) { probe.close(); probe.remove(); root = local; } } }
  if (make) QDir().mkpath(root);
  return root;
}

void DevSession::start() {
  if (on_) return;
  dir_ = QDir(sessionsRoot()).filePath("session_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")); QDir().mkpath(dir_);
  file_ = QDir(dir_).filePath("session.jsonl"); notes_ = 0; on_ = true; clock_.start(); tick_.start(); timer_->start(); qApp->installEventFilter(this);
  lastTool_ = vp_->tool(); lastMode_ = vp_->topoMode; lastSel_ = vp_->pickedInfo();
  { QJsonObject s = state();   // (the system: what a problem on one of them depends on)
    s.insert("system", QJsonObject{{"os", QSysInfo::prettyProductName()}, {"qt", qVersion()}, {"platform", QGuiApplication::platformName()},
                                   {"session", QString::fromLocal8Bit(qgetenv("XDG_SESSION_TYPE"))}, {"desktop", QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP"))}});
    event("start", s); }
#ifdef _WIN32
  setCrashDir(dir_, true); g_t0 = GetTickCount64() - ULONGLONG(clock_.elapsed());
#endif
  ui::toast(QObject::tr("Session recording — into %1").arg(QDir::toNativeSeparators(dir_))); if (changed) changed();
}

void DevSession::stop() {
  if (!on_) return;
  event("stop", state()); on_ = false; timer_->stop(); qApp->removeEventFilter(this);
#ifdef _WIN32
  setCrashDir(sessionsRoot(), false);
#endif
  QApplication::clipboard()->setText(QDir::toNativeSeparators(dir_));
  ui::toast(QObject::tr("Session recorded: %1 (the folder's path copied)").arg(QDir::toNativeSeparators(dir_))); if (changed) changed();
}

void DevSession::event(const QString& kind, const QJsonObject& data) {
  if (!on_) return;
  if (kind == "action") lastAction_ = data.value("id").toString();
#ifdef _WIN32
  { QString w = kind + " " + (data.contains("id") ? data.value("id").toVariant().toString() : data.contains("tool") ? data.value("tool").toString() : data.contains("picked") ? data.value("picked").toString() : data.value("button").toString());   // (a crash line: what was done last)
    w.replace('"', "'").replace('\\', '/'); strncpy(g_lastAct, w.toUtf8().left(90).constData(), sizeof g_lastAct - 1); }
#endif
  QJsonObject o = data; o.insert("t", double(clock_.elapsed()) / 1000.0); o.insert("kind", kind);
  // (the important ones — a stall, an annotation, a failure, the start and the end — marked, and kept in important.jsonl
  // too: read first, the rest only round them)
  const QString txt = data.value("text").toString().toLower();
  const bool important = kind == "stall" || kind == "annotation" || kind == "start" || kind == "stop" || kind == "error"
                         || (kind == "message" && (txt.contains("fail") || txt.contains("error") || txt.contains("could not") || txt.contains("can't") || txt.contains("no such")));
  if (important) o.insert("important", true);
  const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n";
  for (const QString& path : important ? QStringList{file_, QDir(dir_).filePath("important.jsonl")} : QStringList{file_}) { QFile f(path); if (f.open(QIODevice::Append | QIODevice::Text)) f.write(line); }
}

// the project's state: the room, the tool, the mode, what is picked, the snapping, the camera, the meshes shown
QJsonObject DevSession::state() const {
  QJsonObject s{{"project", name_ ? name_() : QString()}, {"path", sc_->path}, {"room", vp_->room()}, {"tool", vp_->tool()}, {"mode", vp_->topoMode}, {"picked", vp_->pickedInfo()},
                {"changes", vp_->changeCount()}, {"camera", camPosText(vp_)},
                {"snap", QJsonObject{{"surface", vp_->tset.snap}, {"element", vp_->tset.esnap}, {"elementMode", vp_->tset.esnapMode}, {"symmetry", vp_->tset.sym}}}};
  QJsonArray sel; for (int id : vp_->selection().items) if (const Item* it = sc_->item(id)) sel.append(QJsonObject{{"id", id}, {"name", it->name}, {"faces", it->poly ? it->poly->faceCount() : 0}});
  s.insert("selected", sel);
  QJsonArray meshes; long long total = 0;
  for (auto& [id, it] : sc_->items) if (it.type == ItemType::IMesh && it.poly && sc_->isVisible(it)) { total += it.poly->faceCount();
    meshes.append(QJsonObject{{"id", id}, {"name", it.name}, {"faces", it.poly->faceCount()}, {"ref", it.raw.value("tref").toBool()}}); }
  s.insert("meshesShown", meshes); s.insert("facesShown", double(total));
  return s;
}

// the annotation: the window's picture to draw on, a note; saved with the camera and the project's state
void DevSession::annotate() {
  if (!on_) { ui::toast(QObject::tr("Start the session recording first")); return; }
  const QImage shot = grab_ ? grab_() : QImage(); if (shot.isNull()) { ui::toast(QObject::tr("Could not take the window's picture")); return; }
  const QJsonObject st = state();
  QDialog d(win_); d.setWindowTitle(QObject::tr("Annotation")); d.resize(1100, 760);
  auto* v = new QVBoxLayout(&d); auto* canvas = new DrawCanvas(shot); v->addWidget(canvas, 1);
  auto* tools = new QHBoxLayout; v->addLayout(tools);
  tools->addWidget(new QLabel(QObject::tr("Draw with the mouse:")));
  for (auto [c, n] : {std::pair{QColor(0xff, 0x30, 0x30), "Red"}, std::pair{QColor(0xff, 0xd0, 0x20), "Yellow"}, std::pair{QColor(0x30, 0xd0, 0xff), "Cyan"}, std::pair{QColor(0x40, 0xff, 0x60), "Green"}}) {
    auto* b = new QPushButton(QObject::tr(n)); b->setStyleSheet(QString("color:%1").arg(c.name())); QColor cc = c; QObject::connect(b, &QPushButton::clicked, &d, [canvas, cc] { canvas->pen = cc; }); tools->addWidget(b); }
  auto* un = new QPushButton(QObject::tr("Undo stroke")); QObject::connect(un, &QPushButton::clicked, &d, [canvas] { canvas->undo(); }); tools->addWidget(un);
  auto* cl = new QPushButton(QObject::tr("Clear")); QObject::connect(cl, &QPushButton::clicked, &d, [canvas] { canvas->clear(); }); tools->addWidget(cl); tools->addStretch(1);
  v->addWidget(new QLabel(QObject::tr("Note:"))); auto* note = new QPlainTextEdit; note->setFixedHeight(90); note->setPlaceholderText(QObject::tr("What is wrong here, what was expected…")); v->addWidget(note);
  auto* bar = new QHBoxLayout; bar->addStretch(1); auto* cn = new QPushButton(QObject::tr("Cancel")); auto* ok = new QPushButton(QObject::tr("Save annotation")); ok->setDefault(true);
  bar->addWidget(cn); bar->addWidget(ok); v->addLayout(bar);
  QObject::connect(cn, &QPushButton::clicked, &d, &QDialog::reject); QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);
  note->setFocus();
  if (d.exec() != QDialog::Accepted) return;
  const QString png = QString("note_%1.png").arg(++notes_, 3, 10, QChar('0'));
  canvas->result().save(QDir(dir_).filePath(png));
  QJsonObject o = st; o.insert("note", note->toPlainText()); o.insert("picture", png); o.insert("drawn", canvas->drawn());
  event("annotation", o); ui::toast(QObject::tr("Annotation %1 kept").arg(notes_));
}

// (a button or a menu item let go of under the mouse: its text, its name, the start of its tip)
bool DevSession::eventFilter(QObject* o, QEvent* e) {
  // (a key pressed: once per press — which key and what had the focus, a text field taking it shown)
  if (on_ && e->type() == QEvent::KeyPress) { auto* k = static_cast<QKeyEvent*>(e);
    if (!k->isAutoRepeat() && k->timestamp() != lastKeyStamp_) { lastKeyStamp_ = k->timestamp(); QWidget* fw = QApplication::focusWidget();
      event("key", {{"key", QKeySequence(k->keyCombination()).toString()}, {"focus", fw ? QString(fw->metaObject()->className()) + (fw->objectName().isEmpty() ? QString() : " " + fw->objectName()) : QString()}}); } }
  // (a mouse press or the wheel: which of the program's windows got it — what stands over the viewport shows; no position)
  if (on_ && (e->type() == QEvent::MouseButtonPress || e->type() == QEvent::Wheel)) if (auto* w = qobject_cast<QWindow*>(o)) {
    static qint64 lastWheel = -1000; const bool wheel = e->type() == QEvent::Wheel;
    if (!wheel || clock_.elapsed() - lastWheel > 1000 || clock_.elapsed() < lastWheel) {   // (a turn of the wheel: one line)
      if (wheel) lastWheel = clock_.elapsed();
      QString name = QString(w->metaObject()->className()) + (w->objectName().isEmpty() ? QString() : " " + w->objectName());
      for (QWidget* t : QApplication::topLevelWidgets()) if (t->windowHandle() == w) { name = QString(t->metaObject()->className()) + (t->objectName().isEmpty() ? QString() : " " + t->objectName()); break; }
      QJsonObject in{{"window", name}, {"what", wheel ? "wheel" : "press"}, {"mods", int(static_cast<QInputEvent*>(e)->modifiers().toInt() >> 24)}};
      if (!wheel) in.insert("button", int(static_cast<QMouseEvent*>(e)->button()));
      event("input", in);
    } }
  if (on_ && e->type() == QEvent::MouseButtonRelease) {
    if (auto* b = qobject_cast<QAbstractButton*>(o); b && b->isEnabled() && b->underMouse()) event("ui", {{"button", b->text()}, {"name", b->objectName()}, {"tip", b->toolTip().left(70)}, {"checked", b->isCheckable() ? QJsonValue(!b->isChecked()) : QJsonValue()}});
    else if (auto* m = qobject_cast<QMenu*>(o); m && m->activeAction()) event("ui", {{"menu", m->activeAction()->text()}});
  }
  return false;
}

}  // namespace hm
#endif
