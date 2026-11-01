#include "app/RefBoard.h"
#include "app/RefPanels.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include <QApplication>
#include <QClipboard>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <map>

namespace hm {

namespace {
constexpr int HEAD = 24;
struct BItem { int id = 0; QString src; double w = 1, h = 1, x = 0, y = 0, s = 1, rot = 0, op = 1; bool flip = false; double l = 0, t = 0, r = 0, b = 0; };
struct Geom { double cx, cy, dw, dh, th, W, H; };
Geom geomOf(const BItem& it) {
  double W = it.w * it.s, H = it.h * it.s, th = it.rot * M_PI / 180;
  double ox = (it.l - it.r) / 2 * W * (it.flip ? -1 : 1), oy = (it.t - it.b) / 2 * H;
  return {it.x + ox * std::cos(th) - oy * std::sin(th), it.y + ox * std::sin(th) + oy * std::cos(th), W * (1 - it.l - it.r), H * (1 - it.t - it.b), th, W, H};
}
QPixmap pixOf(const QString& dataUrl) { QPixmap p; int c = dataUrl.indexOf(','); if (c >= 0) p.loadFromData(QByteArray::fromBase64(dataUrl.mid(c + 1).toLatin1())); return p; }
}  // namespace

struct RefBoard::Impl {
  QWidget* host; Scene* sc; std::function<void()> changed;
  std::vector<BItem> items; double panX = 0, panY = 0, zoom = 1; int sel = -1, uid = 1; bool crop = false, help = false;
  int bx = 60, by = 50, bw = 380, bh = 300; double op = 1; bool locked = false, open = false;
  std::map<int, QPixmap> pix;
  QPushButton* lockBtn = nullptr;                              // (locked: the only part that takes the mouse — a window of its own)
  QTimer* follow = nullptr;
  // a drag: pan / move / scale / rot / l / r / t / b / win (title bar) / grip / op
  struct Drag { QString t; int it = -1; QPointF m0; double x0 = 0, y0 = 0, s0 = 1, rot0 = 0, a0 = 0, d0 = 1, px = 0, py = 0; QPointF c, cw; bool moved = false; QPoint g0; QRect w0; double op0 = 1; } dr;

  QRectF body(const QWidget*) const { return QRectF(0, HEAD, bw, bh - HEAD); }   // (from the kept size: right also before the window is first shown)
  BItem* selItem() { for (auto& i : items) if (i.id == sel) return &i; return nullptr; }
  QPointF scr(double x, double y) const { return QPointF(panX + x * zoom, panY + y * zoom + HEAD); }   // board units → the window
  void load() {
    QJsonObject B = sc->refBoard; items.clear(); pix.clear(); sel = -1; crop = false;
    for (const QJsonValue& v : B.value("items").toArray()) { QJsonObject o = v.toObject(), c = o.value("crop").toObject(); BItem i;
      i.id = o.value("id").toInt(); i.src = o.value("src").toString(); i.w = o.value("w").toDouble(1); i.h = o.value("h").toDouble(1); i.x = o.value("x").toDouble(); i.y = o.value("y").toDouble();
      i.s = o.value("s").toDouble(1); i.rot = o.value("rot").toDouble(); i.op = o.value("op").toDouble(1); i.flip = o.value("flip").toBool();
      i.l = c.value("l").toDouble(); i.t = c.value("t").toDouble(); i.r = c.value("r").toDouble(); i.b = c.value("b").toDouble(); items.push_back(i); }
    QJsonObject pn = B.value("pan").toObject(); panX = pn.value("x").toDouble(); panY = pn.value("y").toDouble(); zoom = B.value("zoom").toDouble(1);
    uid = 1; for (auto& i : items) uid = std::max(uid, i.id + 1);
  }
  void store() {
    QJsonArray a; for (auto& i : items) a.append(QJsonObject{{"id", i.id}, {"src", i.src}, {"w", i.w}, {"h", i.h}, {"x", i.x}, {"y", i.y}, {"s", i.s}, {"rot", i.rot}, {"op", i.op}, {"flip", i.flip},
                                                           {"crop", QJsonObject{{"l", i.l}, {"t", i.t}, {"r", i.r}, {"b", i.b}}}});
    sc->refBoard = QJsonObject{{"items", a}, {"pan", QJsonObject{{"x", panX}, {"y", panY}}}, {"zoom", zoom}};
    if (changed) changed();
  }
  void saveWin() const { QSettings st; st.setValue("board/x", bx); st.setValue("board/y", by); st.setValue("board/w", bw); st.setValue("board/h", bh); st.setValue("board/op", op); st.setValue("board/locked", locked); st.setValue("board/open", open); }
  const QPixmap& pixmap(const BItem& i) { auto it = pix.find(i.id); if (it == pix.end()) it = pix.emplace(i.id, pixOf(i.src)).first; return it->second; }
  // the item under a point of the window (the topmost)
  int itemAt(const QPointF& p) {
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
      Geom g = geomOf(*it); QPointF c = scr(g.cx, g.cy); double dx = (p.x() - c.x()) / zoom, dy = (p.y() - c.y()) / zoom;
      double lx = dx * std::cos(g.th) + dy * std::sin(g.th), ly = -dx * std::sin(g.th) + dy * std::cos(g.th);
      if (std::abs(lx) <= g.dw / 2 && std::abs(ly) <= g.dh / 2) return it->id;
    }
    return -1;
  }
  struct Handle { QString key; QPointF p; };
  std::vector<Handle> handles() {
    std::vector<Handle> hs; BItem* it = selItem(); if (!it) return hs;
    Geom g = geomOf(*it); QPointF c = scr(g.cx, g.cy); double cs = std::cos(g.th), sn = std::sin(g.th);
    auto P = [&](double lx, double ly) { return QPointF(c.x() + (lx * cs - ly * sn) * zoom, c.y() + (lx * sn + ly * cs) * zoom); };
    if (crop) { hs.push_back({it->flip ? "r" : "l", P(-g.dw / 2, 0)}); hs.push_back({it->flip ? "l" : "r", P(g.dw / 2, 0)}); hs.push_back({"t", P(0, -g.dh / 2)}); hs.push_back({"b", P(0, g.dh / 2)}); }
    else { for (auto [sx, sy] : {std::pair{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}) hs.push_back({"scale", P(sx * g.dw / 2, sy * g.dh / 2)}); hs.push_back({"rot", P(0, -g.dh / 2 - 22 / zoom)}); }
    return hs;
  }
  QRect headButton(const QWidget* w, int k) const { int x = w->width() - 4 - 22 * (5 - k); return QRect(x, 3, 20, 18); }   // 0 ＋ · 1 ● · 2 🔒 · 3 ? · 4 ✕
};

RefBoard::RefBoard(QWidget* host, Scene* sc, std::function<void()> changed)
    : QWidget(host->window(), Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), d(new Impl{host, sc, std::move(changed)}) {
  setAttribute(Qt::WA_TranslucentBackground); setAcceptDrops(true); setMouseTracking(true); setFocusPolicy(Qt::StrongFocus); setMinimumSize(200, 150);
  QSettings st; d->bx = st.value("board/x", 60).toInt(); d->by = st.value("board/y", 50).toInt(); d->bw = st.value("board/w", 380).toInt(); d->bh = st.value("board/h", 300).toInt();
  d->op = st.value("board/op", 1.0).toDouble(); d->locked = st.value("board/locked", false).toBool(); d->open = st.value("board/open", false).toBool();
  d->lockBtn = new QPushButton("🔒", host->window()); d->lockBtn->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint); d->lockBtn->setFixedSize(22, 20);
  d->lockBtn->setToolTip(tr("Locked — the window lets the mouse through. Double-click to unlock"));
  QObject::connect(d->lockBtn, &QPushButton::clicked, this, [] {});
  d->lockBtn->hide();
  // (double-click on 🔒: unlock — the button's own window takes the clicks while the board lets them through)
  class Dbl : public QObject { public: RefBoard* b; bool eventFilter(QObject*, QEvent* e) override { if (e->type() == QEvent::MouseButtonDblClick) { b->setLocked(false); return true; } return false; } };
  auto* f = new Dbl; f->b = this; f->setParent(this); d->lockBtn->installEventFilter(f);
  d->follow = new QTimer(this); d->follow->setInterval(50);
  QObject::connect(d->follow, &QTimer::timeout, this, [this] {
    QWidget* w = d->host->window(); bool want = d->open && d->host->isVisible() && w->isVisible() && !w->isMinimized();
    if (!want) { if (isVisible()) hide(); d->lockBtn->hide(); return; }
    QRect hr(d->host->mapToGlobal(QPoint(0, 0)), d->host->size());
    if (hr.width() > 100 && hr.height() > 100) { d->bw = std::clamp(d->bw, 200, hr.width()); d->bh = std::clamp(d->bh, 150, hr.height()); d->bx = std::clamp(d->bx, 0, hr.width() - 80); d->by = std::clamp(d->by, 0, hr.height() - 24); }
    QRect g(hr.topLeft() + QPoint(d->bx, d->by), QSize(d->bw, d->bh));
    if (geometry() != g) setGeometry(g);
    if (!isVisible()) show();
    if (std::abs(windowOpacity() - d->op) > 1e-3) setWindowOpacity(d->op);
    if (d->locked) { QRect lb = d->headButton(this, 2); d->lockBtn->move(g.topLeft() + lb.topLeft()); if (!d->lockBtn->isVisible()) d->lockBtn->show(); d->lockBtn->raise(); } else d->lockBtn->hide();
  });
  d->follow->start();
  d->load();
  setWindowFlag(Qt::WindowTransparentForInput, d->locked);
}

bool RefBoard::isOpen() const { return d->open; }
bool RefBoard::isLocked() const { return d->locked; }
void RefBoard::showBoard(bool on) { d->open = on; d->saveWin(); if (!on) hide(); update(); }
void RefBoard::setLocked(bool on) {
  d->locked = on; d->saveWin();
  bool vis = isVisible(); setWindowFlag(Qt::WindowTransparentForInput, on); if (vis) show();
  if (on) ui::toast(tr("Board locked — the mouse goes through it. Double-click 🔒 to unlock"));
  update();
}
void RefBoard::reload() { d->load(); update(); }

void RefBoard::addFiles(const QStringList& files, const QPointF* at) {
  QRectF B = d->body(this); double z = d->zoom;
  QPointF a = at ? *at : QPointF((B.width() / 2 - d->panX) / z, (B.height() / 2 - d->panY) / z);
  double x = a.x(), prev = 0; int n = 0;
  for (const QString& f : files) {
    QString url; int w, h; if (!refImageFromFile(f, &url, &w, &h)) { ui::toast(tr("Not an image: %1").arg(QFileInfo(f).fileName())); continue; }
    double s = std::min({1.0, 0.6 * B.width() / z / w, 0.6 * B.height() / z / h});
    if (n) x += prev / 2 + 10 / z + w * s / 2;
    BItem it; it.id = d->uid++; it.src = url; it.w = w; it.h = h; it.x = x; it.y = a.y(); it.s = s; d->items.push_back(it);
    d->sel = it.id; prev = w * s; n++;
  }
  if (n) { d->crop = false; d->store(); update(); }
}

void RefBoard::paintEvent(QPaintEvent*) {
  QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::SmoothPixmapTransform);
  QRectF all = rect().adjusted(0.5, 0.5, -0.5, -0.5);
  if (!d->locked) { p.setPen(QColor(0x55, 0x55, 0x55)); p.setBrush(QColor(0x1b, 0x1b, 0x1b)); p.drawRoundedRect(all, 6, 6); }
  else { p.setPen(Qt::NoPen); p.setBrush(QColor(0x1b, 0x1b, 0x1b)); p.drawRoundedRect(all, 6, 6); }
  // the title bar
  if (!d->locked) {
    QPainterPath hp; hp.addRoundedRect(QRectF(0.5, 0.5, width() - 1, HEAD), 6, 6); hp.addRect(QRectF(0.5, HEAD - 6, width() - 1, 6));
    p.setPen(Qt::NoPen); p.setBrush(QColor(0x2b, 0x2b, 0x2b)); p.drawPath(hp.simplified());
    p.setPen(QColor(0xaa, 0xaa, 0xaa)); QFont f = font(); f.setPointSizeF(8); p.setFont(f); p.drawText(QRectF(8, 0, width() - 130, HEAD), Qt::AlignVCenter, tr("Reference board"));
    const char* labs[5] = {"＋", "●", d->locked ? "🔒" : "🔓", "?", "✕"};
    for (int k = 0; k < 5; k++) { QRect r = d->headButton(this, k); p.setPen(QColor(0x55, 0x55, 0x55)); p.setBrush(QColor(0x38, 0x38, 0x38)); p.drawRoundedRect(r, 3, 3); p.setPen(QColor(0xdd, 0xdd, 0xdd)); p.drawText(r, Qt::AlignCenter, labs[k]); }
  }
  QRectF B = d->body(this); p.save(); p.setClipRect(B);
  for (const BItem& it : d->items) {
    const QPixmap& px = d->pixmap(it); if (px.isNull()) continue;
    Geom g = geomOf(it); QPointF c = d->scr(g.cx, g.cy);
    p.save(); p.translate(c); p.rotate(it.rot); p.scale(d->zoom, d->zoom); if (it.flip) p.scale(-1, 1); p.setOpacity(it.op);
    QRectF src(it.l * px.width(), it.t * px.height(), (1 - it.l - it.r) * px.width(), (1 - it.t - it.b) * px.height());
    p.drawPixmap(QRectF(-g.dw / 2, -g.dh / 2, g.dw, g.dh), px, src);
    p.restore();
  }
  if (d->items.empty()) { p.setPen(QColor(0x66, 0x66, 0x66)); p.drawText(B, Qt::AlignCenter | Qt::TextWordWrap, tr("Drop images here, paste (Ctrl+V) or press ＋")); }
  if (!d->locked) if (BItem* it = d->selItem()) {                 // the selection: its frame, its handles
    Geom g = geomOf(*it); QPointF c = d->scr(g.cx, g.cy);
    p.save(); p.translate(c); p.rotate(it->rot); QPen pen(QColor(0xff, 0xa9, 0x40)); if (d->crop) pen.setStyle(Qt::DashLine); p.setPen(pen); p.setBrush(Qt::NoBrush);
    p.drawRect(QRectF(-g.dw * d->zoom / 2, -g.dh * d->zoom / 2, g.dw * d->zoom, g.dh * d->zoom)); p.restore();
    for (auto& h : d->handles()) {
      p.setPen(QColor(0x22, 0x22, 0x22));
      if (h.key == "rot") { p.setBrush(QColor(0x7f, 0xc1, 0xff)); p.drawEllipse(h.p, 5, 5); }
      else if (h.key == "scale") { p.setBrush(QColor(0xff, 0xa9, 0x40)); p.drawRect(QRectF(h.p.x() - 4.5, h.p.y() - 4.5, 9, 9)); }
      else { p.setBrush(Qt::white); bool v = h.key == "l" || h.key == "r"; p.drawRect(v ? QRectF(h.p.x() - 3, h.p.y() - 6, 6, 12) : QRectF(h.p.x() - 6, h.p.y() - 3, 12, 6)); }
    }
  }
  if (!d->locked) {
    // the tip line, the grip, the help
    QRectF tip(B.left(), B.bottom() - 18, B.width(), 18); p.fillRect(tip, QColor(20, 20, 20, 184)); p.setPen(QColor(0xbb, 0xbb, 0xbb)); QFont f = font(); f.setPointSizeF(7.5); p.setFont(f);
    p.drawText(tip.adjusted(6, 0, -18, 0), Qt::AlignVCenter, p.fontMetrics().elidedText(tr("Shift + drag (or middle mouse) — move the view · drag — move an image · wheel — zoom · ? — all keys"), Qt::ElideRight, int(tip.width() - 24)));
    p.setPen(QPen(QColor(0x77, 0x77, 0x77), 1)); for (int k = 0; k < 3; k++) p.drawLine(QPointF(width() - 3 - k * 4, height() - 2), QPointF(width() - 2, height() - 3 - k * 4));
    if (d->help) {
      QRectF hb = B.adjusted(8, 8, -8, -B.height() / 3); p.setPen(QColor(0x44, 0x44, 0x44)); p.setBrush(QColor(20, 20, 20, 242)); p.drawRoundedRect(hb, 5, 5);
      p.setPen(QColor(0xcc, 0xcc, 0xcc));
      p.drawText(hb.adjusted(10, 8, -10, -8), Qt::TextWordWrap, tr("Images: drop files · Ctrl+V — paste · ＋ / double-click on empty space — import\nLMB — select / move · corner squares — scale · blue circle — rotate (Shift — 15° steps)\n"
                                                              "C — crop (drag the white edges), Shift+C — uncrop · H — mirror · 0 — reset rotation\n[ / ] or Alt+wheel — image opacity · Del — delete · PageUp / PageDown — to front / back\n"
                                                              "Board: wheel — zoom · LMB / MMB drag on empty space — pan · Shift + LMB drag — pan anywhere (the images stay) · F — fit all\n"
                                                              "Window: drag the title bar — move · corner — resize · ● hold + drag up / down — window opacity (to 1%)\n🔓 — lock: the window lets the mouse through to the viewport; double-click 🔒 — unlock"));
    }
  }
  p.restore();
}

void RefBoard::mousePressEvent(QMouseEvent* e) {
  if (d->locked) return;
  QPointF m = e->position(); d->dr = {}; d->dr.m0 = m; d->dr.g0 = e->globalPosition().toPoint();
  if (m.y() < HEAD) {
    for (int k = 0; k < 5; k++) if (d->headButton(this, k).contains(m.toPoint())) {
      if (k == 0) { QSettings st; QStringList fs = QFileDialog::getOpenFileNames(this, tr("Reference images"), st.value("refs/dir").toString(), tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)")); if (!fs.isEmpty()) { st.setValue("refs/dir", QFileInfo(fs[0]).absolutePath()); addFiles(fs); } }
      else if (k == 1) { d->dr.t = "op"; d->dr.op0 = d->op; }
      else if (k == 2) setLocked(true);
      else if (k == 3) { d->help = !d->help; update(); }
      else showBoard(false);
      return;
    }
    if (e->button() == Qt::LeftButton) { d->dr.t = "win"; d->dr.w0 = QRect(d->bx, d->by, d->bw, d->bh); }
    return;
  }
  if (m.x() > width() - 14 && m.y() > height() - 14 && e->button() == Qt::LeftButton) { d->dr.t = "grip"; d->dr.w0 = QRect(d->bx, d->by, d->bw, d->bh); return; }
  if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ShiftModifier))) { d->dr.t = "pan"; d->dr.px = d->panX; d->dr.py = d->panY; return; }
  if (e->button() != Qt::LeftButton) return;
  BItem* it = d->selItem();
  if (it) for (auto& h : d->handles()) if (std::abs(m.x() - h.p.x()) <= 7 && std::abs(m.y() - h.p.y()) <= 7) {
    Geom g = geomOf(*it); QPointF c = d->scr(g.cx, g.cy);
    d->dr.t = h.key; d->dr.it = it->id; d->dr.c = c; d->dr.cw = QPointF(g.cx, g.cy); d->dr.s0 = it->s; d->dr.rot0 = it->rot;
    d->dr.a0 = std::atan2(m.y() - c.y(), m.x() - c.x()); d->dr.d0 = std::hypot(m.x() - c.x(), m.y() - c.y()); return;
  }
  int hit = d->itemAt(m);
  if (hit >= 0) { if (d->sel != hit) d->crop = false; d->sel = hit; BItem* h = d->selItem(); d->dr.t = "move"; d->dr.it = hit; d->dr.x0 = h->x; d->dr.y0 = h->y; update(); return; }
  d->sel = -1; d->crop = false; update();
  d->dr.t = "pan"; d->dr.px = d->panX; d->dr.py = d->panY;
}

void RefBoard::mouseMoveEvent(QMouseEvent* e) {
  auto& r = d->dr; if (r.t.isEmpty()) return;
  QPointF m = e->position(); QPoint g = e->globalPosition().toPoint(); double dx = m.x() - r.m0.x(), dy = m.y() - r.m0.y(), z = d->zoom;
  if (r.t == "win") { d->bx = r.w0.x() + (g.x() - r.g0.x()); d->by = r.w0.y() + (g.y() - r.g0.y()); r.moved = true; return; }
  if (r.t == "grip") { d->bw = std::max(200, r.w0.width() + (g.x() - r.g0.x())); d->bh = std::max(150, r.w0.height() + (g.y() - r.g0.y())); r.moved = true; return; }
  if (r.t == "op") { d->op = std::clamp(r.op0 - (g.y() - r.g0.y()) / 200.0, 0.01, 1.0); setWindowOpacity(d->op); ui::hud(tr("Board opacity %1%").arg(int(std::lround(d->op * 100)))); r.moved = true; return; }
  if (!r.moved && std::hypot(dx, dy) < 2) return;
  r.moved = true;
  BItem* it = nullptr; for (auto& i : d->items) if (i.id == r.it) it = &i;
  auto keepCentre = [&] { Geom g2 = geomOf(*it); it->x += r.cw.x() - g2.cx; it->y += r.cw.y() - g2.cy; };   // (scale / turn about the visible centre)
  if (r.t == "pan") { d->panX = r.px + dx; d->panY = r.py + dy; setCursor(Qt::ClosedHandCursor); }
  else if (!it) return;
  else if (r.t == "move") { it->x = r.x0 + dx / z; it->y = r.y0 + dy / z; }
  else if (r.t == "scale") { it->s = std::max(0.005, r.s0 * std::hypot(m.x() - r.c.x(), m.y() - r.c.y()) / std::max(1.0, r.d0)); keepCentre(); }
  else if (r.t == "rot") { double a = r.rot0 + (std::atan2(m.y() - r.c.y(), m.x() - r.c.x()) - r.a0) * 180 / M_PI; if (e->modifiers() & Qt::ShiftModifier) a = std::round(a / 15) * 15; it->rot = a; keepCentre(); }
  else {                                                         // crop: the point in the whole image's own frame
    double wx = (m.x() - d->panX) / z - it->x, wy = (m.y() - HEAD - d->panY) / z - it->y, th = it->rot * M_PI / 180;
    double lx = wx * std::cos(th) + wy * std::sin(th), ly = -wx * std::sin(th) + wy * std::cos(th); if (it->flip) lx = -lx;
    double W = it->w * it->s, H = it->h * it->s;
    if (r.t == "l") it->l = std::clamp((lx + W / 2) / W, 0.0, 1 - it->r - 0.02);
    if (r.t == "r") it->r = std::clamp((W / 2 - lx) / W, 0.0, 1 - it->l - 0.02);
    if (r.t == "t") it->t = std::clamp((ly + H / 2) / H, 0.0, 1 - it->b - 0.02);
    if (r.t == "b") it->b = std::clamp((H / 2 - ly) / H, 0.0, 1 - it->t - 0.02);
  }
  update();
}
void RefBoard::mouseReleaseEvent(QMouseEvent*) {
  auto& r = d->dr;
  if (r.t == "win" || r.t == "grip" || r.t == "op") { d->saveWin(); if (r.t == "op") ui::hud({}); }
  else if (r.moved) d->store();
  r = {}; unsetCursor();
}
void RefBoard::mouseDoubleClickEvent(QMouseEvent* e) {
  if (d->locked || e->position().y() < HEAD || d->itemAt(e->position()) >= 0) return;
  QSettings st; QStringList fs = QFileDialog::getOpenFileNames(this, tr("Reference images"), st.value("refs/dir").toString(), tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)"));
  if (fs.isEmpty()) return; st.setValue("refs/dir", QFileInfo(fs[0]).absolutePath());
  QPointF at((e->position().x() - d->panX) / d->zoom, (e->position().y() - HEAD - d->panY) / d->zoom); addFiles(fs, &at);
}
void RefBoard::wheelEvent(QWheelEvent* e) {
  if (d->locked) return;
  double dy = e->angleDelta().y() ? -e->angleDelta().y() : -e->angleDelta().x();
  BItem* it = d->selItem();
  if ((e->modifiers() & Qt::AltModifier) && it) { it->op = std::clamp(it->op - (dy > 0 ? 1 : -1) * 0.05, 0.05, 1.0); d->store(); update(); return; }
  QPointF m = e->position() - QPointF(0, HEAD); double z = d->zoom, z2 = std::clamp(z * std::pow(1.0015, -dy), 0.02, 20.0);
  d->panX = m.x() - (m.x() - d->panX) / z * z2; d->panY = m.y() - (m.y() - d->panY) / z * z2; d->zoom = z2;
  update();
}
void RefBoard::keyPressEvent(QKeyEvent* e) {
  if (d->locked) return;
  if (e->matches(QKeySequence::Paste)) {
    const QMimeData* md = QApplication::clipboard()->mimeData();
    if (md && md->hasImage()) { QImage img = qvariant_cast<QImage>(md->imageData()); QTemporaryDir td; QString f = td.path() + "/pasted.png"; if (img.save(f)) addFiles({f}); return; }
    if (md && md->hasUrls()) { QStringList fs; for (const QUrl& u : md->urls()) if (u.isLocalFile()) fs << u.toLocalFile(); if (!fs.isEmpty()) addFiles(fs); }
    return;
  }
  if (e->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) return;
  int k = e->key(); BItem* it = d->selItem();
  if (k == Qt::Key_F) {                                         // fit all
    if (d->items.empty()) return;
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    for (auto& i : d->items) { Geom g = geomOf(i); double cs = std::cos(g.th), sn = std::sin(g.th);
      for (auto [sx, sy] : {std::pair{-1, -1}, {1, -1}, {1, 1}, {-1, 1}}) { double x = g.cx + (sx * g.dw / 2) * cs - (sy * g.dh / 2) * sn, y = g.cy + (sx * g.dw / 2) * sn + (sy * g.dh / 2) * cs; x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); } }
    QRectF B = d->body(this); double z = std::clamp(0.92 * std::min(B.width() / std::max(1e-6, x1 - x0), B.height() / std::max(1e-6, y1 - y0)), 0.02, 20.0);
    d->zoom = z; d->panX = B.width() / 2 - (x0 + x1) / 2 * z; d->panY = B.height() / 2 - (y0 + y1) / 2 * z; d->store(); update(); return;
  }
  if (!it) return;
  if (k == Qt::Key_Delete || k == Qt::Key_Backspace) { int id = it->id; d->items.erase(std::remove_if(d->items.begin(), d->items.end(), [id](const BItem& x) { return x.id == id; }), d->items.end()); d->pix.erase(id); d->sel = -1; d->crop = false; }
  else if (k == Qt::Key_C) { if (e->modifiers() & Qt::ShiftModifier) it->l = it->t = it->r = it->b = 0; else d->crop = !d->crop; }
  else if (k == Qt::Key_H) it->flip = !it->flip;
  else if (k == Qt::Key_0) { Geom g = geomOf(*it); it->rot = 0; Geom g2 = geomOf(*it); it->x += g.cx - g2.cx; it->y += g.cy - g2.cy; }
  else if (k == Qt::Key_BracketLeft || k == Qt::Key_BracketRight) { it->op = std::clamp(it->op + (k == Qt::Key_BracketRight ? 0.1 : -0.1), 0.05, 1.0); ui::hud(tr("Image opacity %1%").arg(int(std::lround(it->op * 100)))); QTimer::singleShot(700, [] { ui::hud({}); }); }
  else if (k == Qt::Key_PageUp || k == Qt::Key_PageDown) { BItem c = *it; d->items.erase(std::remove_if(d->items.begin(), d->items.end(), [&](const BItem& x) { return x.id == c.id; }), d->items.end()); if (k == Qt::Key_PageUp) d->items.push_back(c); else d->items.insert(d->items.begin(), c); }
  else return;
  d->store(); update();
}
void RefBoard::dragEnterEvent(QDragEnterEvent* e) { if (!d->locked && (e->mimeData()->hasUrls() || e->mimeData()->hasImage())) e->acceptProposedAction(); }
void RefBoard::dropEvent(QDropEvent* e) {
  QPointF m = e->position(); QPointF at((m.x() - d->panX) / d->zoom, (m.y() - HEAD - d->panY) / d->zoom);
  QStringList fs; for (const QUrl& u : e->mimeData()->urls()) if (u.isLocalFile()) fs << u.toLocalFile();
  if (!fs.isEmpty()) { addFiles(fs, &at); e->acceptProposedAction(); return; }
  if (e->mimeData()->hasImage()) { QImage img = qvariant_cast<QImage>(e->mimeData()->imageData()); QTemporaryDir td; QString f = td.path() + "/dropped.png"; if (img.save(f)) addFiles({f}, &at); e->acceptProposedAction(); return; }
  ui::toast(tr("Could not load the dropped image — save it and drop the file"));
}

}  // namespace hm
