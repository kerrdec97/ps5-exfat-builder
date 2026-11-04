#pragma once

// The Discord webhook notification (legacy `_discord_notify`): when the queue
// finishes, one POST of a JSON embed to general.discordWebhookUrl.
//
//  * Only what a channel needs: counts and, per job, its kind and title. Any
//    absolute path in a title is cut down to its file name (withoutFolders),
//    so folder names never leave the PC. Mentions are disabled
//    (allowed_mentions), so a title like "@everyone" pings nobody.
//  * Nothing is sent when the setting is empty or not a Discord webhook
//    address, when FW_OFFLINE is set (tests, CI), or when the run had no job
//    worth reporting (update checks and diagnostics are left out by the app).
//  * The webhook address holds a secret token: it is never logged
//    (redactWebhookUrl), and problem reports hide it too.
//
// Lives on the UI thread; QNetworkAccessManager is asynchronous.

#include "core/Settings.h"
#include "jobs/JobQueue.h"
#include "jobs/QueueCompletionWatcher.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QUrl>

#include <functional>
#include <vector>

class QNetworkAccessManager;

namespace fw::notify {

struct QueueRunItem {
    QString kind;
    QString kindName; // "Build exFAT image"
    QString title;    // already without folders
    JobState state = JobState::Succeeded;
};

struct QueueRunSummary {
    int succeeded = 0;
    int failed = 0;    // failed or interrupted
    int cancelled = 0;
    QList<QueueRunItem> items;
    bool isEmpty() const { return items.isEmpty(); }
};

using KindName = std::function<QString(const QString& kind)>;
using KindFilter = std::function<bool(const QString& kind)>;

// Summary of the finished jobs of a run. Jobs `include` rejects, and jobs
// that are not finished, are left out; a job id listed twice counts once.
QueueRunSummary summarizeRun(const std::vector<JobRecord>& records, const KindName& kindName = {},
                             const KindFilter& include = {});

// Cuts every absolute path in `text` (C:\…, \\server\share\…, /…, ~/…) down to
// its last segment: "Built D:\Dumps\Astro Bot\x.exfat" → "Built x.exfat".
QString withoutFolders(const QString& text);

// Hides the token of any Discord webhook address in `text` (logs, reports).
QString redactWebhookUrl(const QString& text);

// The request body: one embed with a title, the counts, up to ten jobs, a
// colour (green / amber when something was cancelled / red on failures), the
// app name and version, and `when` (UTC).
QByteArray discordPayload(const QueueRunSummary& summary, const QDateTime& when = QDateTime::currentDateTimeUtc());

// False when FW_OFFLINE is set to anything but "0".
bool networkAllowed();

class DiscordNotifier : public QObject {
    Q_OBJECT
public:
    explicit DiscordNotifier(SettingsStore& settings, QObject* parent = nullptr);
    ~DiscordNotifier() override;

    // Posts a summary every time `watcher` reports a finished queue, from the
    // records `queue` still holds for that run.
    void watch(JobQueue& queue, QueueCompletionWatcher& watcher, KindName kindName = {}, KindFilter include = {});

    // Sends `summary` to the configured webhook. Returns false, sending
    // nothing, when the setting is empty or invalid, the app is offline or the
    // summary is empty. The answer arrives as delivered().
    bool notify(const QueueRunSummary& summary);

    // Tests: deliver to this address instead of the configured one. The setting
    // must still hold a valid webhook: it stays the on/off switch.
    void setDeliveryUrlForTests(const QUrl& url) { m_testUrl = url; }

signals:
    // ok = HTTP 2xx. `message` is safe to show and log (no token).
    void delivered(bool ok, int httpStatus, const QString& message);

private:
    SettingsStore& m_settings;
    QNetworkAccessManager* m_nam;
    QUrl m_testUrl;
};

} // namespace fw::notify
