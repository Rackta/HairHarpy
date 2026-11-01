// The extras (the HTML version's FAQ, achievements and easter eggs): the FAQ window (sections per room, a search by tags
// — loose words, one-letter typos, Russian stems, filler words dropped — the keys as now set; 🏆 Achievements behind a
// 3 × 3 sliding puzzle — or, three times in ten, three in a row of the tool icons on a 12 × 12 board), the achievements (earned once per program window: a Steam-like popup with a chime), the FAQ
// girl (her hover pictures; petted 5 s — three achievements), Mara in the inspector's report (petted — her pat pictures,
// an achievement), the chimera (one launch in five she visits: peeks out at the tools or the Active Tool window; caught —
// achievements, then confetti).
#pragma once
#include <QString>
#include <QStringList>
#include <functional>

class QLabel;
class QPoint;
class QPushButton;
class QWidget;

namespace hm::extras {

bool has(const QString& id);
bool grant(const QString& id);                         // false — already earned
QStringList earned();                                  // (kept over ⟳ Reload)
void restore(const QStringList& ids);
void confetti(QWidget* host, const QPoint& global);

void openFaq(QWidget* parent, const QString& room = "hair");
// the FAQ opened at the help of a function: what (a button's / a menu item's name and tip) looked up — its best section
void openFaqAt(QWidget* parent, const QString& room, const QString& what);
void attachFaqButton(QPushButton* faq);                // hover pictures, petting
void attachMara(QLabel* mara);                         // petting (her pat pictures over the one shown)
void startChimera(QWidget* toolbox, QWidget* activeTool);   // (one launch in five)

}  // namespace hm::extras
