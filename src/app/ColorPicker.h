// A colour picker drawn by hand (the reference canvas's palette, a fill layer's colour): five views of the same colour —
// quad (saturation × value square, the hue strip beside it), triangle (the hue ring round a white–black–hue triangle), round
// (the hue × saturation disc, the value strip beside it), bars (H S V and R G B as gradient bars) and image (a picture to
// pick colours from — loaded once, kept between runs). The colour changes as it is dragged (changed) and is settled on
// release (committed — what goes to the recent colours).
#pragma once
#include <QColor>
#include <QImage>
#include <QWidget>
#include <functional>
#include <vector>

namespace hm {

class ColorPicker : public QWidget {
public:
  explicit ColorPicker(QWidget* parent = nullptr);
  static QStringList modes();                       // quad triangle round bars image (the keys)
  static QString modeName(const QString& key);
  void setMode(const QString& key);
  QString mode() const { return mode_; }
  void setColor(const QColor& c);                    // (from outside: no callback)
  QColor color() const { return QColor::fromHsvF(float((h_ >= 360 ? 0 : h_) / 360.0), float(s_), float(v_)); }
  std::function<void(const QColor&)> changed, committed;
  std::function<void()> onLoad;                       // (the image view: a picture asked for — the owner's file window, then loadImage)
  void loadImage(const QString& file);
protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void mouseReleaseEvent(QMouseEvent*) override;
  void resizeEvent(QResizeEvent*) override { cache_ = {}; }
private:
  QString mode_ = "quad";
  double h_ = 0, s_ = 0, v_ = 1;                     // (hue in degrees: kept when the colour is gray)
  int part_ = -1;                                    // (what is being dragged)
  QImage cache_; QString cacheKey_;
  QImage pic_; QString picFile_;
  QRectF mainRect() const;
  QRectF stripRect() const;
  QRectF picRect() const;
  void pick(const QPointF& p, bool press);
  void setHsv(double h, double s, double v);
};

// a row of colour squares: click — that colour (picked), right click — removed (when removing is allowed), the last one may be ＋
class SwatchGrid : public QWidget {
public:
  explicit SwatchGrid(QWidget* parent = nullptr) : QWidget(parent) { setMouseTracking(true); }
  void setColors(const std::vector<QColor>& c) { c_ = c; relayout(); update(); }
  const std::vector<QColor>& colors() const { return c_; }
  bool plus = false;                                 // (a ＋ square at the end: adds the current colour)
  std::function<void(const QColor&)> picked;
  std::function<void(int)> removed;
  std::function<void()> added;
  int heightForWidth(int w) const override;
  bool hasHeightForWidth() const override { return true; }
protected:
  void paintEvent(QPaintEvent*) override;
  void mousePressEvent(QMouseEvent*) override;
  void mouseMoveEvent(QMouseEvent*) override;
  void leaveEvent(QEvent*) override { hover_ = -1; update(); }
  void resizeEvent(QResizeEvent*) override { relayout(); }
private:
  std::vector<QColor> c_; int hover_ = -1;
  static constexpr int kS = 22, kG = 3;
  int cols() const { return std::max(1, (width() + kG) / (kS + kG)); }
  int count() const { return int(c_.size()) + (plus ? 1 : 0); }
  QRect cell(int i) const { const int n = cols(); return QRect((i % n) * (kS + kG), (i / n) * (kS + kG), kS, kS); }
  int at(const QPoint& p) const;
  void relayout() { setMinimumHeight(heightForWidth(width())); updateGeometry(); }
};

}  // namespace hm
