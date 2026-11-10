#pragma once

// FwStyle: the QProxyStyle behind the generated stylesheet. It paints what
// QSS cannot (checkbox and radio indicators, tinted arrows, no shadows, menu
// metrics) and switches off native focus rectangles: the 2 px outside focus
// ring of the design system is drawn by FocusRingOverlay, a transparent
// widget over the main window, so it can sit 1 px outside any control.

#include <QProxyStyle>
#include <QPointer>

class QApplication;
#include <QTimer>
#include <QWidget>

namespace fw::ui {

class FwStyle : public QProxyStyle {
    Q_OBJECT
public:
    FwStyle();

    void drawPrimitive(PrimitiveElement element, const QStyleOption* option, QPainter* painter,
                       const QWidget* widget = nullptr) const override;
    void drawControl(ControlElement element, const QStyleOption* option, QPainter* painter,
                     const QWidget* widget = nullptr) const override;
    int pixelMetric(PixelMetric metric, const QStyleOption* option = nullptr,
                    const QWidget* widget = nullptr) const override;
    int styleHint(StyleHint hint, const QStyleOption* option = nullptr, const QWidget* widget = nullptr,
                  QStyleHintReturn* returnData = nullptr) const override;
    void polish(QWidget* widget) override;
    void drawItemText(QPainter* painter, const QRect& rect, int flags, const QPalette& pal, bool enabled, const QString& text,
                      QPalette::ColorRole textRole = QPalette::NoRole) const override;
    QSize sizeFromContents(ContentsType type, const QStyleOption* option, const QSize& size,
                           const QWidget* widget) const override;
};

// Application-wide input rules, installed once by theme::apply():
//  * a tooltip that looks like markup and was not written by the app (see
//    plainTip()) is shown as the literal text: tooltips often carry paths,
//    file names and messages from outside;
//  * the mouse wheel does not change a dropdown, spin box or slider that has
//    no keyboard focus: the page under it scrolls instead;
//  * Enter / Return press the focused button, like Space does.
void installAppGuard(QApplication* app);

// True when `w` has the keyboard focus and got it the keyboard way: Tab,
// Backtab, a shortcut, or code that moved it with one of those reasons (the
// sidebar's arrow keys, a sheet's default button). Focus rings are shown for
// this, not after a mouse click. (Qt's WA_KeyboardFocusChange follows real
// Tab key presses only and is cleared by every other focus change.)
bool hasKeyboardFocus(const QWidget* w);

// Paints a 2 px `focus` ring 1 px outside the focused widget (radius + 1).
// Inputs show it whenever they have focus; everything else only after keyboard
// focus changes (Tab, arrows). Widgets opt out with the dynamic property
// "fwNoRing" and change the corner radius with "fwRadius".
class FocusRingOverlay : public QWidget {
    Q_OBJECT
public:
    explicit FocusRingOverlay(QWidget* window);
    void reset(); // forget the target (after programmatic focus changes)

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void onFocusChanged(QWidget* old, QWidget* now);
    QRect ringRect(int* radius) const;
    void refresh();

    QPointer<QWidget> m_target;
    QTimer m_tick;
    QRect m_last;
};

} // namespace fw::ui
