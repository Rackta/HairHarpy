// The HTML version's look as a Qt style sheet: its colours (--bg #1e1e1e, --panel #262626, --panel2 #2e2e2e, --line
// #3a3a3a, --txt #cfcfcf, --mut #8a8a8a, --acc #4f7fd0, --sel #3a5585), Segoe UI 12 px, its buttons (button.on — the
// selection blue, button.primary — green), sliders, scroll bars and panels. Images: the ones the page embedded
// (resources/images, ":/img/<name>").
#pragma once
#include <QColor>
#include <QCursor>
#include <QIcon>
#include <QPixmap>
#include <QString>

class QApplication;

namespace hm::ui {

namespace col {
inline const QColor bg{0x1e, 0x1e, 0x1e}, panel{0x26, 0x26, 0x26}, panel2{0x2e, 0x2e, 0x2e}, line{0x3a, 0x3a, 0x3a},
    txt{0xcf, 0xcf, 0xcf}, mut{0x8a, 0x8a, 0x8a}, acc{0x4f, 0x7f, 0xd0}, sel{0x3a, 0x55, 0x85};
}

void applyTheme(QApplication& app);
QPixmap image(const QString& name);                 // ":/img/<name>.png|jpg|webp"
QIcon icon(const QString& name);
// the show / hide eye (drawn: an outline and its pupil; closed — the lid's line), 16 px (crisp on HiDPI)
QPixmap eyePix(bool open, const QColor& c = QColor(0xcf, 0xcf, 0xcf));
QIcon eyeIcon(bool open);
// the pointer of a tool: the arrow with the tool's picture beside it, lower right (the hot spot — the arrow's tip)
QCursor badgeCursor(const QString& image);

}  // namespace hm::ui
