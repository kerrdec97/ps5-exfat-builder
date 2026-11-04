#pragma once

// PS5 connect › Fakelib (legacy ShadowMount+ › Fakelib Manager, SMP-24): the
// .prx/.sprx overlays in /data/shadowmount/fakelib and its disabled folder
// (connect/ps5/Fakelib.h). Every action opens its own FTP connection, then
// lists the folders again.

#include "app/pages/ps5/ConsoleToolTab.h"
#include "connect/ps5/Fakelib.h"

namespace fw::pages {

class FakelibTab : public ConsoleToolTab {
    Q_OBJECT
public:
    FakelibTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent = nullptr);
    void applyDemo() override;
    void clearDemo() override;

    // Test hooks (no dialogs; `confirmed` stands for the user's answer).
    void refresh();
    void addFile(const QString& localPath, bool confirmed);
    void replaceSelected(const QString& localPath, bool confirmed);
    void removeSelected();
    void toggleSelected();
    QTableWidget* table() { return m_table; }
    const ps5::fakelib::Listing& listing() const { return m_listing; }
    QString statusText() const { return m_status->text(); }

private:
    void showListing(const ps5::fakelib::Listing& listing);
    int selectedRow() const;
    void updateActions();
    // Runs `op` then lists again; `what` names the action in messages.
    void runAndRefresh(const QString& what, std::function<Status(ps5::FtpClient&, const CancellationToken&)> op);
    void askAdd();
    void askReplace();
    void askRemove();
    // Reject → message; Confirm → question. Returns false when the user stops.
    bool checkWithUser(const QString& localPath, bool* confirmed);

    QTableWidget* m_table;
    ui::EmptyState* m_empty;
    QLabel* m_status;
    QPushButton* m_add;
    QPushButton* m_replace;
    QPushButton* m_toggle;
    QPushButton* m_remove;
    ui::IconButton* m_refresh;
    ps5::fakelib::Listing m_listing;
};

} // namespace fw::pages
