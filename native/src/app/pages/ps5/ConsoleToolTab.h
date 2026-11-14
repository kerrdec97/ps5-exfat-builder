#pragma once

// Base of the console-content tabs on the PS5 connect page (Games on the
// console, Fakelib, Autoloader, AMPR index, ShadowMount+ settings and log,
// Crash dumps, Install package). The tabs live in files under pages/ps5/.
//
// Console work runs on a pool thread (QtConcurrent: Qt sockets need a Qt
// thread) over a connection of its own, opened for that piece of work and
// closed after it, like the legacy tabs did. It never touches the page's FTP
// browser session (ConsoleSession), whose requests are serialised on its own
// worker. One piece of work at a time per tab; the result is delivered on the
// UI thread, and not at all once the tab is gone (the destructor cancels and
// waits).

#include "app/ui/Widgets.h"
#include "connect/ps5/Endpoint.h"
#include "connect/ps5/FtpClient.h"
#include "connect/ps5/FtpTransfer.h"
#include "core/Cancellation.h"
#include "core/Result.h"
#include "core/Tr.h"

#include <QCoreApplication>
#include <QFuture>
#include <QPointer>
#include <QTableWidget>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

#include <functional>
#include <memory>

namespace fw {
class AppContext;
}

namespace fw::pages {

class ConsoleToolTab : public QWidget {
    Q_OBJECT
public:
    // The console address and ports as the page shows them (header fields over
    // the ps5.* settings).
    using EndpointFn = std::function<ps5::ConsoleEndpoint()>;

    ConsoleToolTab(AppContext& ctx, EndpointFn endpointFn, const QString& demoKey, QWidget* parent = nullptr);
    ~ConsoleToolTab() override;

    // Screenshot state name ("manager", "fakelib", ...) and how to show it.
    QString demoKey() const { return m_demoKey; }
    virtual void applyDemo() {}
    virtual void clearDemo() {}

    bool isBusy() const { return m_busy; }
    void cancelWork();
    // Why the last runOnConsole() / runLocal() did not start, in the user's words
    // ("" when it did): a tab shows this instead of guessing the reason.
    enum class NotStarted { None, NoHost, Busy };
    NotStarted lastNotStarted() const { return m_notStarted; }
    QString whyNotStarted() const;

signals:
    void toastRequested(fw::ui::Tone tone, const QString& title, const QString& body);
    void busyChanged(bool busy);

protected:
    template <typename T>
    using ConsoleWork = std::function<Result<T>(ps5::FtpClient& client, const CancellationToken& token)>;
    template <typename T>
    using LocalWork = std::function<Result<T>(const CancellationToken& token)>;
    template <typename T>
    using Done = std::function<void(const Result<T>& result)>;

    ps5::ConsoleEndpoint endpoint() const { return m_endpoint(); }

    // Opens a connection to the console, runs `work`, closes it; `done` gets
    // the result on the UI thread. Returns false (and runs nothing) while
    // another piece of work is running or when there is no console address.
    template <typename T>
    bool runOnConsole(ConsoleWork<T> work, Done<T> done);
    // Same without a connection (local files).
    template <typename T>
    bool runLocal(LocalWork<T> work, Done<T> done);
    // Runs `fn` on the UI thread later, unless the tab is gone (progress from work).
    void postToUi(std::function<void()> fn);

    AppContext& m_ctx;

private:
    bool beginWork();
    void endWork();

    EndpointFn m_endpoint;
    QString m_demoKey;
    bool m_busy = false;
    NotStarted m_notStarted = NotStarted::None;
    std::unique_ptr<CancellationSource> m_cancel;
    QFuture<void> m_future;
};

template <typename T>
bool ConsoleToolTab::runOnConsole(ConsoleWork<T> work, Done<T> done)
{
    const ps5::ConsoleEndpoint ep = endpoint();
    if (ep.host.isEmpty()) {
        m_notStarted = NotStarted::NoHost;
        emit toastRequested(ui::Tone::Warn, FW_TR("No console address"),
                            FW_TR("Enter the console's IP address at the top of the page first."));
        return false;
    }
    return runLocal<T>(
        [ep, work](const CancellationToken& token) -> Result<T> {
            ps5::FtpClient client(ps5::ftpOptionsFor(ep));
            if (auto st = client.open(ep.host, ep.ftpPort, token); !st.ok())
                return st.takeError();
            Result<T> r = work(client, token);
            client.quit();
            return r;
        },
        std::move(done));
}

template <typename T>
bool ConsoleToolTab::runLocal(LocalWork<T> work, Done<T> done)
{
    if (!beginWork()) {
        m_notStarted = NotStarted::Busy;
        return false;
    }
    m_notStarted = NotStarted::None;
    const CancellationToken token = m_cancel->token();
    QPointer<ConsoleToolTab> self(this);
    m_future = QtConcurrent::run([self, token, work, done]() {
        auto result = std::make_shared<Result<T>>(work(token));
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [self, result, done]() {
                if (!self)
                    return;
                self->endWork();
                done(*result);
            },
            Qt::QueuedConnection);
    });
    return true;
}

// PS5 connect has one gradient button per view (DESIGN_V5, principle 6):
// while no console is connected, the console card's Connect; once one is,
// the open tool's own main action. A tool marks that button with
// setToolPrimary() (also when its state changes, e.g. Connect / Disconnect);
// it is drawn Primary only while the page it sits on has no Connect to offer.
// Outside the page (a tab on its own) the mark is the variant.
void setToolPrimary(QPushButton* button, bool primary = true);
// The page: whether its console card offers Connect, and the marked buttons
// under `root` drawn accordingly.
void setConnectOffered(QWidget* root, bool offered);

// Shared look for the tabs' tables (header, row height, no grid, whole-row
// selection, read-only) and their cells.
void styleToolTable(QTableWidget* table, int minimumRows);
QTableWidgetItem* toolItem(const QString& text, bool mono = false, const QColor& color = theme::color::fg_1);
// A badge on the left of a cell; the cell's item holds the text in Qt::UserRole.
void setBadgeCell(QTableWidget* table, int row, int column, const QString& text, ui::Tone tone, const QString& icon = {});
// What a cell keeps around a widget placed in it: the item padding of the
// style sheet (the delegate puts cell widgets in the item's text rectangle).
int cellPadding(QTableWidget* table);
// A column of setBadgeCell() badges as wide as its widest badge in the
// language shown (with the tone's glyph, the badge's margins and the cell's
// padding) and its title, so no state is cut ("Größe abweichend").
int badgeColumnWidth(QTableWidget* table, int column, const QStringList& badges);

} // namespace fw::pages
