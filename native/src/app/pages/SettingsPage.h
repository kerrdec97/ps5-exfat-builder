#pragma once

// Settings (Settings artboard), generated from the SettingsSchema: section
// list (200 px) · section header ("n settings differ", Show advanced, Reset
// section) · cards per group of SettingRows · dependency rows (Tools and
// "Tools this section depends on") · search · Import legacy settings… ·
// Reset all… · temporary-files row (Storage).

#include "app/pages/Page.h"
#include "app/ui/DependencyRow.h"
#include "app/ui/SettingRow.h"

#include <QHash>
#include <QPushButton>

namespace fw::pages {

class SettingsPage : public Page {
    Q_OBJECT
public:
    SettingsPage(AppContext& ctx, QWidget* parent = nullptr);
    void navigateWith(const QVariantMap& args) override;
    void refresh() override;
    QString statusContext() const override;
    QStringList demoStates() const override;
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;
    void showSection(const QString& section);
    QString currentSection() const { return m_section; }
    QStringList sections() const { return m_sections; }
    ui::SettingRow* row(const QString& key) const { return m_rows.value(key); }
    void setShowAdvanced(bool on);
    bool showAdvanced() const { return m_advanced; }
    int attentionCount() const; // dependency problems + invalid values
    void setSearchText(const QString& text);
signals:
    void attentionChanged();

private:
    void buildSection(const QString& section);
    void rebuild();
    void applyValue(const QString& key, const QVariant& value);
    void refreshRow(const QString& key);
    void refreshDependencyStates();
    void updateSectionHeader();
    void updateSectionBadges();
    void importLegacy();
    void importLegacyFrom(const QString& path);
    void resetAll();
    void updateTempUsage();
    QString sectionCaption(const QString& section) const;

    QStringList m_sections;
    QString m_section;
    bool m_advanced = false;
    QLineEdit* m_search;
    QWidget* m_sectionList;
    QVBoxLayout* m_sectionLayout;
    QHash<QString, QAbstractButton*> m_sectionButtons;
    QHash<QString, ui::Badge*> m_sectionBadges;
    QWidget* m_pane;
    QVBoxLayout* m_paneLayout;
    QLabel* m_secTitle;
    QLabel* m_secCaption;
    ui::Switch* m_advancedSwitch;
    QPushButton* m_resetSection;
    QWidget* m_cards;
    QVBoxLayout* m_cardsLayout;
    QHash<QString, ui::SettingRow*> m_rows;
    QHash<QString, QString> m_errors;
    ui::InfoBar* m_fixBar = nullptr;
    ui::DependencyList* m_deps = nullptr;
    QLabel* m_tempUsage = nullptr;
    QPushButton* m_tempClear = nullptr;
    bool m_restartNeeded = false;
    QPointer<QWidget> m_demoSheet;
};

} // namespace fw::pages
