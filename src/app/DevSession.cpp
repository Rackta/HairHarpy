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
#include <QAbstractSlider>
#include <QComboBox>
#include <QPointer>
#include <QTabletEvent>
#include <QStorageInfo>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif
#include <QThread>
#include <cmath>
#include <cstring>
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
#include <thread>
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
  explicit DrawCanvas(const QImage& img) : img_(img) { img_.setDevicePixelRatio(1.0); setMinimumSize(480, 300); setCursor(Qt::CrossCursor); }   // (the strokes kept in the picture's pixels: with the interface scaled (Display → Interface, 90 %) the window's picture came with a ratio of 0.9 and the saved strokes were painted 0.9 times as far from its corner — off where they were drawn, 2026-11-10)
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

#if defined(_WIN32) && defined(_M_X64)
// ---- where the program stands while its window is stopped ----
// The main thread is held for a moment, its registers and a copy of its stack are taken, and it is let go; the stack is then
// unwound on the copy. Nothing that takes a lock is called while the thread stands — it could be the one holding that lock.
struct WalkOut { DWORD64 pcs[48]; int n = 0; };
static inline void fixReg(DWORD64& r, DWORD64 oldLo, DWORD64 oldHi, DWORD64 delta) { if (r >= oldLo && r < oldHi) r += delta; }
static void fixRegs(CONTEXT& c, DWORD64 oldLo, DWORD64 oldHi, DWORD64 delta) {
  fixReg(c.Rsp, oldLo, oldHi, delta); fixReg(c.Rbp, oldLo, oldHi, delta); fixReg(c.Rbx, oldLo, oldHi, delta); fixReg(c.Rsi, oldLo, oldHi, delta); fixReg(c.Rdi, oldLo, oldHi, delta);
  fixReg(c.R12, oldLo, oldHi, delta); fixReg(c.R13, oldLo, oldHi, delta); fixReg(c.R14, oldLo, oldHi, delta); fixReg(c.R15, oldLo, oldHi, delta);
}
static bool unwindStep(CONTEXT* c, DWORD64 base, PRUNTIME_FUNCTION fn) {
  __try { PVOID hd = nullptr; DWORD64 est = 0; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c->Rip, fn, c, &hd, &est, nullptr); return true; }
  __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// (the copy stands in for the stack: every register that pointed into the stack points into the copy at the same place)
static void walkCopy(CONTEXT c, const char* buf, size_t size, DWORD64 oldLo, WalkOut& out) {
  const DWORD64 oldHi = oldLo + size, lo = reinterpret_cast<DWORD64>(buf), hi = lo + size, delta = lo - oldLo;
  fixRegs(c, oldLo, oldHi, delta);
  while (out.n < 48 && c.Rip) {
    out.pcs[out.n++] = c.Rip; if (c.Rsp < lo || c.Rsp + 8 > hi) break;
    DWORD64 base = 0; PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
    if (!fn) { c.Rip = *reinterpret_cast<const DWORD64*>(c.Rsp); c.Rsp += 8; }   // (a leaf: its return address on top)
    else if (!unwindStep(&c, base, fn)) break;
    fixRegs(c, oldLo, oldHi, delta);
  }
}
static QString nameOf(DWORD64 pc, bool sym) {
  QString mod; DWORD64 rva = pc; HMODULE h = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(pc), &h) && h) {
    wchar_t p[MAX_PATH]; const DWORD n = GetModuleFileNameW(h, p, MAX_PATH); mod = QFileInfo(QString::fromWCharArray(p, int(n))).completeBaseName(); rva = pc - reinterpret_cast<DWORD64>(h); }
  if (sym) { char raw[sizeof(SYMBOL_INFO) + 512]; SYMBOL_INFO* si = reinterpret_cast<SYMBOL_INFO*>(raw); si->SizeOfStruct = sizeof(SYMBOL_INFO); si->MaxNameLen = 500; DWORD64 disp = 0;
    if (SymFromAddr(GetCurrentProcess(), pc, &disp, si) && si->NameLen) return (mod.isEmpty() ? QString() : mod + "!") + QString::fromLatin1(si->Name, int(si->NameLen)); }
  return (mod.isEmpty() ? QString("?") : mod) + "+0x" + QString::number(rva, 16);
}
#endif

// the program's own warnings (qWarning, Qt's): kept for the record — they come from any thread, so they wait here for the timer
static std::mutex g_warnMx; static std::vector<std::pair<int, QString>> g_warn; static QtMessageHandler g_prevHandler = nullptr; static std::atomic<bool> g_warnOn{false};
static void devMessage(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
  if (g_warnOn.load() && type != QtDebugMsg && type != QtInfoMsg) { std::lock_guard<std::mutex> lk(g_warnMx); if (g_warn.size() < 200) g_warn.push_back({int(type), msg.left(300)}); }
  if (g_prevHandler) g_prevHandler(type, ctx, msg); else { fprintf(stderr, "%s\n", qPrintable(qFormatLogMessage(type, ctx, msg))); fflush(stderr); }
}

DevSession::DevSession(QWidget* window, Viewport* vp, Scene* sc, std::function<QImage()> grabWindow, std::function<QString()> projectName)
    : QObject(window), win_(window), vp_(vp), sc_(sc), grab_(std::move(grabWindow)), name_(std::move(projectName)) {
  readConfig(); g_prevHandler = qInstallMessageHandler(devMessage); vpAlive_ = vp;
  // (what the user does: the tools, the modes, the selections — each once as it changes)
  connect(vp_, &Viewport::toolChanged, this, [this](const QString& t) { if (on_ && t != lastTool_) { lastTool_ = t; event("tool", {{"tool", t}}); } });
  connect(vp_, &Viewport::topoState, this, [this] { if (!on_) return; const QString md = vp_->topoMode, pk = vp_->pickedInfo();
    if (md != lastMode_) { lastMode_ = md; event("mode", {{"mode", md}}); }
    if (pk != lastSel_) { lastSel_ = pk; event("picked", {{"picked", pk}}); } });
  connect(vp_, &Viewport::selectionChanged, this, [this](const std::vector<int>& items) { if (!on_) return; QJsonArray a;
    for (int id : items) if (const Item* it = sc_->item(id)) a.append(QJsonObject{{"id", id}, {"name", it->name}}); event("select", {{"items", a}}); });
  ui::toastHook() = [this](const QString& msg) { if (on_) event("message", {{"text", msg}}); };
  // (a line from anywhere in the program: a stroke, an undo, a save — ui::devLog)
  ui::devHook() = [this](const QString& kind, const QJsonObject& data) { if (on_ && QThread::currentThread() == thread()) event(kind, data); };
  // (the timed scopes: summed by the second; one of 40 ms and more — a line of its own)
  profHook() = [this](const char* what, qint64 ms) { if (!on_ || QThread::currentThread() != thread()) return; Op& o = ops_[QString::fromLatin1(what)]; o.n++; o.total += double(ms); o.max = std::max(o.max, double(ms));
    if (ms >= 40) event("op", {{"name", QString::fromLatin1(what)}, {"ms", double(ms)}}); };
  // (the frames: how many each second, what they cost; a slow one — a line of its own, not more often than twice a second)
  vp_->frameHook = [this](double ms) { if (!on_ || QThread::currentThread() != thread()) return; sec_.frames++; sec_.frameSum += ms; sec_.frameMax = std::max(sec_.frameMax, ms);
    if (pendingInputAt_) { const double lat = double(clock_.elapsed() - pendingInputAt_); pendingInputAt_ = 0; sec_.latN++; sec_.latSum += lat; sec_.latMax = std::max(sec_.latMax, lat); }   // (the pressed pointer moved: how long until a frame showed it)
    static qint64 slowAt = -10000; if (ms >= 80 && clock_.elapsed() - slowAt > 500) { slowAt = clock_.elapsed(); event("slowframe", {{"ms", ms}, {"after", lastAction_}, {"buttons", int(QApplication::mouseButtons().toInt())}}); } };
  timer_ = new QTimer(this); timer_->setInterval(25); timer_->setTimerType(Qt::PreciseTimer);
  connect(timer_, &QTimer::timeout, this, [this] { tick(); });
#ifdef _WIN32
  setCrashDir(sessionsRoot(false), false); SetUnhandledExceptionFilter(crashFilter); signal(SIGABRT, onAbort);   // (a crash: its dump kept)
#endif
}
DevSession::~DevSession() { watch_.store(false); if (watcher_.joinable()) watcher_.join(); ui::devHook() = nullptr; ui::toastHook() = nullptr; profHook() = nullptr; if (vpAlive_) vp_->frameHook = nullptr; }

// The recorder's settings: <Documents>/HairHarpy Sessions/recorder.json (none by default — the recording is started by hand, its
// folder is in Documents, no pictures of the window are taken)
QString DevSession::defaultRoot() {
  QDir docs(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
  if (!QFileInfo(docs.filePath("HairHarpy Sessions")).exists() && QFileInfo(docs.filePath("HairMeshCreator Sessions")).isDir()) docs.rename("HairMeshCreator Sessions", "HairHarpy Sessions");   // (the name before 2.0: its records kept)
  return docs.filePath("HairHarpy Sessions");
}
void DevSession::readConfig() {
  QFile f(QDir(defaultRoot()).filePath("recorder.json")); if (!f.open(QIODevice::ReadOnly)) return;
  const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object(); if (o.isEmpty()) return;
  cfg_.root = o.value("root").toString(); cfg_.autoRec = o.value("auto").toBool(); cfg_.frames = o.value("frames").toBool();
  cfg_.pointer = o.value("pointer").toBool(true); cfg_.stacks = o.value("stacks").toBool(true);
  cfg_.frameEvery = std::clamp(o.value("frameEvery").toDouble(2), 0.25, 60.0); cfg_.frameWidth = std::clamp(o.value("frameWidth").toInt(1920), 320, 7680);
}

// the settings file's folder, when it names one that can be written to; else a Sessions folder beside the program — a copy handed to
// a tester: the records there, sent back with it; else, or where the program's folder can't be written to, in Documents
QString DevSession::sessionsRoot(bool make) const {
  QString root = defaultRoot();
  auto writable = [](const QString& d) { if (!QFileInfo(d).isDir()) return false; QFile probe(QDir(d).filePath(".write_test")); if (!probe.open(QIODevice::WriteOnly)) return false; probe.close(); probe.remove(); return true; };
  { const QString local = QDir(QCoreApplication::applicationDirPath()).filePath("Sessions"); if (writable(local)) root = local; }
  if (!cfg_.root.isEmpty()) { if (make) QDir().mkpath(cfg_.root); if (writable(cfg_.root)) root = cfg_.root; }
  if (make) QDir().mkpath(root);
  return root;
}

void DevSession::start() {
  if (on_) return;
  readConfig(); lowDisk_ = false; diskAt_ = -100000; sysAt_ = 0; cpuAll_ = 0; pendingInputAt_ = 0; quietSince_ = 0; warns_ = 0; lastWarn_.clear();
  { std::lock_guard<std::mutex> lk(g_warnMx); g_warn.clear(); } g_warnOn.store(true);
  dir_ = QDir(sessionsRoot()).filePath("session_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss")); QDir().mkpath(dir_);
  if (cfg_.frames) QDir(dir_).mkpath("frames");
  file_ = QDir(dir_).filePath("session.jsonl"); out_.setFileName(file_); out_.open(QIODevice::Append | QIODevice::Text);
  outImp_.setFileName(QDir(dir_).filePath("important.jsonl")); outImp_.open(QIODevice::Append | QIODevice::Text);
  notes_ = 0; frames_ = 0; on_ = true; clock_.start(); tick_.start(); rounds_ = 0; secAt_ = 0; frameShotAt_ = -100000; lastInputMs_ = 0; sec_ = {}; ops_.clear(); ptr_ = {}; gest_ = {}; keyDown_.clear();
  timer_->start(); qApp->installEventFilter(this);
  lastTool_ = vp_->tool(); lastMode_ = vp_->topoMode; lastSel_ = vp_->pickedInfo();
  statePending_ = stateOut_ = QJsonObject(); { QJsonObject s = stateHook ? stateHook() : QJsonObject(); s.insert("room", vp_->room()); statePending_ = stateOut_ = s; }
  { QJsonObject s = state();   // (the system: what a problem on one of them depends on; the clock: the record laid beside other programs' records)
    s.insert("system", QJsonObject{{"os", QSysInfo::prettyProductName()}, {"qt", qVersion()}, {"platform", QGuiApplication::platformName()},
                                   {"session", QString::fromLocal8Bit(qgetenv("XDG_SESSION_TYPE"))}, {"desktop", QString::fromLocal8Bit(qgetenv("XDG_CURRENT_DESKTOP"))}});
    s.insert("epochMs", double(QDateTime::currentMSecsSinceEpoch())); s.insert("schema", 2);
    s.insert("build", QFileInfo(QCoreApplication::applicationFilePath()).lastModified().toString(Qt::ISODate));
    s.insert("window", QJsonObject{{"w", win_->width()}, {"h", win_->height()}, {"dpr", win_->devicePixelRatioF()}});
    s.insert("recorder", QJsonObject{{"auto", cfg_.autoRec}, {"frames", cfg_.frames}, {"frameEvery", cfg_.frameEvery}, {"pointer", cfg_.pointer}, {"stacks", cfg_.stacks}});
    s.insert("ext", stateOut_);
    event("start", s); }
  if (catalogHook) event("catalog", catalogHook());
#ifdef _WIN32
  setCrashDir(dir_, true); g_t0 = GetTickCount64() - ULONGLONG(clock_.elapsed());
#if defined(_M_X64)
  if (cfg_.stacks && !watcher_.joinable()) { beat_.store(0); watch_.store(true); const unsigned long tid = GetCurrentThreadId(); watcher_ = std::thread([this, tid] { watchLoop(tid); }); }
#endif
#endif
  ui::toast(QObject::tr("Session recording — into %1").arg(QDir::toNativeSeparators(dir_))); if (changed) changed();
}

void DevSession::stop(bool quiet) {
  if (!on_) return;
  g_warnOn.store(false); drainWarnings(); flushPtr(); flushSecond();
  event("stop", state()); on_ = false; timer_->stop(); qApp->removeEventFilter(this); out_.close(); outImp_.close();
  watch_.store(false); if (watcher_.joinable()) watcher_.join();
#ifdef _WIN32
  setCrashDir(sessionsRoot(), false);
#endif
  if (quiet) { if (changed) changed(); return; }
  QApplication::clipboard()->setText(QDir::toNativeSeparators(dir_));
  ui::toast(QObject::tr("Session recorded: %1 (the folder's path copied)").arg(QDir::toNativeSeparators(dir_))); if (changed) changed();
}

void DevSession::event(const QString& kind, const QJsonObject& data) {
  if (!on_) return;
  if (kind == "action") lastAction_ = data.value("id").toString();
#ifdef _WIN32
  if (kind != "ptr" && kind != "perf" && kind != "ops") { QString w = kind + " " + (data.contains("id") ? data.value("id").toVariant().toString() : data.contains("tool") ? data.value("tool").toString() : data.contains("picked") ? data.value("picked").toString() : data.value("button").toString());   // (a crash line: what was done last)
    w.replace('"', "'").replace('\\', '/'); strncpy(g_lastAct, w.toUtf8().left(90).constData(), sizeof g_lastAct - 1); }
#endif
  QJsonObject o = data; if (!o.contains("t")) o.insert("t", double(clock_.elapsed()) / 1000.0); o.insert("kind", kind);
  // (the important ones — a long stall, an annotation, a failure, the start and the end — marked, and kept in important.jsonl
  // too: read first, the rest only round them)
  const QString txt = data.value("text").toString().toLower();
  const bool important = (kind == "stall" && data.value("ms").toDouble() >= 300) || kind == "annotation" || kind == "start" || kind == "stop" || kind == "error"
                         || (kind == "message" && (txt.contains("fail") || txt.contains("error") || txt.contains("could not") || txt.contains("can't") || txt.contains("no such")));
  if (important) o.insert("important", true);
  const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + "\n";
  if (out_.isOpen()) { out_.write(line); out_.flush(); }
  if (important && outImp_.isOpen()) { outImp_.write(line); outImp_.flush(); }
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
  if (stateHook) s.insert("ext", stateHook());
  return s;
}

// ---- the rounds of the timer ----
void DevSession::tick() {
  if (!on_) return;
  const qint64 gap = tick_.restart(), now = clock_.elapsed(); beat_.store(now);
  // (the window stopped: a tick late by more than 0.12 s — how long, after what, and where the program stood meanwhile)
  if (gap > 120) {
    QJsonObject st{{"ms", double(gap)}, {"after", lastAction_}, {"idle", double(now - lastInputMs_ - gap) / 1000.0}, {"active", QGuiApplication::applicationState() == Qt::ApplicationActive}, {"minimized", win_ && win_->isMinimized()},
                   {"buttons", int(QApplication::mouseButtons().toInt())}, {"t", double(now - gap) / 1000.0}};
    { std::lock_guard<std::mutex> lk(stackMx_); QJsonArray shots; std::map<QString, int> top;
      for (const StackShot& s : stacks_) { if (s.t < now - gap - 60) continue; QJsonArray fr; for (const QString& f : s.frames) fr.append(f); shots.append(QJsonObject{{"t", double(s.t) / 1000.0}, {"frames", fr}}); }
      stacks_.clear(); if (!shots.isEmpty()) st.insert("stacks", shots); }
    event("stall", st);
    if (cfg_.frames && gap >= 300) maybeFrame(true);            // (what the window showed when it came back)
  }
  if (wheelOpen_ && now - wheelAt_ > 400) { wheelOpen_ = false; event("wheel", {{"window", wheelWin_}, {"delta", wheelSum_}}); wheelSum_ = 0; }
  if (++rounds_ % 8 == 0) pollState();
  drainWarnings();
  if (now - sysAt_ >= 2000) { sysAt_ = now; sysSample(); }
  // (the disk the record goes to: under 2 GB left — the window's pictures stop, the rest is small and goes on)
  if (now - diskAt_ >= 30000) { diskAt_ = now; const qint64 free = QStorageInfo(dir_).bytesAvailable();
    if (free >= 0 && free < (qint64(2) << 30) && !lowDisk_) { lowDisk_ = true; cfg_.frames = false; event("error", {{"text", "disk space low: the window's pictures are off"}, {"freeMB", double(free >> 20)}});
      ui::toast(QObject::tr("Session record: the disk is almost full — the window's pictures are off")); } }
  if (now - secAt_ >= 1000) { secAt_ = now; flushSecond(); }
  maybeFrame();
}

// the machine's load every 2 s: the processor as a whole, this program's share of it (of all cores), its memory — a stall with the
// processor busy and the program's share small was the machine's, not the program's
void DevSession::sysSample() {
#ifdef _WIN32
  FILETIME i, k, u, c, e, pk, pu; if (!GetSystemTimes(&i, &k, &u) || !GetProcessTimes(GetCurrentProcess(), &c, &e, &pk, &pu)) return;
  auto v = [](const FILETIME& f) { return (quint64(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
  const quint64 idle = v(i), all = v(k) + v(u), own = v(pk) + v(pu);
  PROCESS_MEMORY_COUNTERS pm; pm.cb = sizeof pm; double mem = 0; if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof pm)) mem = double(pm.WorkingSetSize) / 1048576.0;
  if (cpuAll_ && all > cpuAll_) { const double d = double(all - cpuAll_);
    event("sys", {{"cpu", std::round((1.0 - double(idle - cpuIdle_) / d) * 100)}, {"own", std::round(double(own - cpuOwn_) / d * 100)}, {"memMB", std::round(mem)}}); }
  cpuIdle_ = idle; cpuAll_ = all; cpuOwn_ = own;
#endif
}
void DevSession::drainWarnings() {
  std::vector<std::pair<int, QString>> w; { std::lock_guard<std::mutex> lk(g_warnMx); w.swap(g_warn); }
  for (auto& [lv, text] : w) { if (text == lastWarn_ || warns_ >= 2000) continue; lastWarn_ = text; warns_++; event("warn", {{"level", lv}, {"text", text}}); }   // (the same line again and again: once)
}

// (a value is written when it has changed and stayed so for two looks — a slider dragged leaves one line, not a hundred)
void DevSession::pollState() {
  QJsonObject cur = stateHook ? stateHook() : QJsonObject(); cur.insert("room", vp_->room());
  QJsonObject diff;
  for (auto it = cur.begin(); it != cur.end(); ++it) { const QJsonValue pend = statePending_.value(it.key()); if (pend != it.value()) continue; if (stateOut_.value(it.key()) != it.value()) diff.insert(it.key(), it.value()); }
  statePending_ = cur;
  if (diff.isEmpty()) return; for (auto it = diff.begin(); it != diff.end(); ++it) stateOut_.insert(it.key(), it.value());
  event("state", {{"set", diff}});
}

void DevSession::flushSecond() {
  flushPtr();
  if (sec_.frames) { QJsonObject p{{"frames", sec_.frames}, {"avgMs", std::round(sec_.frameSum / sec_.frames * 10) / 10}, {"maxMs", std::round(sec_.frameMax * 10) / 10}, {"hover", std::round(hoverDist_)}};
    if (sec_.latN) { p.insert("latN", sec_.latN); p.insert("latMs", std::round(sec_.latSum / sec_.latN)); p.insert("latMax", std::round(sec_.latMax)); }
    event("perf", p); }
  sec_ = {}; hoverDist_ = 0;
  if (!ops_.empty()) { QJsonObject o; for (auto& [n, v] : ops_) o.insert(n, QJsonArray{v.n, v.total, v.max}); ops_.clear(); event("ops", {{"ops", o}}); }
}

// a picture of the window for the record: after something was done, in a quiet moment (no button held, a quarter of a second
// after the last input — or 5 s at the latest), not more often than the settings say
void DevSession::maybeFrame(bool force) {
  if (!cfg_.frames || !grab_) return; const qint64 now = clock_.elapsed();
  if (!force) { if (now - frameShotAt_ < std::max(qint64(cfg_.frameEvery * 1000), grabMs_ * 33) || lastInputMs_ <= frameShotAt_) return;   // (a slow picture: taken more seldom — at most 3 % of the time)
    if (QApplication::mouseButtons() != Qt::NoButton || QApplication::activeModalWidget() || QApplication::activePopupWidget()) return;
    if (now - std::max(lastInputMs_, quietSince_) < 500 && now - frameShotAt_ < 6000) return;   // (half a second after the hands stopped: the picture never falls on the start of the next stroke)
    if (QGuiApplication::applicationState() != Qt::ApplicationActive) return; }
  else if (now - frameShotAt_ < 700) return;
  frameShotAt_ = now; QElapsedTimer t; t.start(); QImage shot = grab_(); if (shot.isNull()) return; const qint64 grabMs = t.elapsed(); grabMs_ = grabMs;
  const QString name = QString("frames/f_%1.jpg").arg(++frames_, 5, 10, QChar('0')); const QString path = QDir(dir_).filePath(name); const int w = cfg_.frameWidth;
  std::thread([shot, path, w] { QImage s = shot.width() > w ? shot.scaledToWidth(w, Qt::SmoothTransformation) : shot; s.convertToFormat(QImage::Format_RGB888).save(path, "JPG", 82); }).detach();
  event("frame", {{"file", name}, {"grabMs", double(grabMs)}, {"t", double(now) / 1000.0}});
  tick_.restart();                                               // (the picture's own cost is not a stall of the program)
}

// ---- the pointer's path: a place every 25 ms while a button is held, every 0.2 s while none is; a line of up to 80 places ----
void DevSession::ptrSample(const QPointF& g, int buttons, double pressure, bool force) {
  if (!cfg_.pointer) return; const qint64 now = clock_.elapsed();
  if (!force) { if (now - ptrAt_ < (buttons ? 25 : 200)) return; if (!buttons && std::hypot(g.x() - ptrLast_.x(), g.y() - ptrLast_.y()) < 3) return; }
  if (ptr_.isEmpty()) ptrT0_ = now; ptrAt_ = now; ptrLast_ = g; const QPoint p = win_->mapFromGlobal(g.toPoint());
  QJsonArray a{int(now - ptrT0_), p.x(), p.y(), buttons}; if (pressure >= 0) a.append(int(pressure * 1000)); ptr_.append(a);
  if (ptr_.size() >= 80) flushPtr();
}
void DevSession::flushPtr() { if (ptr_.isEmpty()) return; const QJsonArray pts = ptr_; ptr_ = QJsonArray(); event("ptr", {{"t", double(ptrT0_) / 1000.0}, {"pts", pts}}); }

// the part of the window a widget belongs to: its panel's name, the names of what holds it — "paintLeft", "dock:Layers", "hm::ui::Pie"
QString DevSession::zoneOf(QWidget* w) {
  QStringList parts;
  for (QWidget* p = w; p && parts.size() < 4; p = p->parentWidget()) {
    const QString dp = p->property("dpTitle").toString();
    if (!dp.isEmpty()) { parts.prepend("dock:" + dp); break; }
    if (p->isWindow()) { if (!p->inherits("hm::MainWindow")) parts.prepend(QString(p->metaObject()->className()) + (p->objectName().isEmpty() ? QString() : ":" + p->objectName())); break; }
    const QString n = p->objectName(); if (!n.isEmpty() && !n.startsWith("qt_") && (parts.isEmpty() || parts.first() != n)) parts.prepend(n);
  }
  return parts.join("/");
}

// the annotation: the window's picture to draw on, a note and its tags; saved with the camera and the project's state
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
  // (what kind of a note it is — one click instead of words: a note with no text still says what it is about)
  auto* tagRow = new QHBoxLayout; v->addLayout(tagRow); tagRow->addWidget(new QLabel(QObject::tr("What is it:"))); std::vector<std::pair<QString, QPushButton*>> tagBtns;
  for (auto [key, n, tip] : {std::tuple{"lag", "Slow / freezes", "The program was slow or stopped here"}, std::tuple{"wrong", "Wrong result", "It did not do what was expected"}, std::tuple{"artifact", "Artifact", "Something looks broken on the screen or on the model"},
                             std::tuple{"awkward", "Inconvenient", "It works, but it is awkward: too many steps, hard to find, in the way"}, std::tuple{"missing", "Missing", "A function that is needed here and is not there"},
                             std::tuple{"idea", "Idea", "A thought on how it could be"}, std::tuple{"good", "Good", "This is convenient — keep it"}}) {
    auto* b = new QPushButton(QObject::tr(n)); b->setCheckable(true); b->setToolTip(QObject::tr(tip)); b->setFocusPolicy(Qt::NoFocus); tagRow->addWidget(b); tagBtns.push_back({QString(key), b}); }
  tagRow->addStretch(1);
  v->addWidget(new QLabel(QObject::tr("Note:"))); auto* note = new QPlainTextEdit; note->setFixedHeight(90); note->setPlaceholderText(QObject::tr("What is wrong here, what was expected…")); v->addWidget(note);
  auto* bar = new QHBoxLayout; bar->addStretch(1); auto* cn = new QPushButton(QObject::tr("Cancel")); auto* ok = new QPushButton(QObject::tr("Save annotation")); ok->setDefault(true);
  bar->addWidget(cn); bar->addWidget(ok); v->addLayout(bar);
  QObject::connect(cn, &QPushButton::clicked, &d, &QDialog::reject); QObject::connect(ok, &QPushButton::clicked, &d, &QDialog::accept);
  note->setFocus();
  const qint64 at = clock_.elapsed();
  const bool kept = d.exec() == QDialog::Accepted; tick_.restart();
  if (!kept) return;
  const QString png = QString("note_%1.png").arg(++notes_, 3, 10, QChar('0'));
  { const QImage pic = canvas->result(); const QString path = QDir(dir_).filePath(png); std::thread([pic, path] { pic.save(path); }).detach(); }   // (the PNG written aside: ~0.3 s for a big window)
  QJsonObject o = st; o.insert("note", note->toPlainText()); o.insert("picture", png); o.insert("drawn", canvas->drawn());
  { QJsonArray tags; for (auto& [k, b] : tagBtns) if (b->isChecked()) tags.append(k); o.insert("tags", tags); }
  o.insert("t", double(at) / 1000.0);                              // (the moment the note was asked for, not the moment it was written down)
  event("annotation", o); ui::toast(QObject::tr("Annotation %1 kept").arg(notes_));
}

bool DevSession::eventFilter(QObject* o, QEvent* e) {
  if (!on_) return false;
  const QEvent::Type ty = e->type(); const qint64 now = clock_.elapsed();
  if (ty == QEvent::KeyPress || ty == QEvent::MouseButtonPress || ty == QEvent::Wheel || ty == QEvent::TabletPress) lastInputMs_ = now;
  if (ty == QEvent::MouseButtonRelease || ty == QEvent::TabletRelease || ty == QEvent::KeyRelease) quietSince_ = now;   // (the hands let go: the window's picture waits half a second more)
  // (a key pressed: once per press — which key and what had the focus, a text field taking it shown; let go after a quarter of a
  // second or more — how long it was held)
  if (ty == QEvent::KeyPress) { auto* k = static_cast<QKeyEvent*>(e);
    if (!k->isAutoRepeat() && k->timestamp() != lastKeyStamp_) { lastKeyStamp_ = k->timestamp(); QWidget* fw = QApplication::focusWidget(); keyDown_.insert(k->key(), now);
      event("key", {{"key", QKeySequence(k->keyCombination()).toString()}, {"focus", fw ? QString(fw->metaObject()->className()) + (fw->objectName().isEmpty() ? QString() : " " + fw->objectName()) : QString()}}); } }
  else if (ty == QEvent::KeyRelease) { auto* k = static_cast<QKeyEvent*>(e);
    if (!k->isAutoRepeat()) if (auto it = keyDown_.find(k->key()); it != keyDown_.end()) { const qint64 held = now - it.value(); keyDown_.erase(it);
      if (held >= 250) event("keyup", {{"key", QKeySequence(k->key()).toString()}, {"ms", double(held)}}); } }
  // (the mouse, as the program's windows get it: a press — which window and which part of it, where; the moves — the path; the
  // release — the gesture as a whole: how long, how far)
  if (auto* w = qobject_cast<QWindow*>(o)) {
    if (ty == QEvent::MouseButtonPress || ty == QEvent::MouseButtonRelease || ty == QEvent::MouseMove) {
      auto* me = static_cast<QMouseEvent*>(e); const QPointF g = me->globalPosition();
      if (ty == QEvent::MouseButtonPress) {
        QString name = QString(w->metaObject()->className()) + (w->objectName().isEmpty() ? QString() : " " + w->objectName());
        for (QWidget* t : QApplication::topLevelWidgets()) if (t->windowHandle() == w) { name = QString(t->metaObject()->className()) + (t->objectName().isEmpty() ? QString() : " " + t->objectName()); break; }
        const QString zone = name.startsWith("hm::Viewport") ? QStringLiteral("viewport") : zoneOf(QApplication::widgetAt(g.toPoint())); const QPoint p = win_->mapFromGlobal(g.toPoint());
        const int mods = int(me->modifiers().toInt() >> 24);
        const bool penMade = gest_.on && gest_.pen && now - gest_.t0 < 80;   // (the mouse press Qt makes of the pen's: the pen's own line is there already)
        if (!penMade) { event("input", {{"window", name}, {"what", "press"}, {"mods", mods}, {"button", int(me->button())}, {"x", p.x()}, {"y", p.y()}, {"zone", zone}});
          // (a press on a button that is switched off: it gets no event of its own — something was wanted that the program would not do)
          if (auto* db = qobject_cast<QAbstractButton*>(QApplication::widgetAt(g.toPoint())); db && !db->isEnabled())
            event("ui", {{"button", db->text()}, {"name", db->objectName()}, {"tip", db->toolTip().left(70)}, {"disabled", true}, {"zone", zoneOf(db)}}); }
        if (!pendingInputAt_) pendingInputAt_ = now;
        if (!gest_.on) { gest_ = Gesture(); gest_.on = true; gest_.button = int(me->button()); gest_.mods = mods; gest_.t0 = now; gest_.p0 = gest_.last = g; gest_.window = name; gest_.zone = zone; }
        ptrSample(g, int(me->buttons().toInt()), -1, true);
      } else if (ty == QEvent::MouseMove) {
        const double step = std::hypot(g.x() - (gest_.on ? gest_.last.x() : ptrLast_.x()), g.y() - (gest_.on ? gest_.last.y() : ptrLast_.y()));
        if (gest_.on && !gest_.pen) { gest_.dist += step; gest_.last = g; gest_.moves++; } else if (!gest_.on && step < 400) hoverDist_ += step;
        if (me->buttons() != Qt::NoButton && !pendingInputAt_) pendingInputAt_ = now;
        ptrSample(g, int(me->buttons().toInt()), -1, false);
      } else if (gest_.on && (int(me->button()) == gest_.button || me->buttons() == Qt::NoButton)) {
        const QPoint a = win_->mapFromGlobal(gest_.p0.toPoint()), b = win_->mapFromGlobal(g.toPoint());
        event("gesture", {{"window", gest_.window}, {"zone", gest_.zone}, {"button", gest_.button}, {"mods", gest_.mods}, {"ms", double(now - gest_.t0)}, {"dist", std::round(gest_.dist)}, {"moves", gest_.moves},
                          {"x0", a.x()}, {"y0", a.y()}, {"x1", b.x()}, {"y1", b.y()}, {"t", double(gest_.t0) / 1000.0}});
        gest_.on = false; ptrSample(g, 0, -1, true);
      }
    } else if (ty == QEvent::Wheel) {                            // (a turn of the wheel: one line when it stops — which window, how far)
      auto* we = static_cast<QWheelEvent*>(e); QString name = QString(w->metaObject()->className());
      for (QWidget* t : QApplication::topLevelWidgets()) if (t->windowHandle() == w) { name = QString(t->metaObject()->className()); break; }
      wheelSum_ += we->angleDelta().y() / 120.0; wheelAt_ = now; wheelOpen_ = true; wheelWin_ = name;
    } else if (ty == QEvent::TabletPress || ty == QEvent::TabletMove || ty == QEvent::TabletRelease) {   // (the pen: its pressure along the path)
      // (a window that takes the pen itself — the texture window — gets no mouse events of it: the pen's press, path and release
      // are the gesture then; where Qt makes mouse events of the pen as well, the pen's lines stand and the mouse's are passed over)
      auto* te = static_cast<QTabletEvent*>(e); const QPointF g = te->globalPosition(); const bool tip = ty != QEvent::TabletRelease && (te->pressure() > 0 || te->buttons() != Qt::NoButton);
      if (ty == QEvent::TabletPress && !gest_.on) {
        QString name = QString(w->metaObject()->className());
        for (QWidget* t : QApplication::topLevelWidgets()) if (t->windowHandle() == w) { name = QString(t->metaObject()->className()) + (t->objectName().isEmpty() ? QString() : " " + t->objectName()); break; }
        const QString zone = name.startsWith("hm::Viewport") ? QStringLiteral("viewport") : zoneOf(QApplication::widgetAt(g.toPoint())); const QPoint p = win_->mapFromGlobal(g.toPoint()); const int mods = int(te->modifiers().toInt() >> 24);
        event("input", {{"window", name}, {"what", "press"}, {"mods", mods}, {"button", int(te->button())}, {"x", p.x()}, {"y", p.y()}, {"zone", zone}, {"pen", true}});
        gest_ = Gesture(); gest_.on = true; gest_.pen = true; gest_.button = int(te->button()); gest_.mods = mods; gest_.t0 = now; gest_.p0 = gest_.last = g; gest_.window = name; gest_.zone = zone;
      } else if (ty == QEvent::TabletMove && gest_.on && gest_.pen) {
        gest_.dist += std::hypot(g.x() - gest_.last.x(), g.y() - gest_.last.y()); gest_.last = g; gest_.moves++; gest_.pMax = std::max(gest_.pMax, double(te->pressure())); gest_.pSum += te->pressure(); gest_.pN++;
      } else if (ty == QEvent::TabletRelease && gest_.on && gest_.pen) {
        const QPoint a = win_->mapFromGlobal(gest_.p0.toPoint()), b = win_->mapFromGlobal(g.toPoint());
        event("gesture", {{"window", gest_.window}, {"zone", gest_.zone}, {"button", gest_.button}, {"mods", gest_.mods}, {"ms", double(now - gest_.t0)}, {"dist", std::round(gest_.dist)}, {"moves", gest_.moves},
                          {"x0", a.x()}, {"y0", a.y()}, {"x1", b.x()}, {"y1", b.y()}, {"pen", true}, {"pAvg", gest_.pN ? std::round(gest_.pSum / gest_.pN * 1000) / 1000 : 0.0}, {"pMax", std::round(gest_.pMax * 1000) / 1000},
                          {"t", double(gest_.t0) / 1000.0}});
        gest_.on = false;
      }
      if (tip && !pendingInputAt_) pendingInputAt_ = now;
      ptrSample(g, tip ? 1 : 0, te->pressure(), ty != QEvent::TabletMove);
    }
    return false;
  }
  // (what was pressed in the interface: a button or a menu item let go of under the mouse — its text, its name, the start of its tip,
  // the part of the window; a slider let go — its value; a list — what was picked in it)
  if (ty == QEvent::MouseButtonRelease) {
    if (auto* b = qobject_cast<QAbstractButton*>(o); b && b->isEnabled() && b->underMouse())
      event("ui", {{"button", b->text()}, {"name", b->objectName()}, {"tip", b->toolTip().left(70)}, {"checked", b->isCheckable() ? QJsonValue(!b->isChecked()) : QJsonValue()}, {"zone", zoneOf(b)}});
    else if (auto* m = qobject_cast<QMenu*>(o); m && m->activeAction()) {
      QWidget* owner = m->parentWidget(); while (auto* pm = qobject_cast<QMenu*>(owner)) owner = pm->parentWidget();
      event("ui", {{"menu", m->activeAction()->text()}, {"of", owner ? owner->toolTip().left(50) : QString()}, {"zone", owner ? zoneOf(owner) : QString()}}); }
    else if (auto* sl = qobject_cast<QAbstractSlider*>(o)) { QPointer<QAbstractSlider> p(sl); QString label; if (QWidget* par = sl->parentWidget()) if (auto* l = par->findChild<QLabel*>()) label = l->text();
      if (label.isEmpty()) label = sl->toolTip().left(40);
      QTimer::singleShot(0, this, [this, p, label] { if (p && on_) event("ui", {{"slider", label}, {"value", p->value()}, {"zone", zoneOf(p)}}); }); }
    else if (auto* wd = qobject_cast<QWidget*>(o); wd && wd->window() && wd->window()->inherits("QComboBoxPrivateContainer")) {
      if (auto* cb = qobject_cast<QComboBox*>(wd->window()->parentWidget())) { QPointer<QComboBox> p(cb);
        QTimer::singleShot(0, this, [this, p] { if (p && on_) event("ui", {{"combo", p->toolTip().left(50)}, {"text", p->currentText()}, {"zone", zoneOf(p)}}); }); } }
  }
  // (a tip shown: the pointer stood on a button long enough to ask what it is)
  else if (ty == QEvent::ToolTip) { static QPointer<QObject> lastTip; if (auto* b = qobject_cast<QAbstractButton*>(o); b && lastTip != o) { lastTip = o; event("tip", {{"button", b->text()}, {"tip", b->toolTip().left(50)}, {"zone", zoneOf(b)}}); } }
  return false;
}

// the watcher: the main thread's heartbeat seen from a thread of its own; late by more than 0.11 s — the main thread's stack taken
// (up to ten times a stall, 0.1 s apart) and kept for the stall's line
void DevSession::watchLoop(unsigned long mainThread) {
#if defined(_WIN32) && defined(_M_X64)
  HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, mainThread); if (!th) return;
  const size_t cap = 512 * 1024; std::vector<char> buf(cap); bool symTried = false, symOk = false; qint64 lastShot = -1000; int inStall = 0;
  while (watch_.load()) {
    Sleep(20); const qint64 now = clock_.elapsed(), b = beat_.load();
    if (b == 0 || now - b < 110) { inStall = 0; continue; }
    if (inStall >= 10 || now - lastShot < 95) continue;
    alignas(16) CONTEXT ctx; std::memset(&ctx, 0, sizeof ctx); ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER; size_t got = 0; DWORD64 lo = 0; bool ok = false;
    if (SuspendThread(th) == DWORD(-1)) continue;
    if (GetThreadContext(th, &ctx)) { lo = ctx.Rsp;
      // (how much of the stack there is above its top: asked of the memory itself — no lock is taken for it)
      MEMORY_BASIC_INFORMATION mi; size_t room = 0; if (VirtualQuery(reinterpret_cast<LPCVOID>(lo), &mi, sizeof mi)) room = size_t(reinterpret_cast<DWORD64>(mi.BaseAddress) + mi.RegionSize - lo);
      got = std::min(cap, room); if (got >= 16) { std::memcpy(buf.data(), reinterpret_cast<const void*>(lo), got); ok = true; } }
    ResumeThread(th);
    inStall++; lastShot = now; if (!ok) continue;
    WalkOut out; walkCopy(ctx, buf.data(), got, lo, out); if (!out.n) continue;
    if (!symTried) { symTried = true; SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME); symOk = SymInitialize(GetCurrentProcess(), nullptr, TRUE); }
    StackShot s; s.t = now; for (int i = 0; i < out.n; i++) s.frames << nameOf(out.pcs[i], symOk);
    std::lock_guard<std::mutex> lk(stackMx_); stacks_.push_back(std::move(s)); if (stacks_.size() > 80) stacks_.erase(stacks_.begin());
  }
  CloseHandle(th);
#else
  Q_UNUSED(mainThread);
#endif
}

}  // namespace hm
#endif
