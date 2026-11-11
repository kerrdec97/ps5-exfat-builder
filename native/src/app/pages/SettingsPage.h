#pragma once

// Settings, generated from the SettingsSchema: search (Ctrl+F) in the page
// header · section list with icons, Import legacy settings… and Reset all…
// under it · section header ("n changed" badge, Reset section) · cards per
// group of SettingRows, with the section's advanced settings folded into an
// "Advanced" Disclosure · dependency rows (Tools and "Tools this section
// depends on") · "Where the app keeps its files" with the portable-mode row
// and the temporary-files row (Storage) · legacy import (Advanced).

#include "app/pages/Page.h"
#include "app/ui/DependencyRow.h"
#include "app/ui/SettingRow.h"

#include <QHash>
#include <QPointer>
#include <QPushButton>

namespace fw::ui {
class Disclosure;
class Sheet;
} // namespace fw::ui

namespace fw::portable {
struct Plan;
}

class QCheckBox;

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

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void buildSection(const QString& section);
    void rebuild();
    ui::SettingRow* addRow(const SettingDef& def, QWidget* parent, QVBoxLayout* layout, bool divider);
    void applyValue(const QString& key, const QVariant& value);
    void refreshRow(const QString& key);
    void refreshDependencyStates();
    void updateSectionHeader();
    void updateSectionBadges();
    void updateAdvancedSummary();
    void fitLabels();
    void setCompactList(bool compact); // icon-only section list on narrow windows
    void syncSectionBar(bool animate); // the section list's one sliding selection bar
    void importLegacy();
    void importLegacyFrom(const QString& path);
    void resetAll();
    void updateTempUsage();
    QString sectionCaption(const QString& section) const;
    // Storage: where the program, settings, logs and temporary files are, and portable mode.
    void addLocationsCard();
    QString tempLocation() const; // the temporary folder in use (the chosen one, else the app's own)
    void makePortable();
    ui::Sheet* buildPortableSheet(const portable::Plan& plan, QCheckBox*& copy);
    // General › Language: the folder of the user's catalogues, and the bar
    // that asks for a restart once another language is chosen.
    void addLanguageExtras(QWidget* card, QVBoxLayout* layout);
    void updateLanguageBar();
    void restartForLanguage();

    QStringList m_sections;
    QString m_section;
    bool m_advanced = false;
    QLineEdit* m_search;
    QTimer* m_searchDelay = nullptr;
    QWidget* m_columns;
    QWidget* m_sectionList;
    QPushButton* m_importBtn;
    QPushButton* m_resetAllBtn;
    bool m_compactList = false;
    QVBoxLayout* m_sectionLayout;
    QHash<QString, QAbstractButton*> m_sectionButtons;
    QHash<QString, ui::Badge*> m_sectionBadges;
    QWidget* m_sectionBar = nullptr;
    ui::motion::Value* m_sectionBarY = nullptr;
    QWidget* m_pane;
    QVBoxLayout* m_paneLayout;
    QLabel* m_secTitle;
    ui::Badge* m_secChanged;
    QLabel* m_secCaption;
    ui::Link* m_secLink; // Tools: where the bundled components are (opens Storage)
    QPushButton* m_resetSection;
    QWidget* m_cards;
    QVBoxLayout* m_cardsLayout;
    QPointer<ui::Disclosure> m_advancedFold;
    QStringList m_advancedKeys;
    QHash<QString, ui::SettingRow*> m_rows;
    QHash<QString, QString> m_errors;
    ui::InfoBar* m_fixBar = nullptr;
    ui::DependencyList* m_deps = nullptr;
    QLabel* m_tempUsage = nullptr;
    QPushButton* m_tempClear = nullptr;
    QPointer<QWidget> m_locationsCard;
    QPointer<ui::ElideLabel> m_tempLocation;
    QPointer<ui::InfoBar> m_langBar;
    bool m_restartNeeded = false;
    QPointer<QWidget> m_demoSheet;
};

} // namespace fw::pages
