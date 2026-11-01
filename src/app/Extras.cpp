#include "app/Extras.h"
#include "ui/Keymap.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QSet>
#include <QStackedWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <deque>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

namespace hm::extras {

namespace {

double rnd() { return QRandomGenerator::global()->generateDouble(); }

// ---------- sounds: a few notes rendered into a WAV and played (the HTML version's Web Audio chimes) ----------
struct Note { double f, at; };
void play(const std::vector<Note>& notes, bool triangle, double gain, double len) {
#ifdef _WIN32
  static QByteArray wav;                                    // (kept while it plays: PlaySound reads it asynchronously)
  const int rate = 22050; double total = 0; for (auto& n : notes) total = std::max(total, n.at + len + 0.05);
  const int N = int(total * rate); std::vector<float> s(size_t(N), 0.f);
  for (auto& n : notes) {
    int s0 = int(n.at * rate);
    for (int i = 0; i < int(len * rate) && s0 + i < N; i++) {
      double t = double(i) / rate, ph = std::fmod(n.f * t, 1.0);
      double w = triangle ? (ph < 0.5 ? 4 * ph - 1 : 3 - 4 * ph) : std::sin(2 * M_PI * n.f * t);
      double env = t < 0.02 ? t / 0.02 * gain : gain * std::exp(std::log(0.001 / gain) * (t - 0.02) / std::max(0.01, len - 0.07));
      s[size_t(s0 + i)] += float(w * env);
    }
  }
  wav.clear(); auto u32 = [&](uint32_t v) { wav.append(reinterpret_cast<const char*>(&v), 4); }; auto u16 = [&](uint16_t v) { wav.append(reinterpret_cast<const char*>(&v), 2); };
  wav.append("RIFF"); u32(uint32_t(36 + N * 2)); wav.append("WAVEfmt "); u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16); wav.append("data"); u32(uint32_t(N * 2));
  for (float v : s) { int16_t x = int16_t(std::clamp(v, -1.f, 1.f) * 32000); wav.append(reinterpret_cast<const char*>(&x), 2); }
  PlaySoundA(wav.constData(), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
#else
  Q_UNUSED(notes); Q_UNUSED(triangle); Q_UNUSED(gain); Q_UNUSED(len);
#endif
}

// ---------- the achievements ----------
struct Ach { const char *id, *name, *desc, *hint, *img; };
const std::vector<Ach>& achs() {
  static const std::vector<Ach> A = {
    {"pet1", "Petting Enthusiast", "Pet the FAQ girl for 5 seconds", "The FAQ girl in the corner likes a little attention…", "faq_hover"},
    {"pet2", "Petting Professional", "Pet her again — 2 of 3", "She would not mind some more of that", "ach_faq2"},
    {"pet3", "Head-Pat Grandmaster", "Pet her a third time — 3 of 3", "Third time's the charm", "ach_faq3"},
    {"chim1", "Caught me!", "Catch the chimera hiding in the toolbar", "Catch the long-eared rascal", "chimera"},
    {"chim2", "Again?!", "Catch the chimera a second time", "She'll be back. Catch her again", "ach_chim2"},
    {"chim3", "Monster Girl Collector", "Catch the chimera three times", "Three catches make a collection", "ach_chim3"},
    {"lucky", "I Wish I Were That Lucky", "Meet the chimera right as the program starts", "Some meet her the very moment they arrive", "ach_lucky"},
    {"twitter", "Maybe Twitter?", "Leave only the X axis of the gizmo active", "The gizmo has three letters. One of them is a social network", "ach_twitter"},
    {"english", "Wow, are you from England!?", "Name an object in the outliner not in English", "Names don't have to be in English, do they?", "ach_english"},
    {"stones", "Time Heist", "Go back with the History slider and change what happened", "Whatever it takes: go back in time and change what happened", "mara_3"},
    {"mara", "And What Was That For?", "Try to win the inspector over", "The inspector looks strict. Maybe a little kindness helps?", "mara_3"},
    {"secret", "Secret Knowledge", "Put the picture together in the FAQ", "Put the picture back together", "ach_krack"}};
  return A;
}
QSet<QString>& got() { static QSet<QString> g; return g; }

// the Steam-like popup (bottom right of the window): one at a time, the others wait their turn
class AchPopup : public QWidget {
public:
  AchPopup() : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
    setAttribute(Qt::WA_ShowWithoutActivating); setAttribute(Qt::WA_TransparentForMouseEvents); setFixedSize(330, 86);
  }
  void push(const Ach& a) { q_.push_back(&a); if (!busy_) next(); }
protected:
  void paintEvent(QPaintEvent*) override {
    if (!cur_) return;
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::SmoothPixmapTransform);
    QLinearGradient g(0, 0, width(), height()); g.setColorAt(0, QColor(0x2a, 0x47, 0x5e)); g.setColorAt(1, QColor(0x1b, 0x28, 0x38));
    p.setPen(QColor(0x3d, 0x6c, 0x8d)); p.setBrush(g); p.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 3, 3);
    QRect ic(12, 11, 64, 64); p.fillRect(ic, Qt::black);
    QPixmap px = ui::image(cur_->img); if (!px.isNull()) { QPixmap s = px.scaled(64, 64, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation); p.drawPixmap(ic, s, QRect((s.width() - 64) / 2, (s.height() - 64) / 2, 64, 64)); }
    p.setPen(QColor(0x66, 0xc0, 0xf4)); p.setBrush(Qt::NoBrush); p.drawRect(ic);
    QFont f = font(); f.setPointSizeF(7.5); f.setLetterSpacing(QFont::PercentageSpacing, 106); p.setFont(f); p.setPen(QColor(0x8f, 0x98, 0xa0)); p.drawText(QRect(88, 12, 230, 14), Qt::AlignLeft, QObject::tr("ACHIEVEMENT UNLOCKED"));
    f.setPointSizeF(11); f.setBold(true); f.setLetterSpacing(QFont::PercentageSpacing, 100); p.setFont(f); p.setPen(Qt::white);
    p.drawText(QRect(88, 27, 232, 22), Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(QObject::tr(cur_->name), Qt::ElideRight, 232));
    f.setPointSizeF(8.5); f.setBold(false); p.setFont(f); p.setPen(QColor(0xac, 0xb2, 0xb8)); p.drawText(QRect(88, 49, 232, 32), Qt::AlignLeft | Qt::TextWordWrap, QObject::tr(cur_->desc));
  }
private:
  void next() {
    if (q_.empty()) { busy_ = false; hide(); return; }
    busy_ = true; cur_ = q_.front(); q_.pop_front(); update();
    QWidget* w = QApplication::activeWindow(); QRect r = w ? w->geometry() : QApplication::primaryScreen()->availableGeometry();
    QPoint end(r.right() - 16 - width(), r.bottom() - 16 - height()), start(end.x(), end.y() + int(height() * 1.4));
    move(start); show(); raise();
    auto* a = new QPropertyAnimation(this, "pos", this); a->setDuration(500); a->setStartValue(start); a->setEndValue(end); a->setEasingCurve(QEasingCurve::OutBack); a->start(QAbstractAnimation::DeleteWhenStopped);
    play({{784, 0}, {1175, 0.12}}, false, 0.12, 0.6);
    QTimer::singleShot(6000, this, [this, start] {
      auto* b = new QPropertyAnimation(this, "pos", this); b->setDuration(400); b->setEndValue(start); b->setEasingCurve(QEasingCurve::InCubic);
      QObject::connect(b, &QPropertyAnimation::finished, this, [this] { hide(); QTimer::singleShot(q_.empty() ? 0 : 700, this, [this] { next(); }); });
      b->start(QAbstractAnimation::DeleteWhenStopped);
    });
  }
  std::deque<const Ach*> q_; const Ach* cur_ = nullptr; bool busy_ = false;
};
AchPopup* popup() { static QPointer<AchPopup> p; if (!p) p = new AchPopup; return p; }

// ---------- the FAQ: data and search ----------
struct Sec { QString id, title, tags, body; QStringList rooms; bool hotkeys = false; };
struct FaqData { std::vector<Sec> secs; std::vector<std::pair<QString, QStringList>> syn; QSet<QString> stop; };
const FaqData& faq() {
  static FaqData D; static bool loaded = false; if (loaded) return D; loaded = true;
  QFile f(":/faq/faq.json"); if (!f.open(QIODevice::ReadOnly)) return D;
  QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
  for (const QJsonValue& v : o.value("sections").toArray()) { QJsonObject s = v.toObject(); Sec x{s.value("id").toString(), s.value("title").toString(), s.value("tags").toString(), s.value("body").toString(), {}, s.value("gen").toString() == "hotkeys"};
    for (const QJsonValue& r : s.value("rooms").toArray()) x.rooms << r.toString(); if (x.rooms.isEmpty()) x.rooms << "hair"; D.secs.push_back(x); }
  for (const QJsonValue& v : o.value("syn").toArray()) { QJsonArray a = v.toArray(); D.syn.push_back({a[0].toString(), a[1].toString().split(' ', Qt::SkipEmptyParts)}); }
  for (const QJsonValue& v : o.value("stop").toArray()) D.stop.insert(v.toString());
  return D;
}
std::vector<const Sec*> faqList(const QString& room) {
  std::vector<const Sec*> l; for (auto& s : faq().secs) if (s.rooms.contains(room)) l.push_back(&s);
  std::stable_sort(l.begin(), l.end(), [](const Sec* a, const Sec* b) { return (a->rooms.size() > 1) < (b->rooms.size() > 1); });
  return l;
}
QStringList faqWords(QString s) {
  s = s.toLower().replace(QChar(0x451), QChar(0x435));      // (ё → е)
  s.replace(QRegularExpression("[^a-z0-9\\x{0430}-\\x{044F}+ \\-]", QRegularExpression::UseUnicodePropertiesOption), " ");
  QStringList out; for (const QString& w : s.split(QRegularExpression("[\\s\\-]+"), Qt::SkipEmptyParts)) if (!faq().stop.contains(w)) out << w;
  return out;
}
std::vector<QStringList> faqQuery(const QString& s) {
  std::vector<QStringList> r;
  for (const QString& w : faqWords(s)) { QStringList v{w}; for (auto& [st, en] : faq().syn) if (w.startsWith(st)) for (const QString& x : en) if (!v.contains(x)) v << x; r.push_back(v); }
  return r;
}
QString faqBody(const Sec& a) {
  QString b = a.body;
  if (a.hotkeys) { QString rows; for (auto& d : ui::Keymap::get().defs()) rows += QString("<tr><td>{k:%1}</td><td>%2</td></tr>").arg(d.id, d.label.toHtmlEscaped());
    b.replace("<tr><td>{k:__ACTIONS__}</td><td>__</td></tr>", rows); }
  static const QRegularExpression K("\\{k:(\\w+)\\}");
  QString out; qsizetype last = 0;
  for (auto it = K.globalMatch(b); it.hasNext();) { auto m = it.next(); out += b.mid(last, m.capturedStart() - last);
    QString key = ui::Keymap::get().key(m.captured(1)); out += "<kbd>" + (key.isEmpty() ? QString("—") : ui::Keymap::label(key).toHtmlEscaped()) + "</kbd>"; last = m.capturedEnd(); }
  return out + b.mid(last);
}
QString faqText(const Sec& a) {
  QString t = faqBody(a);
  t.replace(QRegularExpression("</(h4|li|p|tr)>"), ". "); t.replace("</td>", " — "); t.replace(QRegularExpression("<[^>]+>"), " ");
  t.replace("&lt;", "<").replace("&gt;", ">").replace("&amp;", "&").replace("&quot;", "\"").replace("&#39;", "'");
  t.replace(QRegularExpression("\\s+"), " "); t.replace(QRegularExpression("\\s*—\\s*\\."), "."); t.replace(QRegularExpression("([.:;])\\s*\\."), "\\1");
  return t;
}
bool lev1(const QString& a, const QString& b) {
  if (std::abs(a.size() - b.size()) > 1) return false;
  qsizetype i = 0, j = 0; int d = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] == b[j]) { i++; j++; continue; }
    if (++d > 1) return false;
    if (a.size() == b.size() && i + 1 < a.size() && j + 1 < b.size() && a[i] == b[j + 1] && a[i + 1] == b[j]) { i += 2; j += 2; continue; }
    if (a.size() > b.size()) i++; else if (b.size() > a.size()) j++; else { i++; j++; }
  }
  return d + (a.size() - i) + (b.size() - j) <= 1;
}
bool wordHit(const QString& q, const QString& w) { return q == w || (q.size() >= 3 && (w.startsWith(q) || (q.startsWith(w) && w.size() >= 4))) || (q.size() >= 5 && lev1(q, w)); }
struct Hit { const Sec* a; double score; };
std::vector<Hit> faqSearch(const QString& query, const QString& room) {
  auto qs = faqQuery(query); if (qs.empty()) return {};
  std::vector<Hit> out;
  for (const Sec* a : faqList(room)) {
    QStringList tw = faqWords(a->tags), ti = faqWords(a->title), bw = faqWords(faqText(*a));
    double score = 0; int gotN = 0;
    for (auto& vs : qs) {
      double s = 0;
      for (const QString& q : vs) {
        bool inT = std::any_of(ti.begin(), ti.end(), [&](const QString& w) { return wordHit(q, w); });
        bool inTag = std::any_of(tw.begin(), tw.end(), [&](const QString& w) { return wordHit(q, w); });
        bool inB = std::any_of(bw.begin(), bw.end(), [&](const QString& w) { return wordHit(q, w); });
        s = std::max(s, inTag ? 3 + (inT ? 0.5 : 0) : inT ? 2.5 : inB ? 1 : 0);
      }
      score += s; if (s > 0) gotN++;
    }
    if (gotN) out.push_back({a, score * (double(gotN) / qs.size())});
  }
  std::stable_sort(out.begin(), out.end(), [](const Hit& x, const Hit& y) { return x.score > y.score; });
  std::vector<Hit> r; for (auto& h : out) if (h.score >= 0.4 * out[0].score && r.size() < 6) r.push_back(h);
  return r;
}

const char* CSS = "h3{color:#fff;font-size:15px;margin:4px 0 8px 0} h4{color:#9ec1ff;font-weight:normal;font-size:13px;margin:14px 0 6px 0} p,li{color:#ccc}"
                  " td{padding:2px 10px 2px 0;color:#ccc;vertical-align:top} kbd{background-color:#3a3a3a;color:#fff;font-family:monospace;font-size:11px} b{color:#eee}"
                  " a{color:#e8e8e8;text-decoration:none} .tags{color:#777;font-size:11px} .sn{color:#999;font-size:12px}";

// ---------- the FAQ window ----------
class FaqWindow : public QDialog {
public:
  explicit FaqWindow(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QObject::tr("FAQ")); resize(920, 660); setStyleSheet("QDialog{background:#262626}");
    auto* v = new QVBoxLayout(this); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
    auto* head = new QWidget; auto* hh = new QHBoxLayout(head); hh->setContentsMargins(12, 10, 12, 10); head->setStyleSheet("border-bottom:1px solid #3a3a3a");
    search_ = new QLineEdit; search_->setClearButtonEnabled(true); search_->setStyleSheet("QLineEdit{background:#111;border:1px solid #555;border-radius:5px;color:#fff;padding:6px 9px;font-size:13px} QLineEdit:focus{border-color:#5a8dee}");
    auto* cl = new QPushButton("✕"); cl->setToolTip(QObject::tr("Close (Esc)")); QObject::connect(cl, &QPushButton::clicked, this, &QDialog::close);
    hh->addWidget(search_, 1); hh->addWidget(cl); v->addWidget(head);
    auto* main = new QHBoxLayout; main->setSpacing(0); v->addLayout(main, 1);
    auto* navScroll = new QScrollArea; navScroll->setFixedWidth(210); navScroll->setWidgetResizable(true); navScroll->setStyleSheet("QScrollArea{border:none;border-right:1px solid #3a3a3a;background:#262626}");
    nav_ = new QWidget; navL_ = new QVBoxLayout(nav_); navL_->setContentsMargins(0, 6, 0, 6); navL_->setSpacing(0); navScroll->setWidget(nav_); main->addWidget(navScroll);
    stack_ = new QStackedWidget; main->addWidget(stack_, 1);
    body_ = new QTextBrowser; body_->setOpenLinks(false); body_->setOpenExternalLinks(false); body_->document()->setDefaultStyleSheet(CSS);
    body_->setStyleSheet("QTextBrowser{background:#262626;border:none;padding:6px 10px;color:#ccc}");
    QObject::connect(body_, &QTextBrowser::anchorClicked, this, [this](const QUrl& u) {
      QString s = u.toString(); if (s.startsWith("sec:")) { sec_ = s.mid(4); search_->clear(); show_(); } else if (s.startsWith("http")) QDesktopServices::openUrl(u); });
    stack_->addWidget(body_);
    achPage_ = new QWidget; stack_->addWidget(achPage_);
    QObject::connect(search_, &QLineEdit::textChanged, this, [this] { show_(); });
    QObject::connect(search_, &QLineEdit::returnPressed, this, [this] { auto r = faqSearch(search_->text(), room_); if (!r.empty()) { sec_ = r[0].a->id; search_->clear(); show_(); } });
  }
  // (the section that best answers the name — else its tip; nothing found: the name typed into the search)
  void goTo(const QString& name, const QString& tip) {
    auto r = name.isEmpty() ? std::vector<Hit>{} : faqSearch(name, room_);
    if (r.empty() && !tip.isEmpty()) r = faqSearch(tip.left(80), room_);
    if (!r.empty()) { sec_ = r[0].a->id; search_->clear(); show_(); } else { search_->setText(name); }
  }
  void open(const QString& room) {
    room_ = room; auto L = faqList(room); if (L.empty()) return;
    static QHash<QString, QString> secOf; sec_ = secOf.value(room, L[0]->id); secOfPtr_ = &secOf;
    if (sec_ != "ach" && std::none_of(L.begin(), L.end(), [&](const Sec* s) { return s->id == sec_; })) sec_ = L[0]->id;
    search_->setPlaceholderText(room == "hair" ? QObject::tr("Search help — e.g. \"camera\", \"how to change view\", \"weld curves\", \"export\"…") : QObject::tr("Search the Topology room's help — e.g. \"camera\", \"rooms\", \"references\"…"));
    show_(); show(); raise(); activateWindow(); search_->setFocus(); search_->selectAll();
  }
protected:
  void keyPressEvent(QKeyEvent* e) override { if (e->key() == Qt::Key_Escape) { close(); return; } QDialog::keyPressEvent(e); }
  void closeEvent(QCloseEvent* e) override { pz_ = {}; QDialog::closeEvent(e); }
private:
  struct Puzzle { QString pic; int t[9]; bool solved = false, valid = false; int moves = 0; bool match3 = false; std::vector<int> g; int score = 0, sel = -1; };   // (match3: the other lock — its board, points, the icon picked)
  Puzzle newPuzzle() {
    const char* pics[5] = {"ach_krack", "ach_hyda", "ach_dryad", "puzzle_mara", "puzzle_faq3"};
    Puzzle P; P.valid = true; for (int i = 0; i < 8; i++) P.t[i] = i; P.t[8] = -1; int e = 8, prev = -1;
    auto solved = [&] { for (int i = 0; i < 9; i++) if (P.t[i] != -1 && P.t[i] != i) return false; return true; };
    for (int k = 0; k < 160 || solved(); k++) {
      std::vector<int> nb; for (int c : {e - 3, e + 3, e % 3 ? e - 1 : -1, e % 3 < 2 ? e + 1 : -1}) if (c >= 0 && c < 9 && c != prev) nb.push_back(c);
      int c = nb[size_t(QRandomGenerator::global()->bounded(int(nb.size())))]; P.t[e] = P.t[c]; P.t[c] = -1; prev = e; e = c;
    }
    P.pic = pics[QRandomGenerator::global()->bounded(5)];
    P.match3 = qEnvironmentVariableIsSet("HM_MATCH3") ? qEnvironmentVariable("HM_MATCH3") == "1" : QRandomGenerator::global()->bounded(100) < 30;   // (three times in ten: three in a row instead)
    return P;
  }
  void show_() {
    if (secOfPtr_) (*secOfPtr_)[room_] = sec_;
    // the list: the room, its sections (● — a search hit), 🏆 Achievements
    QLayoutItem* it; while ((it = navL_->takeAt(0))) { if (it->widget()) { it->widget()->hide(); it->widget()->deleteLater(); } delete it; }
    QString q = search_->text().trimmed(); auto res = q.isEmpty() ? std::vector<Hit>{} : faqSearch(q, room_);
    QSet<QString> hits; for (auto& h : res) hits.insert(h.a->id);
    auto* rl = new QLabel(room_ == "hair" ? QObject::tr("✂ Hair room") : QObject::tr("⬢ Topology room")); rl->setStyleSheet("color:#7fd4ff;font-size:11px;padding:2px 12px 6px 12px;border-bottom:1px solid #3a3a3a");
    rl->setToolTip(QObject::tr("The help of the room you are in — the other room has its own (switch rooms next to ⌨ Hotkeys)")); navL_->addWidget(rl);
    auto item = [&](const QString& text, bool on, const QString& extraCss, std::function<void()> click) {
      class It : public QLabel { public: std::function<void()> fn; using QLabel::QLabel; protected: void mousePressEvent(QMouseEvent*) override { if (fn) fn(); } };
      auto* l = new It(text); l->fn = click; l->setCursor(Qt::PointingHandCursor);
      l->setStyleSheet(QString("QLabel{padding:5px 12px;color:%1;background:%2;%3} QLabel:hover{background:#2f2f2f;color:#fff}").arg(on ? "#fff" : "#bbb", on ? "#33415e" : "transparent", extraCss));
      navL_->addWidget(l);
    };
    for (const Sec* a : faqList(room_)) { QString id = a->id;
      item(a->title + (hits.contains(id) ? "  ●" : ""), q.isEmpty() && sec_ == id, "", [this, id] { sec_ = id; search_->blockSignals(true); search_->clear(); search_->blockSignals(false); show_(); }); }
    item(QObject::tr("🏆 Achievements"), q.isEmpty() && sec_ == "ach", "color:#e8c66a;border-top:1px solid #3a3a3a;margin-top:8px;padding-top:8px", [this] {
      if (sec_ != "ach" || !search_->text().isEmpty()) pz_ = {}; sec_ = "ach"; search_->blockSignals(true); search_->clear(); search_->blockSignals(false); show_(); });
    navL_->addStretch(1);
    if (sec_ != "ach" || !q.isEmpty()) pz_ = {};                    // (left the section — the puzzle changes)
    if (q.isEmpty() && sec_ == "ach") { renderAch(); stack_->setCurrentWidget(achPage_); return; }
    stack_->setCurrentWidget(body_);
    if (!q.isEmpty()) {
      QString h = QString("<h3>%1</h3>").arg((res.empty() ? QObject::tr("Nothing found for \"%1\"") : QObject::tr("Results for \"%1\"")).arg(q.toHtmlEscaped()));
      if (res.empty()) h += "<p>" + QObject::tr("Try other words — e.g. a tool name, \"camera\", \"save\", \"loops\", \"hotkeys\". Or pick a section on the left.") + "</p>";
      QStringList qs; for (auto& v : faqQuery(q)) qs << v;
      for (auto& r : res) {
        // the snippet: the sentence with the most query words (then the longer one)
        QStringList sentences; for (const QString& s : faqText(*r.a).split(QRegularExpression("(?<=[.;])\\s+"))) if (s.size() > 3) sentences << s;
        auto rate = [&](const QString& s) { QStringList ws = faqWords(s); int n = 0; for (const QString& qq : qs) if (std::any_of(ws.begin(), ws.end(), [&](const QString& w) { return wordHit(qq, w); })) n++; return n * 1000 + std::min(int(s.size()), 200); };
        QString sn = sentences.isEmpty() ? QString() : sentences[0]; for (const QString& s : sentences) if (rate(s) > rate(sn)) sn = s;
        if (sn.size() > 220) sn = sn.left(220) + "…";
        h += QString("<table width=\"100%\" style=\"margin-bottom:8px\" cellpadding=\"8\" border=\"1\" bordercolor=\"#3a3a3a\" cellspacing=\"0\"><tr><td><a href=\"sec:%1\"><b>%2</b></a><br><span class=\"sn\">%3</span></td></tr></table>")
               .arg(r.a->id, r.a->title.toHtmlEscaped(), sn.toHtmlEscaped());
      }
      body_->setHtml(h); return;
    }
    auto L = faqList(room_); const Sec* a = L[0]; for (const Sec* s : L) if (s->id == sec_) a = s;
    body_->setHtml(QString("<h3>%1</h3>%2<p class=\"tags\">%3</p>").arg(a->title.toHtmlEscaped(), faqBody(*a), (QObject::tr("Search words: ") + a->tags).toHtmlEscaped()));
  }
  // 🏆 Achievements: a 3 × 3 sliding puzzle — or three in a row (a new one each time the section is opened); solved — the list
  void renderAch() {
    if (achPage_->layout()) { QLayoutItem* it; while ((it = achPage_->layout()->takeAt(0))) { if (it->widget()) it->widget()->deleteLater(); delete it; } delete achPage_->layout(); }
    auto* v = new QVBoxLayout(achPage_); v->setContentsMargins(16, 10, 16, 16);
    auto* h3 = new QLabel(QObject::tr("🏆 Achievements")); h3->setStyleSheet("color:#fff;font-size:15px;font-weight:bold"); v->addWidget(h3);
    if (!pz_.valid) { pz_ = newPuzzle(); if (pz_.match3) m3Deal(); }
    if (!pz_.solved && pz_.match3) { renderMatch3(v); return; }
    if (!pz_.solved) {
      auto* p = new QLabel(QObject::tr("The records are locked. Put the picture together to read them: click a tile next to the empty place to slide it.")); p->setWordWrap(true); p->setStyleSheet("color:#ccc"); v->addWidget(p);
      auto* board = new QWidget; board->setFixedSize(360, 360); board->setStyleSheet("background:#151515;border:1px solid #3a3a3a;border-radius:4px");
      QPixmap pic = ui::image(pz_.pic).scaled(360, 360, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
      for (int cell = 0; cell < 9; cell++) {
        int t = pz_.t[cell]; if (t < 0) continue;
        class Tile : public QLabel { public: std::function<void()> fn; using QLabel::QLabel; protected: void mousePressEvent(QMouseEvent*) override { if (fn) fn(); } };
        auto* tl = new Tile(board); tl->setGeometry((cell % 3) * 120, (cell / 3) * 120, 120, 120); tl->setPixmap(pic.copy((t % 3) * 120, (t / 3) * 120, 120, 120)); tl->setStyleSheet("border:1px solid rgba(0,0,0,140)"); tl->setCursor(Qt::PointingHandCursor);
        tl->fn = [this, cell] {
          int e = 0; for (int i = 0; i < 9; i++) if (pz_.t[i] == -1) e = i;
          if (std::abs(cell / 3 - e / 3) + std::abs(cell % 3 - e % 3) != 1) return;
          pz_.t[e] = pz_.t[cell]; pz_.t[cell] = -1; pz_.moves++;
          bool done = true; for (int i = 0; i < 9; i++) if (pz_.t[i] != -1 && pz_.t[i] != i) done = false;
          if (done) { pz_.solved = true; for (int i = 0; i < 9; i++) pz_.t[i] = i; renderAchBoardSolved(); QTimer::singleShot(1100, this, [this] { grant("secret"); if (sec_ == "ach" && pz_.solved) renderAch(); }); return; }
          QTimer::singleShot(0, this, [this] { renderAch(); });
        };
      }
      v->addWidget(board); v->addStretch(1); boardHost_ = board;
      return;
    }
    int n = 0; for (auto& a : achs()) if (has(a.id)) n++;
    auto* p = new QLabel(QObject::tr("%1 of %2 unlocked in this window. They are kept only while the program window is open — a new window starts from scratch.").arg(n).arg(achs().size())); p->setWordWrap(true); p->setStyleSheet("color:#ccc"); v->addWidget(p);
    auto* sc = new QScrollArea; sc->setWidgetResizable(true); sc->setStyleSheet("QScrollArea{border:none;background:transparent}"); auto* list = new QWidget; auto* lv = new QVBoxLayout(list); lv->setSpacing(6); lv->setContentsMargins(0, 0, 0, 0);
    for (auto& a : achs()) {
      bool g = has(a.id); auto* r = new QFrame; r->setStyleSheet(g ? "QFrame{background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #2a475e,stop:1 #1b2838);border:1px solid #3d6c8d;border-radius:3px}" : "QFrame{background:#242424;border:1px solid #3a3a3a;border-radius:3px}");
      auto* rh = new QHBoxLayout(r); rh->setContentsMargins(8, 6, 8, 6); rh->setSpacing(10);
      auto* ic = new QLabel; ic->setFixedSize(48, 48); ic->setAlignment(Qt::AlignCenter); ic->setStyleSheet("background:#000;border:none;color:#555;font:bold 22px Georgia");
      if (g) ic->setPixmap(ui::image(a.img).scaled(48, 48, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation).copy(0, 0, 48, 48)); else ic->setText("?");
      auto* tx = new QLabel(QString("<b style=\"color:%1\">%2</b><br><span style=\"color:%3;%4\">%5</span>").arg(g ? "#fff" : "#888", QObject::tr(g ? a.name : "Locked").toHtmlEscaped(), g ? "#acb2b8" : "#9a9a9a", g ? "" : "font-style:italic",
                                (g ? QObject::tr(a.desc) : QObject::tr("Hint: ") + QObject::tr(a.hint)).toHtmlEscaped()));
      tx->setStyleSheet("border:none;background:transparent"); tx->setWordWrap(true);
      rh->addWidget(ic); rh->addWidget(tx, 1); lv->addWidget(r);
    }
    lv->addStretch(1); sc->setWidget(list); v->addWidget(sc, 1);
  }
  // the other lock (three times in ten instead of the puzzle): three in a row on a 12 × 12 board of the program's tool
  // icons — swap two side by side; three alike in a row +1, a row of more +2 (the falling ones count too); 5 — the
  // records may be opened
  static constexpr int M3 = 12, M3K = 7;
  static const char* m3Icon(int k) { static const char* I[M3K] = {"tool_move", "tool_rotate", "tt_polypen", "tt_mirror", "tt_loop", "tt_patch", "tool_braid"}; return I[k]; }
  int m3At(int r, int c) const { return pz_.g[size_t(r * M3 + c)]; }
  // (the rows of three and more now: each cell in one marked; → the points they give)
  int m3Find(std::vector<char>& hit) const {
    hit.assign(M3 * M3, 0); int pts = 0;
    for (int dir = 0; dir < 2; dir++) for (int a = 0; a < M3; a++) for (int b = 0; b < M3;) {
      auto at = [&](int i) { return dir ? m3At(i, a) : m3At(a, i); }; int e = b + 1; while (e < M3 && at(e) == at(b) && at(b) >= 0) e++;
      if (e - b >= 3 && at(b) >= 0) { pts += e - b > 3 ? 2 : 1; for (int i = b; i < e; i++) hit[size_t(dir ? i * M3 + a : a * M3 + i)] = 1; }
      b = e; }
    return pts;
  }
  bool m3AnyMove() {                                               // (some swap makes a row — else the board is dealt again)
    std::vector<char> h;
    for (int i = 0; i < M3 * M3; i++) for (int j : {i + 1, i + M3}) { if (j >= M3 * M3 || (j == i + 1 && j % M3 == 0)) continue;
      std::swap(pz_.g[size_t(i)], pz_.g[size_t(j)]); const int p = m3Find(h); std::swap(pz_.g[size_t(i)], pz_.g[size_t(j)]); if (p) return true; }
    return false;
  }
  void m3Deal() {                                                  // (no row to begin with, and a move to make)
    do { pz_.g.assign(M3 * M3, 0);
      for (int i = 0; i < M3 * M3; i++) { int k; do k = QRandomGenerator::global()->bounded(M3K); while ((i % M3 >= 2 && pz_.g[size_t(i - 1)] == k && pz_.g[size_t(i - 2)] == k) || (i >= 2 * M3 && pz_.g[size_t(i - M3)] == k && pz_.g[size_t(i - 2 * M3)] == k)); pz_.g[size_t(i)] = k; }
    } while (!m3AnyMove());
  }
  // (a swap: kept if it makes a row — the rows go, the icons above fall, new ones from the top, again while rows form)
  void m3Swap(int i, int j) {
    std::swap(pz_.g[size_t(i)], pz_.g[size_t(j)]); std::vector<char> hit; int pts = m3Find(hit);
    if (!pts) { std::swap(pz_.g[size_t(i)], pz_.g[size_t(j)]); return; }
    while (pts) { pz_.score += pts;
      for (int c = 0; c < M3; c++) { int w = M3 - 1; for (int r = M3 - 1; r >= 0; r--) if (!hit[size_t(r * M3 + c)]) pz_.g[size_t(w-- * M3 + c)] = m3At(r, c);
        for (; w >= 0; w--) pz_.g[size_t(w * M3 + c)] = QRandomGenerator::global()->bounded(M3K); }
      pts = m3Find(hit); }
    if (!m3AnyMove()) m3Deal();
  }
  void renderMatch3(QVBoxLayout* v) {
    auto* p = new QLabel(QObject::tr("The records are locked. Three alike in a row: click an icon, then one next to it to swap them. Three in a row +1, a longer row +2 — 5 points open the records."));
    p->setWordWrap(true); p->setStyleSheet("color:#ccc"); v->addWidget(p);
    auto* sc = new QLabel(QObject::tr("Points: %1 / 5").arg(pz_.score)); sc->setStyleSheet(QString("color:%1;font-weight:bold").arg(pz_.score >= 5 ? "#8fe08f" : "#e8c66a")); v->addWidget(sc);
    const int cs = 34; auto* board = new QWidget; board->setFixedSize(M3 * cs, M3 * cs); board->setStyleSheet("background:#151515;border:1px solid #3a3a3a;border-radius:4px");
    static QHash<int, QPixmap> pics; if (pics.isEmpty()) for (int k = 0; k < M3K; k++) pics[k] = ui::image(m3Icon(k)).scaled(cs - 4, cs - 4, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    for (int i = 0; i < M3 * M3; i++) {
      class Cell : public QLabel { public: std::function<void()> fn; using QLabel::QLabel; protected: void mousePressEvent(QMouseEvent*) override { if (fn) fn(); } };
      auto* c = new Cell(board); c->setGeometry((i % M3) * cs, (i / M3) * cs, cs, cs); c->setAlignment(Qt::AlignCenter); c->setPixmap(pics.value(pz_.g[size_t(i)]));
      c->setStyleSheet(i == pz_.sel ? "background:#3d6c8d;border:1px solid #7fd4ff;border-radius:3px" : "background:transparent;border:none"); c->setCursor(Qt::PointingHandCursor);
      c->fn = [this, i] {
        const int s = pz_.sel;
        if (s < 0 || s == i) pz_.sel = s == i ? -1 : i;
        else if ((std::abs(s - i) == 1 && s / M3 == i / M3) || std::abs(s - i) == M3) { pz_.sel = -1; m3Swap(s, i); }
        else pz_.sel = i;
        QTimer::singleShot(0, this, [this] { renderAch(); }); };
    }
    v->addWidget(board);
    if (pz_.score >= 5) { auto* go = new QPushButton(QObject::tr("Open the records")); go->setProperty("primary", true); go->setFixedWidth(M3 * cs);
      QObject::connect(go, &QPushButton::clicked, this, [this] { pz_.solved = true; grant("secret"); renderAch(); }); v->addWidget(go); }
    v->addStretch(1);
  }
  void renderAchBoardSolved() {                                     // (the whole picture, its missing corner too)
    if (!boardHost_) return; for (auto* c : boardHost_->findChildren<QLabel*>()) c->deleteLater();
    auto* full = new QLabel(boardHost_); full->setGeometry(0, 0, 360, 360); full->setPixmap(ui::image(pz_.pic).scaled(360, 360, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)); full->show();
  }
  QLineEdit* search_; QWidget* nav_; QVBoxLayout* navL_; QStackedWidget* stack_; QTextBrowser* body_; QWidget* achPage_; QPointer<QWidget> boardHost_;
  QString room_ = "hair", sec_ = "basics"; QHash<QString, QString>* secOfPtr_ = nullptr; Puzzle pz_;
};
QPointer<FaqWindow> g_faq;

}  // namespace

bool has(const QString& id) { return got().contains(id); }
bool grant(const QString& id) {
  const Ach* a = nullptr; for (auto& x : achs()) if (id == x.id) a = &x;
  if (!a || got().contains(id)) return false;
  got().insert(id); popup()->push(*a);
  return true;
}
QStringList earned() { return QStringList(got().begin(), got().end()); }
void restore(const QStringList& ids) { for (const QString& i : ids) got().insert(i); }

void openFaq(QWidget* parent, const QString& room) {
  if (!g_faq) g_faq = new FaqWindow(parent);
  g_faq->open(room);
}
void openFaqAt(QWidget* parent, const QString& room, const QString& what) {
  if (!g_faq) g_faq = new FaqWindow(parent);
  g_faq->open(room);
  // (the name first — the part before the tip; failing that the tip's words)
  QString name = what.section(" | ", 0, 0).section(QRegularExpression("\s[—(:]"), 0, 0).remove(QRegularExpression("[^\w\s+/-]")).trimmed();
  if (name.isEmpty()) name = what.section(" | ", 1).section(QRegularExpression("\s[—(:]"), 0, 0).trimmed();
  g_faq->goTo(name, what.section(" | ", 1));
}

// ---------- confetti: little paper bits fly up and out, then fall and fade ----------
void confetti(QWidget* host, const QPoint& global) {
  class Conf : public QWidget {
  public:
    struct P { double dx, dy, w, h, spin, t; QColor c; bool round; };
    Conf(QWidget* host, QPoint o) : QWidget(host ? host->window() : nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::NoDropShadowWindowHint), o_(o) {
      setAttribute(Qt::WA_TranslucentBackground); setAttribute(Qt::WA_ShowWithoutActivating); setAttribute(Qt::WA_DeleteOnClose);
      QRect r(o - QPoint(700, 700), QSize(1400, 1500)); setGeometry(r); org_ = QPointF(700, 700);
      const char* cols[7] = {"#ff5e7e", "#ffd166", "#06d6a0", "#4cc9f0", "#b388ff", "#ff9f43", "#ffffff"};
      for (int i = 0; i < 90; i++) { double a = -M_PI / 2 + (rnd() - 0.5) * 2.2, v = 160 + rnd() * 260;
        ps_.push_back({std::cos(a) * v, std::sin(a) * v, 5 + rnd() * 5, 7 + rnd() * 8, (rnd() - 0.5) * 1440, 1400 + rnd() * 1200, QColor(cols[i % 7]), rnd() < 0.3}); }
      clock_.start(); auto* tm = new QTimer(this); tm->setInterval(16); QObject::connect(tm, &QTimer::timeout, this, [this] { if (clock_.elapsed() > 2700) close(); else update(); }); tm->start();
      show();
    }
  protected:
    void paintEvent(QPaintEvent*) override {
      QPainter g(this); g.setRenderHint(QPainter::Antialiasing); double now = clock_.elapsed();
      for (auto& p : ps_) {
        double k = std::min(1.0, now / p.t); if (k >= 1) continue;
        double e = 1 - std::pow(1 - k, 2.2);                        // (a cubic-bezier(.2,.7,.4,1) like ease-out)
        double x, y, rot, op;
        if (e < 0.35) { double u = e / 0.35; x = p.dx * 0.8 * u; y = p.dy * 0.8 * u; rot = p.spin * 0.4 * u; op = 1; }
        else { double u = (e - 0.35) / 0.65; x = p.dx * (0.8 + 0.4 * u); y = p.dy * 0.8 + (p.dy * 0.6 + 380 - p.dy * 0.8) * u; rot = p.spin * (0.4 + 0.6 * u); op = 1 - u; }
        g.save(); g.translate(org_ + QPointF(x, y)); g.rotate(rot); g.setOpacity(op); g.setPen(Qt::NoPen); g.setBrush(p.c);
        if (p.round) g.drawEllipse(QRectF(-p.w / 2, -p.h / 2, p.w, p.h)); else g.drawRect(QRectF(-p.w / 2, -p.h / 2, p.w, p.h)); g.restore();
      }
    }
  private:
    QPoint o_; QPointF org_; std::vector<P> ps_; QElapsedTimer clock_;
  };
  new Conf(host, global);
  play({{523, 0}, {659, 0.07}, {784, 0.14}, {1047, 0.21}}, true, 0.1, 0.45);   // (a little rising arpeggio)
}

// ---------- the FAQ girl: a hover picture of three at random; petted 5 s (pauses up to 1 s) — pet 1 / 2 / 3 ----------
void attachFaqButton(QPushButton* faq) {
  class F : public QObject {
  public:
    QPushButton* b; qint64 last = 0, until = 0; double t = 0;
    bool eventFilter(QObject*, QEvent* e) override {
      if (e->type() == QEvent::Enter) { int v = QRandomGenerator::global()->bounded(3); b->setIcon(ui::icon(v == 0 ? "faq_hover" : v == 1 ? "ach_faq2" : "ach_faq3")); last = QDateTime::currentMSecsSinceEpoch(); }
      else if (e->type() == QEvent::Leave) b->setIcon(ui::icon("faq"));
      else if (e->type() == QEvent::MouseMove || e->type() == QEvent::HoverMove) {
        int n = int(has("pet1")) + int(has("pet2")) + int(has("pet3")); qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (n >= 3 || now < until) { t = 0; last = now; return false; }   // (all earned / cooling down)
        double dt = double(now - last); last = now;
        if (dt > 1000) { t = 0; return false; }                         // (stopped stroking — start over)
        t += dt; if (t >= 5000) { t = 0; until = now + 30000; grant(QString("pet%1").arg(n + 1)); }
      }
      return false;
    }
  };
  auto* f = new F; f->b = faq; f->setParent(faq); faq->setMouseTracking(true); faq->setAttribute(Qt::WA_Hover); faq->installEventFilter(f);
}

// ---------- Mara: the pointer moving over her head — she is petted (Mara_Pat_1 → 2 → 3 every 0.5 s over the shown picture);
// petted 5 s in a row (pauses up to 1 s) — "And What Was That For?" ----------
void attachMara(QLabel* mara) {
  class M : public QObject {
  public:
    QLabel* l; QPixmap keep; QTimer* tick = nullptr; int i = 0; qint64 last = 0; double t = 0; bool on = false;
    void stop() { if (!on) return; on = false; if (tick) tick->stop(); l->setPixmap(keep); }
    bool eventFilter(QObject*, QEvent* e) override {
      if (e->type() == QEvent::Leave) { stop(); t = 0; return false; }
      if (e->type() != QEvent::MouseMove) return false;
      QPointF p = static_cast<QMouseEvent*>(e)->position(); double x = p.x() / l->width(), y = p.y() / l->height();
      bool head = x > 0.12 && x < 0.8 && y > 0 && y < 0.58; qint64 now = QDateTime::currentMSecsSinceEpoch();
      if (!head) { stop(); t = 0; return false; }
      if (!on) {
        on = true; keep = l->pixmap(); i = 0; last = now; t = 0;
        l->setPixmap(ui::image("mara_pat_1"));
        if (!tick) { tick = new QTimer(this); tick->setInterval(500); QObject::connect(tick, &QTimer::timeout, this, [this] {
          if (QDateTime::currentMSecsSinceEpoch() - last > 600) { stop(); return; } i++; l->setPixmap(ui::image(QString("mara_pat_%1").arg(i % 3 + 1))); }); }
        tick->start(); return false;
      }
      double dt = double(now - last); last = now;
      if (dt > 1000) { t = 0; return false; }
      t += dt; if (t >= 5000) { t = -1e18; grant("mara"); }
      return false;
    }
  };
  auto* m = new M; m->l = mara; m->setParent(mara); mara->setMouseTracking(true); mara->installEventFilter(m);
}

// ---------- the chimera: one launch in five she visits this window. One visit in three she is there as the program starts
// ("I Wish I Were That Lucky"); else she first peeks out 1–6 min after the start, then every 4–20 min, for 30 s — at the
// bottom of the tools or of the Active Tool window. Clicked — she hides: 1st / 2nd / 3rd catch, then confetti ----------
void startChimera(QWidget* toolbox, QWidget* activeTool) {
  if (rnd() >= 0.2) return;
  struct Chim : public QObject {
    QPointer<QWidget> hosts[2]; QPushButton* btn[2] = {}; QTimer* timer = nullptr;
    void later(bool first) { timer->start(int((first ? 1 + rnd() * 5 : 4 + rnd() * 16) * 60000)); }
    void hideAll() { for (auto* b : btn) if (b) b->hide(); }
    bool seen(int k) { QWidget* h = hosts[k]; return h && h->isVisible() && h->height() > 40 && h->width() > 70; }
    void show(int where) {
      timer->stop(); hideAll();
      int i = where >= 0 ? where : (rnd() < 0.5 ? 0 : 1);
      if (where < 0 && !seen(i) && seen(1 - i)) i = 1 - i;
      QWidget* h = hosts[i]; if (!h) { later(false); return; }
      QPushButton* b = btn[i]; int s = i ? 64 : 46;
      QPoint end(h->width() - s - (i ? 14 : 6), h->height() - s), start(end.x(), h->height());
      b->move(start); b->show(); b->raise();
      auto* a = new QPropertyAnimation(b, "pos", b); a->setDuration(600); a->setStartValue(start); a->setEndValue(end); a->setEasingCurve(QEasingCurve::OutBack); a->start(QAbstractAnimation::DeleteWhenStopped);
      QTimer::singleShot(30000, b, [this, b] { if (b->isVisible()) { b->hide(); later(false); } });
    }
  };
  auto* c = new Chim; c->setParent(toolbox); c->hosts[0] = toolbox; c->hosts[1] = activeTool;
  c->timer = new QTimer(c); c->timer->setSingleShot(true); QObject::connect(c->timer, &QTimer::timeout, c, [c] { c->show(-1); });
  for (int k = 0; k < 2; k++) {
    QWidget* h = c->hosts[k]; if (!h) continue; int s = k ? 64 : 46;
    auto* b = new QPushButton(h); b->setFixedSize(s, s); b->setIcon(ui::icon("chimera")); b->setIconSize(QSize(s, s)); b->setCursor(Qt::PointingHandCursor);
    b->setStyleSheet("QPushButton{border:none;background:transparent;padding:0}"); b->hide(); c->btn[k] = b;
    QObject::connect(b, &QPushButton::clicked, c, [c, b] {
      QPoint g = b->mapToGlobal(b->rect().center()); b->hide();
      int n = int(has("chim1")) + int(has("chim2")) + int(has("chim3"));
      if (n < 3) grant(QString("chim%1").arg(n + 1)); else confetti(b->window(), g);   // (every catch collected — she throws confetti)
      c->later(false);
    });
  }
  if (rnd() < 1.0 / 3) QTimer::singleShot(1500, c, [c] { c->show(-1); QTimer::singleShot(1200, [] { grant("lucky"); }); });   // (right at the start)
  else c->later(true);
}

}  // namespace hm::extras
