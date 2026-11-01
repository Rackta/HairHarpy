#include "app/RefPanels.h"
#include "app/RefBoard.h"
#include "scene/Scene.h"
#include "ui/Pie.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QTimer>
#include <QVBoxLayout>

namespace hm {

bool refImageFromFile(const QString& file, QString* dataUrl, int* w, int* h) {
  QImageReader r(file); r.setAutoTransform(true); QImage img = r.read();
  if (img.isNull()) return false;
  double k = std::min(1.0, 2048.0 / std::max(img.width(), img.height()));
  if (k < 1) img = img.scaled(std::max(1, int(std::lround(img.width() * k))), std::max(1, int(std::lround(img.height() * k))), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
  QString ext = QFileInfo(file).suffix().toLower(); bool png = img.hasAlphaChannel() || ext == "png" || ext == "gif" || ext == "webp" || ext == "svg";
  QByteArray b; QBuffer buf(&b); buf.open(QIODevice::WriteOnly); img.save(&buf, png ? "PNG" : "JPEG", png ? -1 : 92);
  *dataUrl = QString("data:image/%1;base64,").arg(png ? "png" : "jpeg") + QString::fromLatin1(b.toBase64());
  *w = img.width(); *h = img.height(); return true;
}

namespace {

class RefMenu : public QFrame {
public:
  RefMenu(QWidget* parent, Viewport* vp, Scene* sc, RefBoard* board) : QFrame(parent, Qt::Popup), vp_(vp), sc_(sc), board_(board) {
    setAttribute(Qt::WA_DeleteOnClose); setObjectName("refMenu");
    setStyleSheet("#refMenu{background:#262626;border:1px solid #555;border-radius:6px} QLabel[h5=\"true\"]{color:#9a9a9a;font-size:11px;margin-top:6px}"
                  " QFrame[rp=\"true\"]{border:1px solid transparent;border-radius:4px} QFrame[rp=\"true\"]:hover{background:#2e2e2e} QFrame[rp=\"true\"][act=\"true\"]{border-color:#4a7fd6;background:#2c3a52}");
    v_ = new QVBoxLayout(this); v_->setContentsMargins(10, 8, 10, 10); v_->setSpacing(4); setFixedWidth(320);
    build();
  }
  void rebuild() { QTimer::singleShot(0, this, [this] { QWidget* c = content_; content_ = nullptr; if (c) c->deleteLater(); build(); adjustSize(); }); }
private:
  QLabel* h5(const QString& t) { auto* l = new QLabel(t); l->setProperty("h5", true); return l; }
  QPushButton* b(const QString& lab, const QString& tip, std::function<void()> fn, bool on = false) {
    auto* x = new QPushButton(lab); x->setToolTip(tip); x->setFocusPolicy(Qt::NoFocus); if (on) { x->setCheckable(true); x->setChecked(true); }
    QObject::connect(x, &QPushButton::clicked, this, [this, fn] { fn(); rebuild(); }); return x;
  }
  RefPlane* active() { return vp_->refPlane(vp_->activeRefPlane()); }
  void edited(bool commitNow = true) { vp_->refPlanesEdited(commitNow); }
  void build() {
    content_ = new QWidget; auto* v = new QVBoxLayout(content_); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(4); v_->addWidget(content_);
    auto* t = h5(QObject::tr("Reference board")); t->setStyleSheet("margin-top:0"); v->addWidget(t);
    auto* r1 = new QHBoxLayout; v->addLayout(r1);
    r1->addWidget(b(board_->isOpen() ? QObject::tr("🗔 Hide the board") : QObject::tr("🗔 Show the board"), QObject::tr("A free window over the viewport with your reference images (like PureRef)"), [this] { board_->showBoard(!board_->isOpen()); }, board_->isOpen()));
    if (board_->isLocked()) r1->addWidget(b(QObject::tr("🔒 Unlock the board"), {}, [this] { board_->setLocked(false); }));
    r1->addStretch(1);
    auto* hr = new QHBoxLayout; hr->addWidget(h5(QObject::tr("Reference planes")), 1);
    auto* plus = b("＋", QObject::tr("A new plane: pick the view it is for"), [this] { choose_ = !choose_; }, choose_); plus->setFixedWidth(30); hr->addWidget(plus); v->addLayout(hr);
    if (choose_) {
      auto* r2 = new QHBoxLayout; r2->setSpacing(3); v->addLayout(r2);
      for (const char* view : refs::VIEWS) {
        QString vw = view; auto* x = b(refs::viewLabel(vw), QObject::tr("A plane for the %1 view").arg(refs::viewLabel(vw).toLower()), [this, vw] {
          choose_ = false; int n = 1; for (auto& r : sc_->refPlanes) if (r.view == vw) n++;
          RefPlane rp; rp.id = sc_->nextRefId++; rp.view = vw; rp.name = QString("%1 %2").arg(refs::viewLabel(vw)).arg(n); rp.lock = QJsonValue(QJsonValue::Null);
          refs::place(rp, *sc_, vp_->refHomeCamera(rp)); sc_->refPlanes.push_back(rp);
          edited(); vp_->setActiveRefPlane(rp.id);
          ui::toast(QObject::tr("%1: a plane behind the model — ＋ Image to put a picture on it").arg(rp.name));
        });
        x->setStyleSheet("padding:2px 5px"); r2->addWidget(x);
      }
    }
    if (sc_->refPlanes.empty() && !choose_) { auto* m = new QLabel(QObject::tr("No planes yet — press ＋ and pick a view.")); m->setProperty("mut", true); v->addWidget(m); }
    for (const RefPlane& rp : sc_->refPlanes) {
      const int id = rp.id;
      class Row : public QFrame { public: std::function<void()> click; protected: void mouseReleaseEvent(QMouseEvent* e) override { if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()) && click) click(); } };
      auto* r = new Row; r->setProperty("rp", true); r->setProperty("act", vp_->activeRefPlane() == id); r->setCursor(Qt::PointingHandCursor);
      r->setToolTip(QObject::tr("Click — work with this plane (the gizmo moves, turns, scales it). Click again — let go"));
      auto* h = new QHBoxLayout(r); h->setContentsMargins(4, 2, 4, 2);
      h->addWidget(new QLabel(rp.img.isEmpty() ? "▭" : "🖼")); h->addWidget(new QLabel(rp.name + (rp.img.isEmpty() ? QObject::tr(" (no image)") : QString())), 1);
      auto* eye = new QPushButton; eye->setIcon(ui::eyeIcon(rp.visible)); eye->setFlat(true); eye->setFixedWidth(26); eye->setToolTip(QObject::tr("Show / hide"));
      QObject::connect(eye, &QPushButton::clicked, this, [this, id] { RefPlane* p = vp_->refPlane(id); if (!p) return; p->visible = !p->visible; if (!p->visible && vp_->activeRefPlane() < 0) vp_->setActiveRefPlane(-1); edited(); rebuild(); });
      auto* del = new QPushButton("✖"); del->setFlat(true); del->setFixedWidth(26); del->setToolTip(QObject::tr("Delete the plane"));
      QObject::connect(del, &QPushButton::clicked, this, [this, id] { auto& L = sc_->refPlanes; L.erase(std::remove_if(L.begin(), L.end(), [id](const RefPlane& x) { return x.id == id; }), L.end()); edited(); rebuild(); });
      h->addWidget(eye); h->addWidget(del);
      r->click = [this, id] { vp_->setActiveRefPlane(vp_->activeRefPlane() == id ? -1 : id); rebuild(); };
      v->addWidget(r);
    }
    RefPlane* rp = active();
    if (rp) {
      const int id = rp->id;
      v->addWidget(h5(rp->name));
      auto* hint = new QLabel(QObject::tr("✥ Hold Ctrl + Alt and drag in the viewport to move the image (whatever is under the pointer)")); hint->setWordWrap(true); hint->setStyleSheet("color:#8a8a8a;font-size:11px"); v->addWidget(hint);
      auto P = [this, id]() { return vp_->refPlane(id); };
      auto* r3 = new QHBoxLayout; v->addLayout(r3);
      auto* im = b(rp->img.isEmpty() ? QObject::tr("🖼 Image…") : QObject::tr("🖼 Replace image"), QObject::tr("Put an image on the plane (its proportions are kept, the height stays)"), [this, id] {
        // (the file window from the main window, the menu closed first: a popup is closed — and deleted — by any window
        // opened over it, the file window's parent gone with it; the menu opened again after)
        QWidget* host = parentWidget() ? parentWidget()->window() : nullptr; Viewport* vp = vp_; Scene* sc = sc_; RefBoard* board = board_; const QPoint at = pos();
        close();
        QTimer::singleShot(0, host, [host, vp, sc, board, at, id] {
          QSettings st; QString f = QFileDialog::getOpenFileName(host, QObject::tr("Reference image"), st.value("refs/dir").toString(), QObject::tr("Images (*.png *.jpg *.jpeg *.webp *.bmp *.gif)"));
          if (!f.isEmpty()) { st.setValue("refs/dir", QFileInfo(f).absolutePath());
            QString url; int w, h;
            if (!refImageFromFile(f, &url, &w, &h)) ui::toast(QObject::tr("Not an image"));
            else if (RefPlane* p = vp->refPlane(id)) { const Camera& cam = vp->refHomeCamera(*p); refs::apply(*p, cam);
              double a = double(w) / h; p->img = url; p->scl.x = p->scl.y * a * (p->scl.x < 0 ? -1 : 1); p->aspect = a; refs::capture(*p, cam); vp->refPlanesEdited(true); } }
          openRefMenu(host, vp, sc, board, at); });
      });
      im->setProperty("primary", true); r3->addWidget(im);
      r3->addWidget(b(QObject::tr("⊕ Centre"), QObject::tr("Move the plane so its pivot (the centre of the image unless you moved the pivot) is the centre of the view (the model's centre)"), [this, P] { if (RefPlane* p = P()) { const Camera& c = vp_->refHomeCamera(*p); refs::apply(*p, c); refs::centre(*p, *sc_, c); edited(); } }));
      r3->addWidget(b(QObject::tr("↺ Reset"), QObject::tr("Back to the view's orientation, place and size"), [this, P] { if (RefPlane* p = P()) { refs::place(*p, *sc_, vp_->refHomeCamera(*p)); edited(); } }));
      auto turn = [this, P](double a) { if (RefPlane* p = P()) { const Camera& c = vp_->refHomeCamera(*p); refs::apply(*p, c); p->quat = (p->quat * Quat::fromAxisAngle({0, 0, 1}, a)).normalized(); refs::capture(*p, c); edited(); } };
      auto* r4 = new QHBoxLayout; v->addLayout(r4);
      r4->addWidget(b(QObject::tr("⟲ 90°"), QObject::tr("Turn the image 90° to the left"), [turn] { turn(M_PI / 2); }));
      r4->addWidget(b(QObject::tr("⟳ 90°"), QObject::tr("Turn the image 90° to the right"), [turn] { turn(-M_PI / 2); }));
      r4->addWidget(b(QObject::tr("⇋ Mirror"), QObject::tr("Mirror the image left ↔ right"), [this, P] { if (RefPlane* p = P()) { const Camera& c = vp_->refHomeCamera(*p); refs::apply(*p, c); p->scl.x *= -1; refs::capture(*p, c); edited(); } }));
      auto* r7 = new QHBoxLayout; r7->setSpacing(3); v->addLayout(r7); auto* cl = new QLabel(QObject::tr("⧉ Copy to:")); cl->setProperty("mut", true); r7->addWidget(cl);
      for (const char* view : refs::VIEWS) {
        QString vw = view; if (vw == rp->view) continue; bool mir = refs::opposite(rp->view) == vw;
        auto* x = b(refs::viewLabel(vw) + (mir ? " ⇋" : ""), QObject::tr("A copy of this plane for the %1 view: the same image, size and place on the model").arg(refs::viewLabel(vw)) + (mir ? QObject::tr(" — mirrored (the opposite side)") : QString()), [this, P, vw, mir] {
          RefPlane* p = P(); if (!p) return; int n = 1; for (auto& r : sc_->refPlanes) if (r.view == vw) n++;
          RefPlane t = refs::copyTo(*p, vw, *sc_, vp_->refHomeCamera(*p), sc_->nextRefId++, QString("%1 %2").arg(refs::viewLabel(vw)).arg(n));
          QString from = p->name; sc_->refPlanes.push_back(t); edited();
          ui::toast(QString("%1 → %2%3").arg(from, t.name, mir ? QObject::tr(" (mirrored)") : QString()));
        });
        x->setStyleSheet("padding:2px 5px"); r7->addWidget(x);
      }
      r7->addStretch(1);
      auto* fr = new QHBoxLayout; v->addLayout(fr); fr->addWidget(new QLabel(QObject::tr("Opacity")));
      auto* op = new QSlider(Qt::Horizontal); op->setRange(2, 100); op->setValue(int(std::lround(rp->opacity * 100))); fr->addWidget(op, 1);
      auto* ov = new QLabel(QString("%1%").arg(op->value())); ov->setFixedWidth(34); fr->addWidget(ov);
      QObject::connect(op, &QSlider::valueChanged, this, [this, P, ov](int x) { if (RefPlane* p = P()) { p->opacity = x / 100.0; ov->setText(QString("%1%").arg(x)); vp_->refPlanesEdited(false); } });
      QObject::connect(op, &QSlider::sliderReleased, this, [this] { edited(); });
      auto chk = [&](const QString& lab, const QString& tip, bool on, std::function<void(bool)> fn) { auto* c = new QCheckBox(lab); c->setToolTip(tip); c->setChecked(on); QObject::connect(c, &QCheckBox::toggled, this, [this, fn](bool x) { fn(x); rebuild(); }); v->addWidget(c); };
      chk(QObject::tr("Only in the %1 view").arg(refs::viewLabel(rp->view)), QObject::tr("Show the plane only in its own view (front plane — only in the Front view …)"), rp->onlyView, [this, P](bool x) { if (RefPlane* p = P()) { p->onlyView = x; edited(); } });
      if (rp->follows()) {
        chk(QObject::tr("📌 Lock to the model"), QObject::tr("Place and scale the image with the gizmo, then lock it: moving the view (pan) and zooming keep it on the model. Only orbiting can put the view off — \"↩ Locked view\" returns to it"), rp->lock.isObject(), [this, P](bool x) {
          if (RefPlane* p = P()) { refs::setLock(*p, vp_->refHomeCamera(*p), x); edited();
            ui::toast(x ? QObject::tr("Locked: panning and zooming keep the image on the model; orbiting turns it with the camera — \"↩ Locked view\" brings the view back") : QObject::tr("Unlocked: the image stays put on the screen")); } });
        if (rp->lock.isObject()) v->addWidget(b(QObject::tr("↩ Locked view"), QObject::tr("Back to the view of the moment the image was locked (direction, place and zoom)"), [this, P] {
          RefPlane* p = P(); if (!p) return; QJsonObject lv = p->lock.toObject().value("view").toObject();
          if (vp_->camera().view != "ortho") vp_->setViewMode("ortho");
          Camera& c = vp_->camera(); auto vec = [](const QJsonValue& x) { QJsonArray a = x.toArray(); return Vec3(a[0].toDouble(), a[1].toDouble(), a[2].toDouble()); };
          c.pos = vec(lv.value("pos")); QJsonArray q = lv.value("quat").toArray(); c.quat = Quat{q[0].toDouble(), q[1].toDouble(), q[2].toDouble(), q[3].toDouble()}.normalized();
          c.pivot = vec(lv.value("pivot")); if (lv.value("zoom").isDouble()) c.zoom = lv.value("zoom").toDouble(); c.up = {0, 1, 0};
          vp_->liveView(); vp_->updateGizmo();
        }));
      }
      chk(QObject::tr("Over the model"), QObject::tr("Draw the image over the model (not hidden behind it) — set Opacity to see the model through it"), rp->over, [this, P](bool x) { if (RefPlane* p = P()) { p->over = x; edited(); } });
      auto* r5 = new QHBoxLayout; v->addLayout(r5);
      r5->addWidget(b(QObject::tr("✥ Move (E)"), {}, [this] { vp_->setTool("move"); }, vp_->tool() == "move"));
      r5->addWidget(b(QObject::tr("⟳ Rotate (R)"), {}, [this] { vp_->setTool("rotate"); }, vp_->tool() == "rotate"));
      r5->addWidget(b(QObject::tr("⬚ Scale (T)"), {}, [this] { vp_->setTool("gizmo"); }, vp_->tool() == "gizmo"));
      auto* r6 = new QHBoxLayout; v->addLayout(r6);
      r6->addWidget(b(vp_->refPivEdit() ? QObject::tr("🔓 Pivot — move it, then lock") : QObject::tr("🔒 Pivot"), QObject::tr("Unlock to move the gizmo's pivot onto the spot of the picture you want to rotate / scale about (it stays on that spot of the image)"), [this] { vp_->setRefPivEdit(!vp_->refPivEdit()); }, vp_->refPivEdit()));
      r6->addWidget(b(QObject::tr("⊙ Pivot to centre"), QObject::tr("The pivot back to the centre of the image"), [this, P] { if (RefPlane* p = P()) { p->piv[0] = p->piv[1] = 0; edited(); } }));
      auto* note = new QLabel(QObject::tr("While the plane is active here, the gizmo works on it (W — local / world axes). Selecting anything in the scene lets it go. Del — delete it."));
      note->setWordWrap(true); note->setStyleSheet("color:#8a8a8a;font-size:11px;margin-top:6px"); v->addWidget(note);
    }
  }
  Viewport* vp_; Scene* sc_; RefBoard* board_; QVBoxLayout* v_; QWidget* content_ = nullptr; bool choose_ = false;
};
QPointer<RefMenu> g_menu;

}  // namespace

void openRefMenu(QWidget* parent, Viewport* vp, Scene* sc, RefBoard* board, const QPoint& global) {
  if (g_menu) g_menu->close();
  g_menu = new RefMenu(parent, vp, sc, board); g_menu->adjustSize(); g_menu->move(global); g_menu->show();
}
void closeRefMenu() { if (g_menu) g_menu->close(); }
bool refMenuOpen() { return g_menu && g_menu->isVisible(); }

void attachRefIcons(QWidget* window, QWidget* vpBox, Viewport* vp, Scene* sc, const std::function<void(const QString&)>& pickTool) {
  auto* w = new QWidget(window, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus | Qt::NoDropShadowWindowHint);
  w->setAttribute(Qt::WA_TranslucentBackground); w->setAttribute(Qt::WA_ShowWithoutActivating);
  auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 0, 0, 0); h->setSpacing(4);
  auto icon = [&](const QString& t) { auto* b = new QPushButton(t); b->setFixedSize(26, 26); b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet("QPushButton{border-radius:13px;padding:0;font-size:13px;background:rgba(40,40,40,220);border:1px solid #555} QPushButton:checked{background:#3a5585}"); h->addWidget(b); return b; };
  auto* lock = icon("🔒"); lock->setCheckable(true);
  auto* cen = icon("⊕"); cen->setToolTip(QObject::tr("Centre: the pivot (with the picture) onto the centre of the view"));
  QObject::connect(lock, &QPushButton::clicked, vp, [vp, pickTool] {
    bool on = !vp->refPivEdit(); vp->setRefPivEdit(on); if (on) pickTool("move");
    ui::toast(on ? QObject::tr("Pivot unlocked: the gizmo moves only the pivot point — put it where to rotate / scale from, then lock it") : QObject::tr("Pivot locked: rotate (R) and scale (T) work about it"));
  });
  QObject::connect(cen, &QPushButton::clicked, vp, [vp, sc] { int id = vp->activeRefPlane(); RefPlane* p = id >= 0 ? vp->refPlane(id) : nullptr; if (!p) return;
    const Camera& c = vp->refHomeCamera(*p); refs::apply(*p, c); refs::centre(*p, *sc, c); vp->refPlanesEdited(true); vp->updateGizmo(); });
  auto* t = new QTimer(w); t->setInterval(80);
  QObject::connect(t, &QTimer::timeout, w, [w, vpBox, vp, lock] {
    const QString tl = vp->tool(); bool gz = tl == "move" || tl == "rotate" || tl == "gizmo";
    auto s = vp->activeRefPlane() >= 0 && gz && vpBox->isVisible() && vpBox->window()->isActiveWindow() ? vp->gizmoScreen() : std::nullopt;
    if (!s || QApplication::mouseButtons() != Qt::NoButton) { if (w->isVisible() && !s) w->hide(); return; }
    QPoint g = vp->toGlobal(*s) + QPoint(70, -120);
    QRect box(vpBox->mapToGlobal(QPoint(0, 0)), vpBox->size()); if (!box.contains(g)) { w->hide(); return; }
    bool on = vp->refPivEdit(); { QSignalBlocker b(lock); lock->setChecked(on); } lock->setText(on ? "🔓" : "🔒");
    lock->setToolTip(on ? QObject::tr("Pivot unlocked — the gizmo moves the pivot. Click to lock") : QObject::tr("Pivot locked. Click to unlock and move the pivot"));
    w->adjustSize(); if (w->pos() != g) w->move(g); if (!w->isVisible()) w->show();
  });
  t->start();
}

}  // namespace hm