#pragma once

// Page base classes.
//
//  * Page        — scrollable content under a PageHeader that stays at the
//                  top (the title and the page's main buttons never scroll
//                  out of reach); navigation, F5, drops, Ctrl+B and
//                  screenshot "demo states" are virtuals.
//  * JobFormPage — the "Form + dock" template (§2.4): a left form column
//                  (max 760 px) and the 400 px Jobs dock; it owns the
//                  Add-to-queue / primary buttons, the unavailable state when a
//                  job kind is not registered, the job options panel and the
//                  submit flow through services/JobSubmit. When the primary
//                  button starts a job, the form column gives way to the job
//                  monitor (stages, clocks, live log) until the user asks for
//                  the form back ("New build") or the job has been over for a
//                  while; "Watch" in the dock opens any running job there.

#include "app/ui/JobMonitor.h"
#include "app/ui/JobRows.h"
#include "app/ui/SettingRow.h"
#include "app/ui/Shell.h"
#include "app/ui/Widgets.h"
#include "core/Result.h"

#include <QScrollArea>
#include <QUrl>
#include <QVariantMap>
#include <QWidget>

#include <functional>

namespace fw {
class AppContext;
class JobQueueModel;
class LogModel;
} // namespace fw

namespace fw::pages {

struct NavRequest {
    QString page;
    QVariantMap args; // e.g. {"source": "D:/Dumps/PPSA01325"}, {"jobId": "..."}, {"section": "Tools"}
};

class Page : public QWidget {
    Q_OBJECT
public:
    Page(AppContext& ctx, const QString& key, const QString& title, const QString& caption, QWidget* parent = nullptr);
    QString key() const { return m_key; }
    QString title() const { return m_title; }
    ui::PageHeader* header() { return m_header; }
    QVBoxLayout* content() { return m_content; }
    QScrollArea* scrollArea() { return m_scroll; }
    // The strip above the scroll area that holds the header. It is part of
    // the page while the content is at its top and gets a hairline once the
    // content has moved under it.
    QWidget* headerBar() const { return m_headerBar; }
    bool isHeaderLined() const;

    virtual bool hasPrimaryAction() const { return false; }
    virtual void primaryAction() {}
    virtual void refresh() {}                                  // F5
    virtual bool acceptsDrop(const QList<QUrl>& urls) const { Q_UNUSED(urls); return false; }
    virtual void dropUrls(const QList<QUrl>& urls) { Q_UNUSED(urls); }
    virtual void setDragHighlight(bool on) { Q_UNUSED(on); }
    virtual void navigateWith(const QVariantMap& args) { Q_UNUSED(args); }
    virtual QString statusContext() const { return {}; }     // status bar, right side
    virtual void onShown() {}
    // Edits that exist only in this window and would be lost on exit, as the
    // middle of a sentence ("autoload.txt has edits that are not saved");
    // "" when there are none. The window asks before it closes.
    virtual QString unsavedChanges() const { return {}; }
    // Screenshot states (CI evidence): names and how to enter/leave them.
    virtual QStringList demoStates() const { return {}; }
    virtual void applyDemoState(const QString& name) { Q_UNUSED(name); }
    virtual void clearDemoState() {}
signals:
    void navigateRequested(const fw::pages::NavRequest& request);
    void toastRequested(fw::ui::Tone tone, const QString& title, const QString& body);
    // A toast with one button ("Start queue").
    void toastActionRequested(fw::ui::Tone tone, const QString& title, const QString& body, const QString& actionText,
                              const std::function<void()>& action);
    void statusContextChanged();

protected:
    AppContext& m_ctx;

private:
    QString m_key, m_title;
    QWidget* m_headerBar;
    ui::PageHeader* m_header;
    QScrollArea* m_scroll;
    QVBoxLayout* m_content;
};

class JobFormPage : public Page {
    Q_OBJECT
public:
    JobFormPage(AppContext& ctx, JobQueueModel& queueModel, const QString& key, const QString& title,
                const QString& caption, const QString& primaryText, const QString& featureName, QWidget* parent = nullptr);
    bool hasPrimaryAction() const override { return true; }
    // Ctrl+B acts on the form the user sees, not on the one hidden behind the monitor.
    void primaryAction() override
    {
        if (m_view == View::Form)
            submitClicked(true);
    }
    ui::JobsDock* dock() { return m_dock; }
    QVBoxLayout* form() { return m_form; }
    QVBoxLayout* aside() { return m_aside; }
    QPushButton* primaryButton() { return m_primary; }
    QPushButton* queueButton() { return m_queue; }
    bool isAvailable() const { return m_available; }
    QStringList demoStates() const override { return {QStringLiteral("error")}; }
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;
    void setDragHighlight(bool on) override;
    ui::Card* sourceCard() const { return m_sourceCard; }
    // "Open the output folder when done" for the job being submitted; a page
    // with its own switch for it answers from that one.
    virtual bool openOutputWhenDone() const;

    // What the left column shows: the form, or the job monitor that replaces
    // it while a job the page started runs.
    enum class View { Form, Monitor };
    View view() const { return m_view; }
    ui::JobMonitor* monitor() { return m_monitor; }
    bool isMonitorShown() const { return m_view == View::Monitor; }
    void showMonitor(const QString& id);
    void showForm();
    void setNewJobText(const QString& text); // the header button that brings the form back
    QPushButton* newJobButton() { return m_newJob; }
    // Whether the primary button switches to the monitor (default) or keeps
    // the form, for actions whose result lives in the form (Image tools › Compare).
    void setMonitorOnStart(bool on) { m_monitorOnStart = on; }
    bool monitorOnStart() const { return m_monitorOnStart; }
signals:
    void jobSubmitted(const QString& id, bool openOutputFolderWhenDone);
    void monitorShownChanged(bool shown);

protected:
    void showEvent(QShowEvent* e) override;
    // Job kinds the page needs; the unavailable state shows when any is missing.
    void setRequiredKinds(const QStringList& kinds);
    void refreshAvailability();
    // Pages implement validation + submission; return the job id.
    virtual Result<QString> submit(bool buildNow) = 0;
    // Called before submit; return a user-facing message to block submission.
    virtual QString validateForm() { return {}; }
    void showFormError(const QString& title, const QString& message);
    void clearFormError();
    // The first thing the form already knows to be missing ("Choose the game
    // dump first."), shown as the tooltip of the two start buttons; "" when
    // nothing is known to be missing. Only what the page has at hand: this is
    // not validateForm() and must cost nothing.
    void setStartHint(const QString& hint);
    QString startHint() const { return m_startHint; }
    void setSourceCard(ui::Card* card) { m_sourceCard = card; }
    // Standard "job settings" card with the schema's perJob settings.
    ui::JobOptionsPanel* addJobOptionsCard(const QString& title, const QStringList& sections,
                                          const QStringList& excludeKeys, const QString& settingsLink = {},
                                          bool withOpenFolderRow = true);
    ui::JobOptionsPanel* options() { return m_options; }
    // After a page showed or hid rows of the job options card: its "n changed" summary again.
    void refreshOptionsSummary()
    {
        if (m_refreshOptionsSummary)
            m_refreshOptionsSummary();
    }
    QString featureName() const { return m_feature; }
    // For a page whose outputs differ (Build: image, package, packed folder):
    // what the "not available in this build" state names.
    void setFeatureName(const QString& name);

private:
    void submitClicked(bool buildNow);
    void applyStartTip();
    QString m_feature;
    QString m_startHint;
    QStringList m_kinds;
    bool m_available = true;
    void setView(View view);
    ui::JobsDock* m_dock;
    QWidget* m_formWidget;
    QWidget* m_monitorHost;
    ui::JobMonitor* m_monitor;
    View m_view = View::Form;
    bool m_monitorOnStart = true;
    QVBoxLayout* m_form;
    QVBoxLayout* m_aside;
    QWidget* m_submitBar; // Add to queue + the primary button (hidden as one in the Monitor view)
    QPushButton* m_primary;
    QPushButton* m_queue;
    QPushButton* m_newJob;
    ui::UnavailableState* m_unavailable = nullptr;
    ui::Card* m_unavailableCard = nullptr;
    ui::InfoBar* m_error;
    ui::JobOptionsPanel* m_options = nullptr;
    std::function<void()> m_refreshOptionsSummary;
    ui::Switch* m_openFolder = nullptr;
    ui::Card* m_sourceCard = nullptr;
};

// Shared helpers for pages
//
// Text that comes from outside the app (titles, IDs, names and paths from
// dumps, ZIPs, images and packages, messages that quote them) needs no helper
// here: the labels of the widget library are plain text, and tooltips go
// through ui::plainTip() or the application-wide tooltip rule (ui/Style.h).
QString humanTime(const QDateTime& t);            // "2026-11-04 14:36"
QString relativeTime(const QDateTime& t);         // "2 min ago"
QString driveLabel(const QString& path);          // "D:" / "/"
// What the drive of `folder` (or of the nearest folder above it that exists)
// reports. Blocking: a mapped network drive or an unplugged letter can take
// seconds, so pages use probeFreeSpace(); screenshot runs, which render a page
// at once, ask here.
struct DriveSpace {
    bool ready = false;
    qint64 available = -1;
    QString label; // "D:" / "/" / "this drive"
};
DriveSpace driveSpaceOf(const QString& folder);
// Asks the drive of `folder` for its free space on a worker thread (a mapped
// network drive or an unplugged letter can block for seconds) and calls
// `done(ready, bytesAvailable, driveLabel)` on the UI thread while `context`
// still exists. Callers keep a generation counter to drop stale answers.
void probeFreeSpace(QObject* context, const QString& folder, std::function<void(bool ready, qint64 available, const QString& label)> done);

} // namespace fw::pages

Q_DECLARE_METATYPE(fw::pages::NavRequest)
