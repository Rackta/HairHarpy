#include "ui/Theme.h"
#include <QApplication>
#include <QFile>
#include <QFont>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QStyleFactory>

namespace hm::ui {

namespace {
const char* QSS = R"QSS(
* { font-family: "Segoe UI"; font-size: 12px; color: #cfcfcf; }
QMainWindow, QWidget#app, QDialog { background: #1e1e1e; }
QWidget[panel="true"] { background: #262626; }
QToolTip { background: #333; color: #ddd; border: 1px solid #555; padding: 3px 5px; }

QPushButton, QToolButton {
  background: #2e2e2e; color: #cfcfcf; border: 1px solid #444; border-radius: 4px; padding: 3px 8px;
}
QPushButton:hover, QToolButton:hover { background: #3a3a3a; }
QPushButton:disabled, QToolButton:disabled { color: #6a6a6a; border-color: #383838; }
QPushButton:checked, QToolButton:checked, QPushButton[on="true"] { background: #3a5585; border-color: #5f7fb8; color: #fff; }
QPushButton[primary="true"] { background: #2f5f3a; border-color: #4a8a57; }
QPushButton[primary="true"]:hover { background: #3a7046; }
QPushButton[danger="true"]:hover { background: #9a3a3a; border-color: #d06060; }
QPushButton[flat2="true"] { background: transparent; border-color: #3a3a3a; color: #8a8a8a; padding: 2px 6px; font-size: 11px; }
QPushButton[flat2="true"]:hover { background: #333; color: #cfcfcf; }
QToolButton::menu-indicator { image: none; width: 0; }

QPushButton[tool="true"] { background: transparent; border: 1px solid transparent; border-radius: 7px; padding: 0; }
QPushButton[tool="true"]:hover { border: 2px solid #92776d; }
QPushButton[tool="true"]:checked { background: transparent; border: 2px solid #cbbfb3; }

QLineEdit, QSpinBox, QDoubleSpinBox, QComboBox, QPlainTextEdit, QTextEdit {
  background: #111; color: #fff; border: 1px solid #444; border-radius: 3px; padding: 2px 4px; selection-background-color: #3a5585;
}
QLineEdit:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-color: #4f7fd0; }
QComboBox { background: #1d1d1d; color: #cfcfcf; }
QComboBox QAbstractItemView { background: #262626; border: 1px solid #555; selection-background-color: #3d4b66; }
QCheckBox { spacing: 6px; }
QCheckBox::indicator { width: 13px; height: 13px; border: 1px solid #666; border-radius: 2px; background: #1b1b1b; }
QCheckBox::indicator:checked { background: #4f7fd0; border-color: #6f9fe8; }

QSlider::groove:horizontal { height: 4px; background: #444; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #4f7fd0; border-radius: 2px; }
QSlider::handle:horizontal { width: 12px; height: 12px; margin: -5px 0; border-radius: 6px; background: #d8d8d8; }
QSlider::handle:horizontal:hover { background: #fff; }

QScrollBar:vertical { width: 8px; background: transparent; margin: 0; }
QScrollBar:horizontal { height: 8px; background: transparent; margin: 0; }
QScrollBar::handle:vertical, QScrollBar::handle:horizontal { background: #3f444c; border-radius: 3px; min-height: 20px; min-width: 20px; margin: 1px; }
QScrollBar::handle:hover { background: #5a616c; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QTreeWidget, QTreeView, QListWidget { background: #262626; border: none; outline: none; }
QTreeWidget::item, QListWidget::item { height: 23px; }
QTreeWidget::item:hover, QListWidget::item:hover { background: #2f2f2f; }
QTreeWidget::item:selected, QListWidget::item:selected { background: #3a5585; color: #fff; }

QMenu { background: #2b2b2b; border: 1px solid #555; padding: 4px; }
QMenu::item { padding: 4px 18px 4px 10px; border-radius: 3px; }
QMenu::item:selected { background: #3d4b66; }
QMenu::item:disabled { color: #666; }
QMenu::separator { height: 1px; background: #444; margin: 3px 2px; }

QProgressBar { background: #1b1b1b; border: 1px solid #3a3a3a; border-radius: 3px; height: 12px; text-align: center; color: #8fd6f2; font-size: 10px; }
QProgressBar::chunk { background: #3ec6f2; border-radius: 2px; }

QFrame[sep="true"] { background: #3a3a3a; }
QLabel[mut="true"] { color: #8a8a8a; }
QLabel[h4="true"] { color: #fff; font-weight: 600; }
QSplitter::handle { background: #3a3a3a; }
QSplitter::handle:hover { background: #6f9fe8; }
)QSS";
}  // namespace

void applyTheme(QApplication& app) {
  app.setStyle(QStyleFactory::create("Fusion"));
  QPalette p;
  p.setColor(QPalette::Window, col::panel); p.setColor(QPalette::WindowText, col::txt);
  p.setColor(QPalette::Base, QColor(0x11, 0x11, 0x11)); p.setColor(QPalette::AlternateBase, col::panel2);
  p.setColor(QPalette::Text, col::txt); p.setColor(QPalette::Button, col::panel2); p.setColor(QPalette::ButtonText, col::txt);
  p.setColor(QPalette::Highlight, col::sel); p.setColor(QPalette::HighlightedText, Qt::white);
  p.setColor(QPalette::ToolTipBase, QColor(0x33, 0x33, 0x33)); p.setColor(QPalette::ToolTipText, QColor(0xdd, 0xdd, 0xdd));
  p.setColor(QPalette::PlaceholderText, col::mut);
  p.setColor(QPalette::Disabled, QPalette::Text, QColor(0x66, 0x66, 0x66)); p.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(0x66, 0x66, 0x66));
  app.setPalette(p);
  QFont f("Segoe UI"); f.setPixelSize(12); app.setFont(f);
  app.setStyleSheet(QString::fromUtf8(QSS));
}

QPixmap image(const QString& name) {
  static QHash<QString, QPixmap> cache;
  auto it = cache.find(name); if (it != cache.end()) return *it;
  QPixmap pm;
  for (const char* ext : {"png", "jpg", "webp"}) { QString p = QString(":/img/%1.%2").arg(name, ext); if (QFile::exists(p)) { pm.load(p); break; } }
  cache.insert(name, pm);
  return pm;
}

QIcon icon(const QString& name) { return QIcon(image(name)); }

QCursor badgeCursor(const QString& name) {
  static QHash<QString, QCursor> cache; if (auto it = cache.find(name); it != cache.end()) return *it;
  const qreal k = 2;                                            // (drawn at twice the size: sharp on a scaled screen)
  QPixmap pm(int(48 * k), int(48 * k)); pm.fill(Qt::transparent); pm.setDevicePixelRatio(k);
  QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); p.setRenderHint(QPainter::SmoothPixmapTransform);
  QPolygonF arrow; arrow << QPointF(1, 1) << QPointF(1, 22) << QPointF(6, 17.2) << QPointF(9.6, 25.4) << QPointF(13, 24) << QPointF(9.4, 15.9) << QPointF(16, 15.9);
  p.setPen(QPen(Qt::black, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::white); p.drawPolygon(arrow);
  QPixmap b = image(name); if (!b.isNull()) p.drawPixmap(QRectF(15, 15, 30, 30), b, QRectF(b.rect()));
  p.end();
  QCursor c(pm, 1, 1); cache.insert(name, c); return c;
}

QPixmap eyePix(bool open, const QColor& c) {
  static QHash<QString, QPixmap> cache; QString key = QString("%1%2").arg(open).arg(c.rgba());
  if (auto it = cache.find(key); it != cache.end()) return *it;
  const int S = 16, K = 3; QPixmap pm(S * K, S * K); pm.fill(Qt::transparent);
  QPainter p(&pm); p.setRenderHint(QPainter::Antialiasing); p.scale(K, K);
  QPen pen(c, 1.3); pen.setCapStyle(Qt::RoundCap); pen.setJoinStyle(Qt::RoundJoin); p.setPen(pen); p.setBrush(Qt::NoBrush);
  if (open) {
    QPainterPath e; e.moveTo(1.5, 8); e.quadTo(8, 1.5, 14.5, 8); e.quadTo(8, 14.5, 1.5, 8); p.drawPath(e);
    p.setBrush(c); p.setPen(Qt::NoPen); p.drawEllipse(QPointF(8, 8), 2.4, 2.4);
  } else {
    QPainterPath l; l.moveTo(1.5, 7); l.quadTo(8, 12.5, 14.5, 7); p.drawPath(l);
    for (double x : {4.0, 8.0, 12.0}) { double y = x == 8.0 ? 9.8 : 9.0; p.drawLine(QPointF(x, y), QPointF(x + (x - 8) * 0.12, y + 2.2)); }
  }
  p.end(); pm.setDevicePixelRatio(K);
  cache.insert(key, pm); return pm;
}
QIcon eyeIcon(bool open) { return QIcon(eyePix(open)); }

}  // namespace hm::ui
