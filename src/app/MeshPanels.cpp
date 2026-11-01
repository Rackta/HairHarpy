#include "app/MeshPanels.h"
#include "app/ModPanels.h"
#include "app/TexWindow.h"
#include "app/ToolPanels.h"
#include "scene/Looks.h"
#include "scene/MeshMods.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QVBoxLayout>
#include <cmath>
#include <set>

namespace hm {

namespace {
QLabel* h4(const QString& t) { auto* l = new QLabel(t); l->setProperty("h4", true); l->setWordWrap(true); return l; }
QLabel* muted(const QString& t) { auto* l = new QLabel(t); l->setProperty("mut", true); l->setWordWrap(true); return l; }
QPushButton* button(const QString& t, const QString& tip = {}) { auto* b = new QPushButton(t); b->setToolTip(tip); b->setFocusPolicy(Qt::NoFocus); return b; }
QString fmtLen(double v) {
  if (v == 0) return "0.00";
  int e = int(std::floor(std::log10(std::abs(v))));
  if (e < -6 || e >= 3) { QString s = QString::number(v, 'e', 2); s.replace(QRegularExpression("e([+-])0*(\\d)"), "e\\1\\2"); return s; }
  return QString::number(v, 'f', std::max(0, 2 - e));
}
QWidget* hbox(std::initializer_list<QWidget*> ws) { auto* w = new QWidget; auto* h = new QHBoxLayout(w); h->setContentsMargins(0, 4, 0, 0); h->setSpacing(4); for (QWidget* x : ws) h->addWidget(x); h->addStretch(1); return w; }
QString kindName(const QString& k) { return k == "xform" ? QObject::tr("Transform") : k == "thick" ? QObject::tr("Thickness") : k == "mask" ? QObject::tr("Mask") : k; }
}  // namespace

void imeshExtras(QVBoxLayout* v, Viewport* vp, Scene* sc, int id) {
  const Item* m = sc->item(id); const PolyMesh* B = m->base ? m->base.get() : m->poly.get(); if (!B) return;
  if (looks::hasUV(*m)) {                                     // textures (the mesh has UVs)
    auto* tb = button(QObject::tr("▦ Textures…"), QObject::tr("Colour / opacity maps of every material of this mesh, the texture folder (also: double-click ▦ in the outliner)"));
    QWidget* host = v->parentWidget();
    QObject::connect(tb, &QPushButton::clicked, vp, [vp, sc, id, host] { openTexWindow(host, vp, sc, id); });
    v->addWidget(tb, 0, Qt::AlignLeft);
    QJsonObject L = looks::lookOf(*m);
    v->addWidget(muted(QObject::tr("Texture sets: %1 · textures %2%3").arg(looks::setsOf(*m).size()).arg(L.value("tex").toBool() ? QObject::tr("on") : QObject::tr("off")).arg(L.value("flat").toBool() ? QObject::tr(" · flat (unlit)") : QString())));
  } else v->addWidget(muted(QObject::tr("No UVs in the file — no textures for this mesh.")));
  // its polygroups: each one shown / hidden — one mesh, the groups inside it
  if (B->groupNames.size() > 1 && B->faceGroup.size() == size_t(B->faceCount())) {
    std::set<int> hid; for (const QJsonValue& x : m->raw.value("hidGroups").toArray()) hid.insert(x.toInt());
    std::vector<int> cnt(B->groupNames.size(), 0); for (int g : B->faceGroup) if (g >= 0 && size_t(g) < cnt.size()) cnt[size_t(g)]++;
    auto* box = new QWidget; auto* bl = new QVBoxLayout(box); bl->setContentsMargins(4, 2, 4, 2); bl->setSpacing(1);
    bl->addWidget(muted(QObject::tr("Polygroups (%1)").arg(B->groupNames.size())));
    for (size_t gi = 0; gi < B->groupNames.size(); gi++) {
      auto* r = new QWidget; auto* h = new QHBoxLayout(r); h->setContentsMargins(0, 0, 0, 0);
      auto* n = new QLabel(B->groupNames[gi]); n->setToolTip(QObject::tr("%1 polygons").arg(cnt[gi])); h->addWidget(n, 1);
      bool off = hid.count(int(gi)); auto* eye = button("", QObject::tr("Show / hide this polygroup")); eye->setIcon(ui::eyeIcon(!off)); eye->setFlat(true);
      int g = int(gi);
      QObject::connect(eye, &QPushButton::clicked, vp, [vp, id, g, off] { vp->runEdit([id, g, off](Scene& s, Selection&, const CurveCache&) {
        Item* mm = s.item(id); std::set<int> hs; for (const QJsonValue& x : mm->raw.value("hidGroups").toArray()) hs.insert(x.toInt());
        if (off) hs.erase(g); else hs.insert(g); QJsonArray a; for (int x : hs) a.append(x); mm->raw.insert("hidGroups", a); return QString(); }, false); });
      h->addWidget(eye); bl->addWidget(r);
    }
    auto* sa = new QScrollArea; sa->setWidget(box); sa->setWidgetResizable(true); sa->setMaximumHeight(220); sa->setFrameShape(QFrame::StyledPanel);
    v->addWidget(sa);
  }
  auto mods = meshmods::modsOf(*sc, id);
  QStringList ml; for (int x : mods) { const Item* xi = sc->item(x); ml << kindName(xi->raw.value("kind").toString()) + (xi->visible ? QString() : QObject::tr(" (off)")); }
  if (meshmods::heavy(*m)) {                                  // (a heavy mesh: nothing to edit — Move / Rotate / Scale still work)
    auto* l = muted(QObject::tr("Over 300 000 triangles: no wireframe and not editable (it can be moved, rotated, scaled).")); l->setStyleSheet("color:#e0b060"); v->addWidget(l);
    if (!ml.isEmpty()) v->addWidget(muted(QObject::tr("Modifiers: ") + ml.join(", ")));
    return;
  }
  // one Thickness modifier (with its mask inside): added, or selected when there is one
  int has = -1; for (int x : mods) if (sc->item(x)->raw.value("kind").toString() == "thick") has = x;
  auto* b = button(has >= 0 ? QObject::tr("⬤ Thickness & mask…") : QObject::tr("+ ⬤ Thickness & mask"), QObject::tr("Paint where the mesh gets thicker or thinner, and a mask that holds it back — one modifier, two brushes"));
  QObject::connect(b, &QPushButton::clicked, vp, [vp, id, has] {
    if (has >= 0) { vp->setSelectedItems({has}); return; }
    vp->runEdit([id](Scene& s, Selection& sl, const CurveCache&) { QString msg; int x = meshmods::newMeshExtra(s, id, "thick", &msg); if (x < 0) return "!" + msg; sl.anchors.clear(); sl.items = {x};
      return QObject::tr("Thickness added — set the intensity, then paint (Ctrl — inverted, Ctrl+Alt — erase, Shift — smooth); its Mask brush holds it back"); }, true);
    brushFor(vp, "thick");
  });
  v->addWidget(b, 0, Qt::AlignLeft);
  if (!ml.isEmpty()) v->addWidget(muted(QObject::tr("Modifiers: ") + ml.join(", ")));
}

void meshXmodProps(QVBoxLayout* v, Viewport* vp, Scene* sc, int id) {
  const Item* x = sc->item(id); const QString kind = x->raw.value("kind").toString(); const int mid = x->raw.value("mesh").toInt(-1); const Item* m = sc->item(mid);
  v->addWidget(muted(QObject::tr("Modifier of %1. The eye in the outliner switches it off.").arg(m ? m->name : QObject::tr("(deleted mesh)"))));
  auto setP = [vp, id](const QString& k, double val, bool commit) {
    vp->runEdit([id, k, val](Scene& s, Selection&, const CurveCache&) { Item* it = s.item(id); QJsonObject p = it->raw.value("p").toObject(); p.insert(k, val); it->raw.insert("p", p); return QString(); }, false);
    (void)commit;
  };
  if (kind == "xform") {
    QJsonObject p = x->raw.value("p").toObject(); double st = m ? std::pow(10, std::floor(std::log10(meshmods::baseSize(*m) / 100))) : 0.01;
    auto row = [&](const QString& label, std::initializer_list<const char*> keys, double step, int dec) {
      auto* r = new QWidget; auto* h = new QHBoxLayout(r); h->setContentsMargins(0, 2, 0, 2); h->setSpacing(4);
      auto* l = new QLabel(label); l->setProperty("mut", true); l->setFixedWidth(70); h->addWidget(l);
      for (const char* k : keys) {
        auto* sb = new QDoubleSpinBox; sb->setRange(-1e9, 1e9); sb->setDecimals(dec); sb->setSingleStep(step); sb->setValue(p.value(k).toDouble(k[0] == 's' ? 1 : 0));
        sb->setKeyboardTracking(false); QString kk = k;
        QObject::connect(sb, &QDoubleSpinBox::valueChanged, vp, [setP, kk](double val) { setP(kk, val, true); });
        h->addWidget(sb, 1);
      }
      v->addWidget(r);
    };
    row(QObject::tr("Position"), {"px", "py", "pz"}, st, 4); row(QObject::tr("Rotation °"), {"rx", "ry", "rz"}, 1, 2); row(QObject::tr("Scale"), {"sx", "sy", "sz"}, 0.05, 3);
    auto* rb = button(QObject::tr("Reset"), QObject::tr("Back to the imported position, rotation and size"));
    QObject::connect(rb, &QPushButton::clicked, vp, [vp, id] { vp->runEdit([id](Scene& s, Selection&, const CurveCache&) { Item* it = s.item(id); QJsonObject p = it->raw.value("p").toObject();
      for (const char* k : {"px", "py", "pz", "rx", "ry", "rz"}) p.insert(k, 0); for (const char* k : {"sx", "sy", "sz"}) p.insert(k, 1); it->raw.insert("p", p); return QString(); }, false); emit vp->brushChanged(); });
    v->addWidget(rb, 0, Qt::AlignLeft);
    v->addWidget(muted(QObject::tr("Move (E), Rotate (R) and Scale (T) on the mesh write here. Rotation and scale are about the mesh's centre.")));
    return;
  }
  auto weights = [vp, id](const char* key, std::function<double(double)> f) {
    vp->runEdit([id, key, f](Scene& s, Selection&, const CurveCache&) { Item* it = s.item(id); QJsonArray a = it->raw.value(key).toArray(), o;
      if (a.isEmpty()) a = it->raw.value("mask").toArray();
      for (const QJsonValue& w : a) o.append(f(w.toDouble())); it->raw.insert(key, o); return QString(); }, false);
  };
  auto brushBtn = [vp](const QString& k, const QString& label) { auto* b = new QPushButton(label); b->setIcon(ui::icon("brush_paint")); b->setIconSize(QSize(18, 18)); b->setCheckable(true); b->setChecked(vp->maskBrush.on && vp->maskBrush.kind == k);
    QObject::connect(b, &QPushButton::clicked, vp, [vp, k] { brushFor(vp, k); }); return b; };
  if (kind == "thick") {
    QJsonObject p = x->raw.value("p").toObject(); double amt = p.value("amt").toDouble(), ref = p.value("amtRef").toDouble(amt); if (!(ref > 0)) ref = m ? meshmods::baseSize(*m) * 0.02 : 0.01;
    double mx = std::max(ref * 4, amt * 1.05);
    auto* r = new SliderRow(QObject::tr("Amount"), 0, mx, ref / 100, amt, fmtLen); r->setTip(QObject::tr("How far a weight of ±100% pushes the surface out (orange) or in (blue) along its normals"));
    QObject::connect(r, &SliderRow::changed, vp, [vp, id](double val) { Item* it = vp->scene()->item(id); QJsonObject pp = it->raw.value("p").toObject(); pp.insert("amt", val); it->raw.insert("p", pp); vp->requestUpdate(); });
    QObject::connect(r, &SliderRow::released, vp, [vp](double) { vp->commitLive(); });
    v->addWidget(r);
    v->addWidget(muted(QObject::tr("Painted thickness: orange — outwards, blue — inwards. The colours show only while you work on it (this modifier selected or its brush on).")));
    auto* cl = button(QObject::tr("Clear")); QObject::connect(cl, &QPushButton::clicked, vp, [weights] { weights("mask", [](double) { return 0.0; }); });
    v->addWidget(hbox({cl, brushBtn("thick", QObject::tr("🖌 Thickness brush"))}));
    v->addWidget(h4(QObject::tr("◐ Mask")));
    v->addWidget(muted(QObject::tr("Painted mask (dark): where it is 100% the thickness does nothing.")));
    auto* c2 = button(QObject::tr("Clear")); QObject::connect(c2, &QPushButton::clicked, vp, [weights] { weights("hold", [](double) { return 0.0; }); });
    auto* f2 = button(QObject::tr("Fill")); QObject::connect(f2, &QPushButton::clicked, vp, [weights] { weights("hold", [](double) { return 1.0; }); });
    auto* i2 = button(QObject::tr("Invert")); QObject::connect(i2, &QPushButton::clicked, vp, [weights] { weights("hold", [](double w) { return 1 - w; }); });
    v->addWidget(hbox({c2, f2, i2, brushBtn("mask", QObject::tr("🖌 Mask brush"))}));
    return;
  }
  v->addWidget(muted(QObject::tr("Painted mask (dark): where it is 100% the Thickness modifiers of this mesh do nothing.")));
  auto* c = button(QObject::tr("Clear")); QObject::connect(c, &QPushButton::clicked, vp, [weights] { weights("mask", [](double) { return 0.0; }); });
  auto* f = button(QObject::tr("Fill")); QObject::connect(f, &QPushButton::clicked, vp, [weights] { weights("mask", [](double) { return 1.0; }); });
  auto* i = button(QObject::tr("Invert")); QObject::connect(i, &QPushButton::clicked, vp, [weights] { weights("mask", [](double w) { return 1 - w; }); });
  v->addWidget(hbox({c, f, i, brushBtn("mask", QObject::tr("🖌 Mask brush"))}));
}

}  // namespace hm
