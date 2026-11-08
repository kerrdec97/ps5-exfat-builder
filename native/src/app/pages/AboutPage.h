#pragma once

// About: the product card (brand mark, name, version, update status, Check
// for updates), link cards (GitHub, Report a problem, What's new, Keyboard
// shortcuts), credits, licence and the folded third-party notices.

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
