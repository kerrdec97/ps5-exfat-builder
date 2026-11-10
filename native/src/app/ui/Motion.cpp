#include "app/ui/Motion.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QWidget>

#ifdef Q_OS_WIN
#include <qt_windows.h>
#endif

namespace fw::ui::motion {

namespace {

int g_override = -1; // -1: the rules below; 0 / 1: forced by a test

// Windows: Settings > Accessibility > Visual effects > Animation effects.
// Asked again every few seconds: the user can change it while the app runs.
bool systemAnimations()
{
#ifdef Q_OS_WIN
    static QElapsedTimer age;
    static bool on = true;
    if (!age.isValid() || age.elapsed() > 5000) {
        BOOL value = TRUE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &value, 0))
            on = value != FALSE;
        age.start();
    }
    return on;
#else
    return true;
#endif
}

} // namespace

bool enabled()
{
    if (g_override >= 0)
        return g_override == 1;
    if (!qApp || qApp->property("fwScreenshotMode").toBool())
        return false;
    static const bool headless = QGuiApplication::platformName() == QLatin1String("offscreen")
                              || QGuiApplication::platformName() == QLatin1String("minimal");
    return !headless && systemAnimations();
}

void setOverride(bool on) { g_override = on ? 1 : 0; }
void clearOverride() { g_override = -1; }

// ---- Value -----------------------------------------------------------------------

Value::Value(QWidget* owner, qreal initial, int durationMs)
    : QObject(owner), m_owner(owner), m_value(initial), m_target(initial), m_duration(durationMs)
{
    m_animation.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        m_value = v.toReal();
        changed();
    });
    connect(&m_animation, &QAbstractAnimation::finished, this, [this] {
        if (m_value != m_target) { // exactly the target, whatever the last step computed
            m_value = m_target;
            changed();
        }
    });
    owner->installEventFilter(this);
}

void Value::animateTo(qreal target)
{
    if (target == m_target && (isRunning() || m_value == target))
        return;
    // Already there (an animation the other way had not moved yet): nothing
    // to show, and no step would ever be reported.
    if (!enabled() || m_duration <= 0 || !m_owner->isVisible() || m_value == target) {
        jumpTo(target);
        return;
    }
    m_animation.stop();
    m_target = target;
    m_animation.setDuration(m_duration);
    m_animation.setStartValue(m_value);
    m_animation.setEndValue(target);
    m_animation.start();
}

void Value::jumpTo(qreal value)
{
    m_animation.stop();
    const bool moved = m_value != value;
    m_value = m_target = value;
    if (moved)
        changed();
}

void Value::changed()
{
    if (m_onChange)
        m_onChange();
    else
        m_owner->update();
}

bool Value::eventFilter(QObject* watched, QEvent* event)
{
    // Nothing runs for a widget nobody sees: it is at its end state when it comes back.
    if (watched == m_owner && event->type() == QEvent::Hide && isRunning())
        jumpTo(m_target);
    return QObject::eventFilter(watched, event);
}

// ---- HoverFade -------------------------------------------------------------------

HoverFade::HoverFade(QWidget* owner, std::function<void(int)> repaint)
    : QObject(owner), m_in(owner, 0.0, fast), m_out(owner, 0.0, fast)
{
    if (repaint) {
        m_in.onChange([this, repaint] {
            if (m_current >= 0)
                repaint(m_current);
        });
        m_out.onChange([this, repaint] {
            if (m_previous >= 0)
                repaint(m_previous);
        });
    }
}

qreal HoverFade::amount(int index) const
{
    if (index < 0)
        return 0.0;
    if (index == m_current)
        return m_in.value();
    if (index == m_previous)
        return m_out.value();
    return 0.0;
}

void HoverFade::setHovered(int index)
{
    if (index == m_current)
        return;
    const qreal leaving = m_in.value();
    // Back onto the item that was still fading out: it goes on from there.
    const qreal entering = index >= 0 && index == m_previous ? m_out.value() : 0.0;
    m_out.jumpTo(0.0); // an older fade-out ends here
    m_previous = m_current;
    m_current = index;
    m_out.jumpTo(leaving);
    m_out.animateTo(0.0);
    m_in.jumpTo(entering);
    m_in.animateTo(index >= 0 ? 1.0 : 0.0);
}

void HoverFade::reset()
{
    m_current = m_previous = -1;
    m_in.jumpTo(0.0);
    m_out.jumpTo(0.0);
}

} // namespace fw::ui::motion
