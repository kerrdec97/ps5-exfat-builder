#pragma once

// Schema-driven setting editors (§4.21).
//
//  * SettingEditor  — one control for one SettingDef (Switch, Dropdown or
//    Segmented, TextInput with unit, PathField, list field), with value()/
//    setValue() and an edited() signal.
//  * SettingRow     — label + help + editor on the right, validation message,
//    disabled-when-dependency state, override marker ("Overridden for this
//    job" + "Use default") for job pages.
//  * JobOptionsPanel — every `perJob` setting of the given sections, grouped
//    into cards by `group`; produces the overrides map for JobQueue::enqueue.

#include "app/ui/Widgets.h"
#include "core/Settings.h"

#include <QHash>
#include <QPointer>
#include <QWidget>

namespace fw {
class AppContext;
}

namespace fw::ui {

class SettingEditor : public QObject {
    Q_OBJECT
public:
    SettingEditor(const SettingDef& def, QWidget* parent);
    QWidget* widget() const { return m_widget; }
    QVariant value() const;
    void setValue(const QVariant& v);   // does not emit edited()
    void setEnabled(bool enabled);
    void setError(bool error);
    const SettingDef& def() const { return m_def; }
signals:
    void edited(const QVariant& value); // user interaction only

private:
    void build(QWidget* parent);
    SettingDef m_def;
    QWidget* m_widget = nullptr;
    QWidget* m_control = nullptr; // the focusable control inside
    bool m_updating = false;
};

class SettingRow : public QWidget {
    Q_OBJECT
public:
    SettingRow(const SettingDef& def, QWidget* parent = nullptr);
    SettingEditor* editor() { return m_editor; }
    const SettingDef& def() const { return m_def; }
    void setMessage(const QString& text, Tone tone); // under the control; empty hides
    QString messageText() const;
    void setDisabledReason(const QString& reason);   // "" enables the row
    void setOverridden(bool on, const QString& defaultText = {});
    bool isOverridden() const { return m_overridden; }
    void setDivider(bool on);                         // border_1 under the row
    void setRestartNote(bool on);
    QLabel* labelWidget() { return m_label; }
    static QString conciseHelp(const QString& description); // first sentence, <= ~130 chars
    static QString displayValue(const SettingDef& def, const QVariant& value); // "128 KB", "On", "D:\\Images"
signals:
    void useDefaultRequested();

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;

private:
    void updateHelp();
    QBoxLayout* m_layout = nullptr;
    QWidget* m_right = nullptr;
    bool m_narrow = false;
    SettingDef m_def;
    SettingEditor* m_editor;
    QLabel* m_label;
    QLabel* m_info = nullptr;
    QLabel* m_help;
    Badge* m_badge;
    Link* m_useDefault;
    MessageRow* m_message;
    QString m_defaultText, m_disabledReason;
    bool m_overridden = false;
    bool m_divider = true;
    bool m_restart = false;
};

class JobOptionsPanel : public QWidget {
    Q_OBJECT
public:
    // Lists perJob settings in `sections` (schema order); `excludeKeys` are
    // handled by the page itself (e.g. the output folder field).
    JobOptionsPanel(AppContext& ctx, const QStringList& sections, const QStringList& excludeKeys = {},
                    QWidget* parent = nullptr);
    QVariantMap overrides() const { return m_overrides; }
    void setOverride(const QString& key, const QVariant& value); // programmatic (e.g. a page's own switch)
    void clearOverrides();
    bool hasErrors() const { return !m_errors.isEmpty(); }
    QStringList errorMessages() const { return m_errors.values(); }
    int rowCount() const { return int(m_rows.size()); }
    SettingRow* row(const QString& key) const { return m_rows.value(key); }
    QStringList keys() const { return m_order; }
    void setGroupCards(bool cards) { m_cards = cards; } // before build(); default: cards per group
    // Before build(): only keys equal to, or starting with, one of these
    // ("exfat.", "verification.mode"); empty = every perJob key of the sections.
    void setKeyFilter(const QStringList& keysOrPrefixes) { m_include = keysOrPrefixes; }
    void build();
    QVariant effectiveValue(const QString& key) const;
signals:
    void overridesChanged();

private:
    void onEdited(const QString& key, const QVariant& value);
    void clearError(const QString& key);
    void refreshRow(const QString& key);
    void refreshDependencies();
    AppContext& m_ctx;
    QStringList m_sections, m_exclude, m_order, m_include;
    QHash<QString, SettingRow*> m_rows;
    QVariantMap m_overrides;
    QHash<QString, QString> m_errors; // rows that show a validation error of their own
    bool m_cards = true;
    bool m_built = false;
};

} // namespace fw::ui
