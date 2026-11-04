#pragma once

// About: version and licence, Check for updates, Report a problem…, keyboard
// shortcuts, credits (from the legacy README), third-party notices.

#include "app/pages/Page.h"

namespace fw::pages {

class AboutPage : public Page {
    Q_OBJECT
public:
    explicit AboutPage(AppContext& ctx, QWidget* parent = nullptr);
    QString statusContext() const override;
    void setUpdateStatus(const QString& text, ui::Tone tone);
signals:
    void checkForUpdatesRequested();
    void reportProblemRequested();
    void shortcutsRequested();

private:
    ui::Badge* m_updateBadge;
    QPushButton* m_checkUpdates;
};

// The shortcut table shared by the About page and the help menu dialog.
struct ShortcutInfo {
    QString keys;
    QString action;
};
QList<ShortcutInfo> shortcutTable();

} // namespace fw::pages
