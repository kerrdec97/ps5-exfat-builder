#pragma once

// PS5 connect › AMPR index (legacy Advanced › AMPR, ADV-21): generate
// ampr_emu.index for a local app0 folder (ampr.index.local) or for a game
// folder on the console, uploaded with a .bak of the old index
// (ampr.index.ftp). Both are queue jobs; results show in the Queue.

#include "app/pages/ps5/ConsoleToolTab.h"

namespace fw::pages {

class AmprTab : public ConsoleToolTab {
    Q_OBJECT
public:
    AmprTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent = nullptr);
    void applyDemo() override;
    void clearDemo() override;

    // Test hooks (no dialogs). Return the queued job id, or empty.
    QString submitLocal(const QString& folder, bool force);
    QString submitConsole(const QString& remoteFolder);

private:
    void generateLocal();
    void generateConsole();
    void browseConsole();
    void updateAvailability();

    ui::PathField* m_localFolder;
    QPushButton* m_localGo;
    ui::MessageRow* m_localStatus;
    QLineEdit* m_remoteFolder;
    QPushButton* m_remoteBrowse;
    QPushButton* m_remoteGo;
    ui::MessageRow* m_remoteStatus;
};

} // namespace fw::pages
