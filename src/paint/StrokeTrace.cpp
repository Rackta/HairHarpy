#include "paint/StrokeTrace.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <mutex>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hm::strace {

namespace {
thread_local int t_mute = 0;
struct State {
  std::mutex m; bool on = false; qint64 checkedAt = 0, flushedAt = 0; QFile f; QString session; long long qpf = 0, qpc0 = 0; QByteArray buf;
  ~State() { if (!buf.isEmpty() && f.isOpen()) { f.write(buf); f.flush(); } }   // (the program closing: the last lines kept)
};
State& st() { static State s; return s; }
double nowSec(const State& s) {
#ifdef _WIN32
  LARGE_INTEGER q; QueryPerformanceCounter(&q); return s.qpf > 0 ? double(q.QuadPart - s.qpc0) / double(s.qpf) : 0.0;
#else
  (void)s; return 0.0;
#endif
}
void flushLocked(State& s) { if (!s.buf.isEmpty() && s.f.isOpen()) { s.f.write(s.buf); s.f.flush(); } s.buf.clear(); s.flushedAt = QDateTime::currentMSecsSinceEpoch(); }
void check(State& s) {
  const QString hs = QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath("HairHarpy Sessions/stroke_rec.json");
  QFile f(hs); QJsonObject o; if (f.open(QIODevice::ReadOnly)) o = QJsonDocument::fromJson(f.readAll()).object();
  const QString ses = o.value("session").toString();
  const bool fresh = !ses.isEmpty() && QDateTime::currentMSecsSinceEpoch() - qint64(o.value("hb").toDouble()) < 8000 && QDir(ses).exists();
  if (fresh && (!s.on || ses != s.session)) {
    flushLocked(s); s.f.close(); s.f.setFileName(QDir(ses).filePath("hairharpy_trace.jsonl"));
    if (!s.f.open(QIODevice::WriteOnly | QIODevice::Append)) { s.on = false; return; }
    s.session = ses; s.qpf = qint64(o.value("qpf").toDouble()); s.qpc0 = qint64(o.value("qpc0").toDouble()); s.on = true;
    s.buf += QByteArray("{\"t\":") + QByteArray::number(nowSec(s), 'f', 6) + ",\"e\":\"hello\",\"pid\":" + QByteArray::number(QCoreApplication::applicationPid()) +
             ",\"app\":\"HairHarpy\",\"version\":\"" + QCoreApplication::applicationVersion().toUtf8() + "\"}\n";
  } else if (!fresh && s.on) { flushLocked(s); s.f.close(); s.on = false; s.session.clear(); }
}
}  // namespace

bool on() {
  State& s = st(); const qint64 now = QDateTime::currentMSecsSinceEpoch();
  std::lock_guard<std::mutex> l(s.m);
  if (now - s.checkedAt > 1000) { s.checkedAt = now; check(s); }
  if (s.on && now - s.flushedAt > 500) flushLocked(s);
  return s.on;
}

Mute::Mute() { t_mute++; }
Mute::~Mute() { t_mute--; }

void line(const char* kind, std::initializer_list<std::pair<const char*, double>> nums, const QByteArray& extra) {
  if (t_mute) return;
  State& s = st(); std::lock_guard<std::mutex> l(s.m); if (!s.on) return;
  s.buf += "{\"t\":"; s.buf += QByteArray::number(nowSec(s), 'f', 6); s.buf += ",\"e\":\""; s.buf += kind; s.buf += '"';
  for (const auto& [k, v] : nums) { s.buf += ",\""; s.buf += k; s.buf += "\":"; s.buf += QByteArray::number(v, 'g', 7); }
  if (!extra.isEmpty()) { s.buf += ','; s.buf += extra; }
  s.buf += "}\n";
  bool lift = false; for (const auto& [k, v] : nums) if (k[0] == 'k' && !k[1] && v == 2) lift = true;   // (a pen / button lifted: the stroke written out at once)
  if (lift || s.buf.size() > (1 << 18)) flushLocked(s);
}

QByteArray str(const char* key, const QString& v) {
  QByteArray e = QJsonDocument(QJsonObject{{"x", v}}).toJson(QJsonDocument::Compact);   // ({"x":"…"} — the value escaped as JSON does it)
  return QByteArray("\"") + key + "\":" + e.mid(5, e.size() - 6);
}

}  // namespace hm::strace
