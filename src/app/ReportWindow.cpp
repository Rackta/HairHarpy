#include "app/ReportWindow.h"
#include "app/Extras.h"
#include "ui/Theme.h"
#include "view/Viewport.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStyle>
#include <QVBoxLayout>
#include <cmath>

namespace hm {

ReportWindow::ReportWindow(QWidget* host, Viewport* vp, Scene* sc) : QFrame(host->window(), Qt::Tool | Qt::FramelessWindowHint), host_(host), vp_(vp), sc_(sc) {
  setObjectName("weldWarn");
  setStyleSheet("#weldWarn{background:#111725;border:2px solid #5a8dee;border-radius:16px}"
                "#weldWarn QListWidget{background:transparent;color:#aab;border:none;border-left:1px solid #2a3350}"
                "#weldWarn QPushButton{background:#1f2638;border:1px solid #3a4568;color:#dde;border-radius:6px;padding:5px 12px;min-width:68px}"
                "#weldWarn QPushButton[blue=\"true\"]{background:#3b6fd8;border-color:#5a8dee;color:#fff;min-width:84px}");
  auto* h = new QHBoxLayout(this); h->setContentsMargins(14, 14, 14, 14); h->setSpacing(16);
  mara_ = new QLabel; mara_->setFixedSize(150, 150); mara_->setScaledContents(true); h->addWidget(mara_, 0, Qt::AlignVCenter);
  extras::attachMara(mara_);                                  // (petted — her pat pictures, an achievement)
  auto* mid = new QVBoxLayout; mid->setSpacing(8);
  title_ = new QLabel; title_->setStyleSheet("font-weight:600;font-size:14px;color:#fff"); title_->setWordWrap(true);
  text_ = new QLabel; text_->setStyleSheet("color:#c8cbd8;font-size:12px"); text_->setWordWrap(true);
  prog_ = new QProgressBar; prog_->setRange(0, 100); prog_->setMaximumWidth(220); prog_->hide();
  mid->addWidget(title_); mid->addWidget(text_); mid->addWidget(prog_); mid->addStretch(1);
  h->addLayout(mid, 13);
  auto* side = new QVBoxLayout; side->setSpacing(8);
  list_ = new QListWidget; list_->setToolTip(tr("Click — highlight this place")); list_->setMaximumHeight(130);
  side->addWidget(list_, 1);
  auto* bar = new QHBoxLayout; bar->setSpacing(6); bar->addStretch(1);
  accAll_ = new QPushButton(tr("Accept all")); rejAll_ = new QPushButton(tr("Reject all"));
  fix_ = new QPushButton(tr("Auto-fix")); fix_->setToolTip(tr("Try to mend these places automatically — you review each fix (blue markers) and accept or reject it"));
  again_ = new QPushButton(tr("Check again")); again_->setIcon(ui::icon("insp_runcheck")); again_->setIconSize(QSize(20, 20)); again_->setToolTip(tr("Check the connections again — after you have fixed some of them"));
  convert_ = new QPushButton(tr("Convert")); convert_->setToolTip(tr("Convert the checked cocoons' locks to meshes")); convert_->setProperty("blue", true);
  ok_ = new QPushButton(tr("OK")); ok_->setProperty("blue", true); ok_->setDefault(true);
  for (QPushButton* b : {accAll_, rejAll_, fix_, again_, convert_, ok_}) bar->addWidget(b);
  side->addLayout(bar);
  h->addLayout(side, 10);
  setFixedWidth(760);

  connect(list_, &QListWidget::currentRowChanged, this, [this](int r) {
    if (r < 0 || r >= int(places_.size())) return;
    const auto& S = vp_->weldSpots(); int best = -1; double bd = 1;
    for (size_t i = 0; i < S.size(); i++) { double d = S[i].c.distanceTo(places_[size_t(r)].c); if (d < bd) { bd = d; best = int(i); } }
    if (best >= 0) vp_->setWeldHot(best);
  });
  connect(ok_, &QPushButton::clicked, this, &ReportWindow::dismiss);
  connect(again_, &QPushButton::clicked, this, [this] { afterFix_ = false; vp_->endFixReview(); vp_->checkConnections(true); });
  connect(fix_, &QPushButton::clicked, this, &ReportWindow::runAutoFix);
  auto all = [this](bool acc) { vp_->fixAll(acc); afterFix_ = false; if (isVisible()) vp_->checkConnections(true); };
  connect(accAll_, &QPushButton::clicked, this, [all] { all(true); });
  connect(rejAll_, &QPushButton::clicked, this, [all] { all(false); });
  connect(convert_, &QPushButton::clicked, this, [this] { dismiss(); emit convertRequested(); });
}

void ReportWindow::setMara(const QString& img) { mara_->setPixmap(ui::image(img)); }

void ReportWindow::place() {
  if (!host_) return;
  adjustSize();
  QPoint br = host_->mapToGlobal(QPoint(host_->width(), host_->height()));
  move(br.x() - width() - 16, br.y() - height() - 16);
}

void ReportWindow::dismiss() { hide(); afterFix_ = false; vp_->endFixReview(); vp_->clearWeldSpots(); }

// the places found (nothing — "no faults", with Convert); quiet (report false) — only refreshed when it is open
void ReportWindow::showProblems(const std::vector<WeldProblem>& problems, bool report) {
  if (!report && !isVisible()) return;
  // (after Auto-fix it says what it mended until the review is over; then — everything mended — "no faults")
  if (!report && afterFix_) { if (vp_->reviewing() || !problems.empty()) return; afterFix_ = false; }
  afterFix_ = false;
  QSignalBlocker b(list_); list_->clear(); places_ = problems;
  prog_->hide(); text_->show(); accAll_->hide(); rejAll_->hide();
  if (problems.empty()) {
    setMara("mara_3"); title_->setText(tr("Inspector's Report"));
    text_->setText(tr("No faults were found during the inspection."));
    list_->hide(); again_->hide(); fix_->hide(); convert_->show(); ok_->setText(tr("Close")); ok_->setProperty("blue", false);
  } else {
    setMara("mara"); title_->setText(tr("Inspector's Report: Mismatched joints found"));
    text_->setText(tr("Some locks that are meant to be welded cross each other, but their joints are not made correctly. Please fix these "
                      "connections, or mark them as correct so they are ignored (right-click a red marker)."));
    for (auto& p : problems) list_->addItem("✕ " + weldIssueText(*sc_, vp_->hair(), p));
    list_->show(); again_->show(); fix_->show(); convert_->hide(); ok_->setText(tr("OK")); ok_->setProperty("blue", true);
  }
  ok_->style()->unpolish(ok_); ok_->style()->polish(ok_);
  show(); place(); raise();
}

// Auto-fix: the places of a kind it can mend are mended (each tried, kept only if the place is gone) — blue markers to
// review one by one (RMB: before / after, accept, reject) or all at once; the rest stay red, untouched
void ReportWindow::runAutoFix() {
  if (places_.empty()) return;
  std::vector<WeldProblem> problems = places_; int m = int(problems.size());
  afterFix_ = true;
  for (QPushButton* b : {fix_, ok_, again_}) b->setEnabled(false);
  setMara("mara_2"); title_->setText(tr("Inspector's Report: looking for fixes…")); text_->hide(); prog_->setValue(0); prog_->show();
  QApplication::setOverrideCursor(Qt::WaitCursor);
  int n = vp_->autoFix(problems, [this, m](double d) {
    prog_->setValue(int(d * 100)); prog_->setFormat(tr("Place %1 of %2").arg(std::min(m, int(std::lround(d * m)) + 1)).arg(m));
    QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
  });
  QApplication::restoreOverrideCursor();
  for (QPushButton* b : {fix_, ok_, again_}) b->setEnabled(true);
  prog_->hide(); text_->show(); fix_->hide(); afterFix_ = true;
  if (n) {
    setMara("mara"); title_->setText(tr("Inspector's Report: Mismatched joints found"));
    text_->setText(tr("Auto-fix mended %1 of %2 place%3 — blue markers. Right-click a blue marker to compare before / after, then accept or "
                      "reject the fix. The places it couldn't mend stay red: fix them yourself or mark them as correct.").arg(n).arg(m).arg(m == 1 ? "" : "s"));
  } else {
    setMara("mara_3"); title_->setText(tr("Inspector's Report"));
    text_->setText(tr("I can't fix this — I've got paws. Please fix the connections by hand."));
  }
  QSignalBlocker b(list_); list_->clear();
  for (auto& p : problems) {
    const WeldFix* f = nullptr; for (auto& x : vp_->fixes()) if (x.c.distanceTo(p.c) < 1) f = &x;
    auto* it = new QListWidgetItem((f ? "✓ " : "✕ ") + weldIssueText(*sc_, vp_->hair(), p) + (f ? " — " + f->op.txt : QString()));
    if (f) it->setForeground(QColor(0x8f, 0xc3, 0xff));
    list_->addItem(it);
  }
  accAll_->setVisible(n > 0); rejAll_->setVisible(n > 0);
  place();
}

}  // namespace hm
