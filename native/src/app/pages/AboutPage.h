#pragma once

// About: the product card (brand mark, name, version, update status, Check
// for updates and the action a found update offers), link cards (GitHub, Report a problem, What's new, Keyboard
// shortcuts), credits, licence and the folded third-party notices.

#include "app/pages/Page.h"

namespace fw::pages {

class AboutPage : public Page {
    Q_OBJECT
public:
    explicit AboutPage(AppContext& ctx, QWidget* parent = nullptr);
    QString statusContext() const override;
    void setUpdateStatus(const QString& text, ui::Tone tone);
    // The button beside "Check for updates" for what a found update offers
    // ("Download", "Open the release page", "Show in folder"); "" hides it.
    void setUpdateAction(const QString& text, const QString& toolTip);
    // The rows of the third-party table: component, use, licence. They follow
    // packaging/THIRD_PARTY_NOTICES-native.md (its table, then the bundled
    // separate program).
    static QList<QStringList> thirdPartyComponents();
    // The line under the licence: what a private build carries (looked up in
    // `toolsDir`, the app's tools folder), or the public build's text.
    static QString affiliationNote(const QString& toolsDir);
signals:
    void checkForUpdatesRequested();
    void updateActionRequested();
    void reportProblemRequested();
    void shortcutsRequested();

private:
    ui::Badge* m_updateBadge;
    QPushButton* m_checkUpdates;
    QPushButton* m_updateAction;
};

// The shortcut table shared by the About page and the help menu dialog.
struct ShortcutInfo {
    QString keys;
    QString action;
};
QList<ShortcutInfo> shortcutTable();

} // namespace fw::pages
