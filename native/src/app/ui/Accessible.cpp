#include "app/ui/Accessible.h"

#include "app/ui/Components.h"
#include "app/ui/Widgets.h"

#include <QAccessible>
#include <QAccessibleWidget>

#include <cmath>

namespace fw::ui {

#if QT_CONFIG(accessibility)

namespace {

// The widget's own accessible name, then `fallback`.
QString nameOr(const QWidget* w, const QString& fallback)
{
    return w->accessibleName().isEmpty() ? fallback : w->accessibleName();
}

class BadgeAccessible : public QAccessibleWidget {
public:
    explicit BadgeAccessible(Badge* w) : QAccessibleWidget(w, QAccessible::StaticText) {}
    QString text(QAccessible::Text t) const override
    {
        if (t == QAccessible::Name)
            return nameOr(widget(), static_cast<Badge*>(widget())->text());
        return QAccessibleWidget::text(t);
    }
};

class ProgressAccessible : public QAccessibleWidget, public QAccessibleValueInterface {
public:
    explicit ProgressAccessible(ProgressBar* w) : QAccessibleWidget(w, QAccessible::ProgressBar) {}
    void* interface_cast(QAccessible::InterfaceType t) override
    {
        if (t == QAccessible::ValueInterface)
            return static_cast<QAccessibleValueInterface*>(this);
        return QAccessibleWidget::interface_cast(t);
    }
    QString text(QAccessible::Text t) const override
    {
        if (t == QAccessible::Value)
            return bar()->state() == ProgressBar::State::Indeterminate ? QString() : thinPercent(shownFraction());
        if (t == QAccessible::Name)
            return nameOr(widget(), QStringLiteral("Progress"));
        return QAccessibleWidget::text(t);
    }
    QAccessible::State state() const override
    {
        QAccessible::State s = QAccessibleWidget::state();
        s.busy = bar()->state() == ProgressBar::State::Indeterminate;
        s.readOnly = true;
        return s;
    }
    QVariant currentValue() const override { return int(std::floor(shownFraction() * 100.0 + 1e-9)); }
    void setCurrentValue(const QVariant&) override {}
    QVariant maximumValue() const override { return 100; }
    QVariant minimumValue() const override { return 0; }
    QVariant minimumStepSize() const override { return 1; }

private:
    ProgressBar* bar() const { return static_cast<ProgressBar*>(widget()); }
    double shownFraction() const { return bar()->state() == ProgressBar::State::Done ? 1.0 : bar()->fraction(); }
};

// Segmented and ChoiceTiles paint their options themselves; to assistive
// tools they are one control whose value is the current option.
class ChoiceAccessible : public QAccessibleWidget {
public:
    explicit ChoiceAccessible(QWidget* w) : QAccessibleWidget(w, QAccessible::ComboBox) {}
    QString text(QAccessible::Text t) const override
    {
        if (t == QAccessible::Value) {
            if (auto* seg = qobject_cast<Segmented*>(widget()))
                return seg->segmentText(seg->currentIndex());
            if (auto* tiles = qobject_cast<ChoiceTiles*>(widget()))
                return tiles->choiceTitle(tiles->currentIndex());
        }
        return QAccessibleWidget::text(t);
    }
};

class StatTileAccessible : public QAccessibleWidget {
public:
    explicit StatTileAccessible(StatTile* w)
        : QAccessibleWidget(w, w->focusPolicy() == Qt::NoFocus ? QAccessible::StaticText : QAccessible::Button)
    {
    }
    QAccessible::Role role() const override
    {
        return widget()->focusPolicy() == Qt::NoFocus ? QAccessible::StaticText : QAccessible::Button;
    }
    QString text(QAccessible::Text t) const override
    {
        auto* tile = static_cast<StatTile*>(widget());
        if (t == QAccessible::Name) {
            const QString value = tile->valueLabel()->text();
            return value.isEmpty() ? tile->accessibleName() : QStringLiteral("%1: %2").arg(tile->accessibleName(), value);
        }
        return QAccessibleWidget::text(t);
    }
};

// A plain widget that acts as a button ("fwRole" = "button").
class ButtonLikeAccessible : public QAccessibleWidget {
public:
    explicit ButtonLikeAccessible(QWidget* w) : QAccessibleWidget(w, QAccessible::Button) {}
    QAccessible::State state() const override
    {
        QAccessible::State s = QAccessibleWidget::state();
        const QVariant expanded = widget()->property("fwExpanded");
        if (expanded.isValid()) {
            s.expandable = true;
            s.expanded = expanded.toBool();
            s.collapsed = !expanded.toBool();
        }
        return s;
    }
};

QAccessibleInterface* factory(const QString&, QObject* object)
{
    auto* w = qobject_cast<QWidget*>(object);
    if (!w)
        return nullptr;
    if (auto* badge = qobject_cast<Badge*>(w))
        return new BadgeAccessible(badge);
    if (auto* bar = qobject_cast<ProgressBar*>(w))
        return new ProgressAccessible(bar);
    if (qobject_cast<Segmented*>(w) || qobject_cast<ChoiceTiles*>(w))
        return new ChoiceAccessible(w);
    if (qobject_cast<Link*>(w))
        return new QAccessibleWidget(w, QAccessible::Link);
    if (auto* tile = qobject_cast<StatTile*>(w))
        return new StatTileAccessible(tile);
    if (w->property("fwRole").toString() == QLatin1String("button"))
        return new ButtonLikeAccessible(w);
    return nullptr;
}

} // namespace

void installAccessibility()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;
    QAccessible::installFactory(factory);
}

#else

void installAccessibility() {}

#endif

} // namespace fw::ui
