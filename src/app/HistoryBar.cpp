// The History slider — see HistoryBar.h.
#include "app/HistoryBar.h"
#include "app/Overlays.h"
#include "view/Viewport.h"
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace hm {

// the steps as cells side by side (as many as fit — narrower when there are many); the current one lit, the later ones dim
class HistoryBar::Strip : public QWidget {
public:
  explicit Strip(Viewport* vp) : vp_(vp) { setMouseTracking(true); setMinimumHeight(30); setCursor(Qt::PointingHandCursor); }
  // (the steps shown: the Inspector's "Steps shown" of them round the current one)
  std::pair<int, int> span() const { const int n = vp_->historySize(), cnt = std::min(n, std::max(1, overlays::cfg().histShown));
    const int first = std::clamp(vp_->historyIndex() - cnt / 2, 0, std::max(0, n - cnt)); return {first, cnt}; }
  int at(double x) const { const auto [first, cnt] = span(); if (cnt <= 0) return -1; return first + std::clamp(int(x / (double(width()) / cnt)), 0, cnt - 1); }
protected:
  void paintEvent(QPaintEvent*) override {
    QPainter g(this); g.setRenderHint(QPainter::Antialiasing);
    const int n = vp_->historySize(), cur = vp_->historyIndex(); if (n <= 0) return;
    const auto [first, cnt] = span();
    const double w = double(width()) / cnt, gap = w > 6 ? 2 : w > 3 ? 1 : 0;
    for (int i = first; i < first + cnt; i++) {
      const QRectF r((i - first) * w + gap / 2, 4, std::max(1.0, w - gap), height() - 8);
      QColor c = i == cur ? QColor(0x5a, 0x8d, 0xee) : i < cur ? QColor(0x3a, 0x45, 0x68) : QColor(0x2a, 0x2e, 0x3a);
      if (i == hover_) c = c.lighter(140);
      g.setPen(Qt::NoPen); g.setBrush(c); g.drawRoundedRect(r, std::min(3.0, w / 3), std::min(3.0, w / 3));
    }
    // (the marker over the current step)
    const double x = (cur - first + 0.5) * w; g.setBrush(QColor(0xff, 0xff, 0xff)); g.drawPolygon(QPolygonF({QPointF(x - 4, 0), QPointF(x + 4, 0), QPointF(x, 5)}));
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    const int i = at(e->position().x());
    if (i != hover_) { hover_ = i; update(); }
    if (i >= 0) QToolTip::showText(e->globalPosition().toPoint(), QObject::tr("%1. %2").arg(i).arg(vp_->historyLabel(i)), this);
    if (dragging_ && i >= 0) vp_->historyGoTo(i);
  }
  void leaveEvent(QEvent*) override { hover_ = -1; update(); }
  void mousePressEvent(QMouseEvent* e) override {
    const int i = at(e->position().x()); if (i < 0) return;
    if (e->button() == Qt::LeftButton) { dragging_ = true; vp_->historyGoTo(i); return; }
    if (e->button() == Qt::RightButton) {
      QMenu m; m.setStyleSheet("QMenu{background:#1f2638;color:#dde;border:1px solid #3a4568} QMenu::item:selected{background:#3b6fd8} QMenu::item:disabled{color:#778}");
      m.addSection(QObject::tr("%1. %2").arg(i).arg(vp_->historyLabel(i)));
      auto* go = m.addAction(QObject::tr("⏵ Go to this step"));
      auto* del = m.addAction(QObject::tr("✖ Delete this history step (the later ones stay)"));
      del->setEnabled(i > 0); if (i == 0) del->setToolTip(QObject::tr("The project as it was opened stays"));
      QAction* a = m.exec(e->globalPosition().toPoint());
      if (a == go) vp_->historyGoTo(i); else if (a == del) vp_->historyDelete(i);
    }
  }
  void mouseReleaseEvent(QMouseEvent*) override { dragging_ = false; }
private:
  Viewport* vp_; int hover_ = -1; bool dragging_ = false;
};

HistoryBar::HistoryBar(QWidget* host, Viewport* vp) : QFrame(host->window(), Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint), host_(host), vp_(vp) {
  setAttribute(Qt::WA_TranslucentBackground); setObjectName("histWin");
  auto* outer = new QVBoxLayout(this); outer->setContentsMargins(0, 0, 0, 0);
  auto* body = new QFrame; body->setObjectName("histBody"); outer->addWidget(body);
  auto* v = new QVBoxLayout(body); v->setContentsMargins(12, 6, 12, 8); v->setSpacing(2);
  auto* head = new QHBoxLayout; head->setSpacing(8);
  title_ = new QLabel(tr("History")); head->addWidget(title_);
  now_ = new QLabel; head->addWidget(now_, 1);
  warn_ = new QLabel; head->addWidget(warn_);
  // (no Close / undo / redo buttons: the Inspector's button shows and hides it, Ctrl+Z / Ctrl+Shift+Z step)
  v->addLayout(head);
  strip_ = new Strip(vp); strip_->setToolTip(QString()); v->addWidget(strip_);
  hint_ = new QLabel(tr("Click or drag — go to a step · RMB on a step — delete it (what it did is undone, the later steps stay)"));
  hint_->setObjectName("histHint"); v->addWidget(hint_);
  connect(vp, &Viewport::historyChanged, this, [this] { if (isVisible()) sync(); });
  applyStyle();
}

// the look from the Inspector (the background's colour and opacity, the font's size, weight and opacity)
void HistoryBar::applyStyle() {
  const auto& s = overlays::cfg().histS;
  auto rgba = [](QColor c, double a) { return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(int(std::lround(std::clamp(a, 0.0, 1.0) * 255))); };
  const double t = s.textOp; const QString pt = QString::number(s.size, 'f', 1);
  setStyleSheet(QString("#histBody{background:%1;border:0px solid %2;border-radius:10px}"
                        "#histBody QLabel{color:%3;font-size:%4pt;font-weight:%5}"
                        "#histBody QLabel#histHint{color:%6;font-size:%7pt;font-weight:400}"
                        "#histBody QPushButton{background:#1f2638;border:1px solid #3a4568;color:#dde;border-radius:5px;padding:1px 8px}")
                    .arg(rgba(s.bg, s.bgOp), QString("transparent"), rgba(QColor(0xc8, 0xcb, 0xd8), t), pt).arg(s.weight)
                    .arg(rgba(QColor(0x7c, 0x82, 0x96), t), QString::number(std::max(6.0, s.size - 1.5), 'f', 1)));
  title_->setStyleSheet(QString("font-weight:%1;color:%2").arg(std::max(600, s.weight)).arg(rgba(Qt::white, t)));
  warn_->setStyleSheet(QString("color:%1").arg(rgba(QColor(0xf0, 0xb8, 0x60), t)));
  sync();
  if (isVisible()) place();
}

void HistoryBar::sync() {
  const int n = vp_->historySize(), cur = vp_->historyIndex();
  now_->setText(n <= 0 ? QString() : overlays::cfg().histSteps ? tr("Step %1 of %2: %3").arg(cur).arg(n - 1).arg(vp_->historyLabel(cur)) : vp_->historyLabel(cur));
  const int later = n - 1 - cur;
  warn_->setText(later > 0 ? tr("⚠ An edit made now overwrites the %1 later step%2").arg(later).arg(later == 1 ? "" : "s") : QString());
  strip_->update();
}

QSize HistoryBar::sizeFor(QWidget* host, int height) { return {std::clamp((host ? host->width() : 1200) - 80, 320, 1100), height}; }

// the viewport's bottom middle, or where it was put in the Inspector's preview (a fraction of the free room)
void HistoryBar::place() {
  if (!host_) return;
  const QSize sz = sizeFor(host_, sizeHint().height());
  resize(sz);
  const QPointF f = overlays::cfg().histPos;
  QPoint q((host_->width() - sz.width()) / 2, host_->height() - sz.height() - 14);
  if (f.x() >= 0) q = QPoint(int(f.x() * std::max(0, host_->width() - sz.width())), int(f.y() * std::max(0, host_->height() - sz.height())));
  move(host_->mapToGlobal(q));
}

}  // namespace hm
