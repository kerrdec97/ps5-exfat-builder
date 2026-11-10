#pragma once

// Motion: the few animations of the design system. All are small, fast and
// never block: a value eases to its target (OutCubic) and the widget paints
// it. An animation always ends in exactly the state the widget would have had
// without it, stops when its widget is hidden, and dies with its widget.
//
// One switch turns every animation into its end state at once: in screenshot
// mode (qApp property "fwScreenshotMode"), on the offscreen and minimal
// platforms (tests), and when Windows has its animations switched off.

#include <QObject>
#include <QVariantAnimation>

#include <functional>

class QWidget;

namespace fw::ui::motion {

inline constexpr int fast = 110, base = 160, slow = 220; // ms

bool enabled();
// Tests: force motion on or off whatever the rules above say.
void setOverride(bool on);
void clearOverride();

// One eased number that a widget paints. It belongs to `owner`: it animates
// only while the owner is visible, jumps to its target when the owner is
// hidden, and is deleted with it.
class Value : public QObject {
    Q_OBJECT
public:
    explicit Value(QWidget* owner, qreal initial = 0.0, int durationMs = base);
    qreal value() const { return m_value; }   // what to paint now
    qreal target() const { return m_target; } // where it is going
    void setDuration(int ms) { m_duration = ms; }
    // Called after every change instead of owner->update(): to repaint a
    // part of the owner only, or to move something.
    void onChange(std::function<void()> fn) { m_onChange = std::move(fn); }
    void animateTo(qreal target);
    void jumpTo(qreal value);
    bool isRunning() const { return m_animation.state() == QAbstractAnimation::Running; }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void changed();
    QWidget* m_owner;
    QVariantAnimation m_animation;
    std::function<void()> m_onChange;
    qreal m_value, m_target;
    int m_duration;
};

// Hover over one of several items that a single widget paints (segments,
// tiles, rows): the item under the pointer fades in while the one it left
// fades out. `repaint(index)` updates one item (default: the whole owner).
class HoverFade : public QObject {
    Q_OBJECT
public:
    explicit HoverFade(QWidget* owner, std::function<void(int)> repaint = {});
    void setHovered(int index); // -1: none
    int hovered() const { return m_current; }
    qreal amount(int index) const; // 0..1, for painting item `index`
    void reset();                  // the items changed: nothing is hovered, at once
    bool isRunning() const { return m_in.isRunning() || m_out.isRunning(); }

private:
    Value m_in, m_out;
    int m_current = -1, m_previous = -1;
};

} // namespace fw::ui::motion
