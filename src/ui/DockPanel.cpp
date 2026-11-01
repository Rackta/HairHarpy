#include "ui/DockPanel.h"
#include <QPointer>
#include <QScreen>
#include <QTextDocument>
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QStyle>
#include <QVBoxLayout>

namespace hm::ui {

DockPanel::DockPanel(const QString& title, DockColumn* column, bool collapsible, bool pin, bool grow)
    : QFrame(column), column_(column), grow_(grow) {
  setObjectName("dpanel");
  setStyleSheet("#dpanel{background:#262626;border-bottom:1px solid #3a3a3a}"
                "#dpanel[float=\"true\"]{border:1px solid #555;border-radius:6px}"
                "#dhead{background:#2b2b2b;border-bottom:1px solid #3a3a3a}");
  auto* v = new QVBoxLayout(this); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(0);
  head_ = new QWidget; head_->setObjectName("dhead"); head_->setFixedHeight(28); head_->setCursor(Qt::SizeAllCursor);
  auto* h = new QHBoxLayout(head_); h->setContentsMargins(collapsible ? 4 : 12, 0, 6, 0); h->setSpacing(6);
  if (collapsible) {
    colBtn_ = new QPushButton("▾"); colBtn_->setToolTip(tr("Collapse / expand")); colBtn_->setCursor(Qt::PointingHandCursor);
    colBtn_->setStyleSheet("QPushButton{background:none;border:none;color:#aaa;padding:0 4px}QPushButton:hover{color:#fff}");
    connect(colBtn_, &QPushButton::clicked, this, [this] { setCollapsed(!collapsed_); });
    h->addWidget(colBtn_);
  }
  title_ = new QLabel(title); h->addWidget(title_, 1);
  if (pin) {
    pinBtn_ = new QPushButton("📌"); pinBtn_->setCheckable(true); pinBtn_->setCursor(Qt::PointingHandCursor);
    pinBtn_->setToolTip(tr("Pin: keep showing this item's settings — selecting something else doesn't switch them (click again to unpin)"));
    pinBtn_->setStyleSheet("QPushButton{background:none;border:none;padding:0 4px;color:#777}QPushButton:checked{color:#fff;background:none}");
    connect(pinBtn_, &QPushButton::toggled, this, [this](bool on) { pinned_ = on; emit pinToggled(on); });
    h->addWidget(pinBtn_);
  }
  // long help texts (a grey paragraph of 90+ characters) fold under this ? (foldHelp): hover — read them, click — keep them open
  helpBtn_ = new QPushButton("?"); helpBtn_->setFixedSize(18, 18); helpBtn_->setCursor(Qt::PointingHandCursor); helpBtn_->setCheckable(true);
  helpBtn_->setStyleSheet("QPushButton{border:1px solid #666;border-radius:9px;padding:0;font-size:10px;color:#aaa;background:none}QPushButton:hover,QPushButton:checked{color:#fff;border-color:#aaa}");
  helpBtn_->installEventFilter(this); helpBtn_->hide(); h->addWidget(helpBtn_);
  connect(helpBtn_, &QPushButton::toggled, this, [this](bool on) { showHelp(on || helpBtn_->underMouse()); });
  dockBtn_ = new QPushButton(tr("Dock")); dockBtn_->setToolTip(tr("Pin back to the right column"));
  dockBtn_->setStyleSheet("QPushButton{padding:0 6px;font-size:11px}"); dockBtn_->setFixedHeight(20); dockBtn_->hide();
  connect(dockBtn_, &QPushButton::clicked, this, [this] { column_->dock(this); });
  h->addWidget(dockBtn_);
  head_->installEventFilter(this); title_->installEventFilter(this);
  v->addWidget(head_);
  scroll_ = new QScrollArea; scroll_->setWidgetResizable(true); scroll_->setFrameShape(QFrame::NoFrame);
  scroll_->setStyleSheet("QScrollArea{background:#262626}"); scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  v->addWidget(scroll_, 1);
  setBody(new QWidget);
}

void DockPanel::setBody(QWidget* w) {
  const int keepY = scroll_->verticalScrollBar()->value();      // (the panel made anew — a list switched, a setting changed: it stays where it was scrolled)
  QWidget* old = scroll_->takeWidget(); if (old) { old->hide(); old->deleteLater(); }
  w->setObjectName("dbody"); { QPalette pl = w->palette(); pl.setColor(QPalette::Window, QColor(0x26, 0x26, 0x26)); w->setPalette(pl); w->setAutoFillBackground(true); }   // (its background — a palette, not a style sheet: that would polish every child again)
  if (w->layout()) { w->layout()->setContentsMargins(8, 8, 8, 8); w->layout()->setSizeConstraint(QLayout::SetMinimumSize); }   // (too tall: it scrolls, never squeezed)
  scroll_->setWidget(w); body_ = w;
  if (keepY > 0) { scroll_->verticalScrollBar()->setValue(keepY); QTimer::singleShot(0, scroll_, [s = QPointer<QScrollArea>(scroll_), keepY] { if (s) s->verticalScrollBar()->setValue(keepY); }); }
  foldHelp();
}

// the grey paragraphs of 90+ characters (no colour of their own, no children) leave the window — under the ?
void DockPanel::foldHelp() {
  help_.clear();
  for (QLabel* l : body_->findChildren<QLabel*>()) {
    if (!l->property("mut").toBool() || l->styleSheet().contains("color") || !l->findChildren<QWidget*>().isEmpty()) continue;
    QString s = l->text().trimmed(); if (s.size() < 90 || Qt::mightBeRichText(s)) continue;
    help_ << s; l->hide();
  }
  helpBtn_->setVisible(!help_.isEmpty()); if (help_.isEmpty()) { helpBtn_->setChecked(false); showHelp(false); }
  else if (helpBtn_->isChecked()) showHelp(true);
}
void DockPanel::showHelp(bool on) {
  static QPointer<QLabel> pop;
  if (!on || help_.isEmpty()) { if (pop && pop->property("owner").value<void*>() == this) pop->hide(); return; }
  if (!pop) { pop = new QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint); pop->setWordWrap(true); pop->setFixedWidth(320); pop->setAttribute(Qt::WA_ShowWithoutActivating);
    pop->setStyleSheet("QLabel{background:#2b2b2b;border:1px solid #555;border-radius:5px;padding:8px 10px;color:#ccc}"); }
  pop->setProperty("owner", QVariant::fromValue<void*>(this));
  QStringList ps; for (const QString& s : help_) ps << "<p style=\"margin:0 0 6px 0\">" + s.toHtmlEscaped() + "</p>";
  pop->setText(ps.join("")); pop->adjustSize();
  QPoint g = helpBtn_->mapToGlobal(QPoint(helpBtn_->width(), helpBtn_->height() + 6));
  QRect sr = helpBtn_->screen()->availableGeometry();
  int x = std::clamp(g.x() - pop->width(), sr.left() + 8, sr.right() - pop->width() - 8), y = g.y() + pop->height() < sr.bottom() ? g.y() : std::max(sr.top() + 8, helpBtn_->mapToGlobal(QPoint(0, 0)).y() - pop->height() - 6);
  pop->move(x, y); pop->show(); pop->raise();
}

void DockPanel::setTitle(const QString& t) { title_->setText(t); }

// (docked, not growing: as tall as its content, up to 30 % of the column — the rest scrolls)
QSize DockPanel::sizeHint() const {
  int h = head_->height() + 2;
  if (!collapsed_ && body_) h += std::min(body_->sizeHint().height(), int(column_->height() * 0.3));
  return QSize(QFrame::sizeHint().width(), h);
}

void DockPanel::setWanted(bool on) {
  if (on == wanted_) return;
  wanted_ = on;
  if (floating_) setVisible(on); else column_->relayout();
}

void DockPanel::setCollapsed(bool on) {
  collapsed_ = on; scroll_->setVisible(!on);
  if (colBtn_) colBtn_->setText(on ? "▸" : "▾");
  if (floating_) adjustSize();
  column_->relayout();
}

void DockPanel::setFloating(bool on, const QPoint& global) {
  if (on == floating_) { if (on) move(global); return; }
  floating_ = on;
  QSize s = size();
  if (on) {
    setParent(column_->window(), Qt::Tool | Qt::FramelessWindowHint);
    setProperty("float", true); style()->unpolish(this); style()->polish(this);
    resize(std::max(280, s.width()), std::clamp(s.height(), 120, 700));
    move(global); show(); raise();
  } else {
    setProperty("float", false); style()->unpolish(this); style()->polish(this);
    setParent(column_); setWindowFlags(Qt::Widget);
  }
  dockBtn_->setVisible(on);
  column_->relayout();
}

bool DockPanel::eventFilter(QObject* o, QEvent* e) {
  if (o == helpBtn_) { if (e->type() == QEvent::Enter) showHelp(true); else if (e->type() == QEvent::Leave && !helpBtn_->isChecked()) showHelp(false); return false; }
  if (o != head_ && o != title_) return QFrame::eventFilter(o, e);
  if (e->type() == QEvent::MouseButtonPress) {
    auto* m = static_cast<QMouseEvent*>(e); if (m->button() != Qt::LeftButton) return false;
    pressAt_ = m->globalPosition().toPoint(); grabOff_ = pressAt_ - mapToGlobal(QPoint(0, 0)); dragging_ = false;
    return true;
  }
  if (e->type() == QEvent::MouseMove) {
    auto* m = static_cast<QMouseEvent*>(e); if (!(m->buttons() & Qt::LeftButton)) return false;
    QPoint g = m->globalPosition().toPoint();
    if (!dragging_ && (g - pressAt_).manhattanLength() < 6) return true;
    dragging_ = true;
    setFloating(true, g - grabOff_);
    column_->showMark(column_->indexAt(g));
    return true;
  }
  if (e->type() == QEvent::MouseButtonRelease) {
    auto* m = static_cast<QMouseEvent*>(e);
    if (dragging_) { int at = column_->indexAt(m->globalPosition().toPoint()); column_->showMark(-1); if (at >= 0) column_->dock(this, at); }
    dragging_ = false;
    return true;
  }
  return QFrame::eventFilter(o, e);
}

// ---- the column ----
DockColumn::DockColumn(QWidget* parent) : QFrame(parent) {
  setObjectName("dock"); setStyleSheet("#dock{background:#262626;border-left:1px solid #3a3a3a}");
  lay_ = new QVBoxLayout(this); lay_->setContentsMargins(0, 0, 0, 0); lay_->setSpacing(0);
  mark_ = new QFrame(this); mark_->setStyleSheet("background:#6f9fe8;border-radius:2px"); mark_->hide();
}

void DockColumn::setTop(QWidget* w) { top_ = w; relayout(); }
void DockColumn::addPanel(DockPanel* p) { order_.push_back(p); relayout(); }

void DockColumn::dock(DockPanel* p, int index) {
  if (index >= 0) {
    auto it = std::find(order_.begin(), order_.end(), p); if (it != order_.end()) order_.erase(it);
    // (index among the docked windows → its place in the order)
    int k = 0; size_t at = order_.size();
    for (size_t i = 0; i < order_.size(); i++) if (!order_[i]->floating()) { if (k == index) { at = i; break; } k++; }
    order_.insert(order_.begin() + long(at), p);
  }
  p->setFloating(false);
}

int DockColumn::indexAt(const QPoint& global) const {
  QPoint l = mapFromGlobal(global); if (!rect().contains(l)) return -1;
  int k = 0;
  for (DockPanel* p : order_) { if (p->floating() || !p->isVisible()) continue; if (l.y() < p->geometry().center().y()) return k; k++; }
  return k;
}

void DockColumn::showMark(int index) {
  if (index < 0) { mark_->hide(); return; }
  int y = top_ ? top_->geometry().bottom() : 0, k = 0;
  for (DockPanel* p : order_) { if (p->floating() || !p->isVisible()) continue; if (k == index) { y = p->geometry().top(); break; } y = p->geometry().bottom(); k++; }
  mark_->setGeometry(4, y - 1, width() - 8, 3); mark_->show(); mark_->raise();
}

void DockColumn::relayout() {
  while (lay_->count()) lay_->takeAt(0);
  if (top_) lay_->addWidget(top_);
  bool anyGrow = false;
  for (DockPanel* p : order_) {
    if (p->floating()) continue;
    if (!p->wanted()) { p->hide(); continue; }
    int stretch = p->collapsed() ? 0 : p->grows() ? 1 : 0;
    if (stretch) anyGrow = true;
    lay_->addWidget(p, stretch);
    p->show();
  }
  if (!anyGrow) lay_->addStretch(1);
}

}  // namespace hm::ui
