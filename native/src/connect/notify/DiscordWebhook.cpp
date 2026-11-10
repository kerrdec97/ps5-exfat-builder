#include "connect/notify/DiscordWebhook.h"

#include "connect/notify/Module.h"
#include "core/BuildInfo.h"
#include "core/Log.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>

namespace fw::notify {

namespace {

const Log& notifyLog()
{
    static const Log log(QStringLiteral("notify"));
    return log;
}

constexpr int kMaxItems = 10;
// Embed colours (data sent to Discord, not UI colours): the legacy green, the
// app's warning amber and danger red.
constexpr int kGreen = 0x4caf50;
constexpr int kAmber = 0xe0a030;
constexpr int kRed = 0xe5484d;

bool isFinished(JobState s)
{
    return s == JobState::Succeeded || s == JobState::Failed || s == JobState::Cancelled || s == JobState::Interrupted;
}

QString plural(int n, const QString& one, const QString& many)
{
    return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? one : many);
}

// Discord markdown would turn "*" and "_" in game titles into formatting, and
// "[text](address)" into a link that hides where it goes.
QString escapeMarkdown(const QString& s)
{
    QString out;
    out.reserve(s.size() + 8);
    for (const QChar c : s) {
        if (c == u'\\' || c == u'*' || c == u'_' || c == u'~' || c == u'`' || c == u'|' || c == u'>' || c == u'['
            || c == u']' || c == u'(' || c == u')')
            out += u'\\';
        out += c;
    }
    return out;
}

} // namespace

QueueRunSummary summarizeRun(const std::vector<JobRecord>& records, const KindName& kindName, const KindFilter& include)
{
    QueueRunSummary s;
    QSet<QString> seen;
    for (const JobRecord& r : records) {
        if (!isFinished(r.state) || seen.contains(r.id) || (include && !include(r.kind)))
            continue;
        seen.insert(r.id);
        QueueRunItem item;
        item.kind = r.kind;
        item.kindName = kindName ? kindName(r.kind) : r.kind;
        item.title = withoutFolders(r.title).simplified();
        item.state = r.state;
        switch (r.state) {
        case JobState::Succeeded: ++s.succeeded; break;
        case JobState::Cancelled: ++s.cancelled; break;
        default: ++s.failed; break;
        }
        s.items << item;
    }
    return s;
}

QString withoutFolders(const QString& text)
{
    // A path root (drive, UNC share, "/", "~/") followed by folder segments;
    // the segments may hold spaces but never a separator, ':' or a line break,
    // so the match ends at the last separator and the file name stays.
    static const QRegularExpression folders(
        QStringLiteral("(?:(?<![A-Za-z0-9])[A-Za-z]:[\\\\/]|\\\\\\\\[^\\\\/\\s]+[\\\\/]|~[\\\\/]|(?<![A-Za-z0-9_:/.~\\\\-])/(?!\\s))"
                       "(?:[^\\\\/:*?\"<>|\\r\\n]*[\\\\/])*"));
    QString out = text;
    out.replace(folders, QString());
    return out;
}

QString redactWebhookUrl(const QString& text)
{
    static const QRegularExpression token(
        QStringLiteral("(https?://(?:[A-Za-z0-9-]+\\.)*discord(?:app)?\\.com/api/(?:v\\d+/)?webhooks/\\d+/)[A-Za-z0-9_\\-]+"),
        QRegularExpression::CaseInsensitiveOption);
    QString out = text;
    return out.replace(token, QStringLiteral("\\1<token hidden>"));
}

QByteArray discordPayload(const QueueRunSummary& s, const QDateTime& when)
{
    const QString title = s.failed > 0 ? QStringLiteral("Queue finished with %1").arg(plural(s.failed, QStringLiteral("failure"), QStringLiteral("failures")))
                                       : QStringLiteral("Queue finished");
    QStringList counts;
    counts << QStringLiteral("%1 done").arg(plural(s.succeeded, QStringLiteral("job"), QStringLiteral("jobs")));
    if (s.failed > 0)
        counts << QStringLiteral("%1 failed").arg(s.failed);
    if (s.cancelled > 0)
        counts << QStringLiteral("%1 cancelled").arg(s.cancelled);
    QStringList lines;
    lines << counts.join(QStringLiteral(" · ")) << QString();
    for (int i = 0; i < s.items.size() && i < kMaxItems; ++i) {
        const QueueRunItem& it = s.items[i];
        const QString mark = it.state == JobState::Succeeded ? QStringLiteral("✅")
                           : it.state == JobState::Cancelled ? QStringLiteral("⚪")
                                                             : QStringLiteral("❌");
        lines << QStringLiteral("%1 %2: %3").arg(mark, escapeMarkdown(it.kindName), escapeMarkdown(it.title.left(150)));
    }
    if (s.items.size() > kMaxItems)
        lines << QStringLiteral("… and %1 more").arg(s.items.size() - kMaxItems);
    QJsonObject embed{
        {QStringLiteral("title"), QStringLiteral("\U0001F3AE ") + title},
        {QStringLiteral("description"), lines.join(u'\n').left(4000)},
        {QStringLiteral("color"), s.failed > 0 ? kRed : s.cancelled > 0 ? kAmber : kGreen},
        {QStringLiteral("footer"), QJsonObject{{QStringLiteral("text"), QStringLiteral("%1 %2 %3").arg(QLatin1String(build::kProductName),
                                                                                                    QLatin1String(build::kEditionName),
                                                                                                    QLatin1String(build::kVersion))}}},
        {QStringLiteral("timestamp"), when.toUTC().toString(QStringLiteral("yyyy-MM-ddTHH:mm:ssZ"))},
    };
    const QJsonObject body{{QStringLiteral("embeds"), QJsonArray{embed}},
                           {QStringLiteral("allowed_mentions"), QJsonObject{{QStringLiteral("parse"), QJsonArray{}}}}};
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

bool networkAllowed()
{
    return !qEnvironmentVariableIsSet("FW_OFFLINE") || qgetenv("FW_OFFLINE") == "0";
}

DiscordNotifier::DiscordNotifier(SettingsStore& settings, QObject* parent)
    : QObject(parent), m_settings(settings), m_nam(new QNetworkAccessManager(this))
{
}

DiscordNotifier::~DiscordNotifier() = default;

void DiscordNotifier::watch(JobQueue& queue, QueueCompletionWatcher& watcher, KindName kindName, KindFilter include)
{
    QPointer<QueueCompletionWatcher> w(&watcher);
    connect(&watcher, &QueueCompletionWatcher::queueFinished, this,
            [this, &queue, w, kindName = std::move(kindName), include = std::move(include)](int, int, int) {
                if (!w)
                    return;
                std::vector<JobRecord> records;
                for (const QString& id : w->lastRunJobIds())
                    if (auto r = queue.record(id))
                        records.push_back(*r);
                (void)notify(summarizeRun(records, kindName, include));
            });
}

bool DiscordNotifier::notify(const QueueRunSummary& summary)
{
    const QString url = m_settings.value(keys::kDiscordWebhookUrl).toString().trimmed();
    if (url.isEmpty() || summary.isEmpty())
        return false;
    if (!discordWebhookProblem(url).isEmpty()) {
        notifyLog().warn(QStringLiteral("Discord notification not sent: the webhook address in Settings › General is not a Discord webhook."));
        return false;
    }
    if (!networkAllowed()) {
        notifyLog().debug(QStringLiteral("Discord notification not sent (FW_OFFLINE is set)."));
        return false;
    }
    QNetworkRequest req(m_testUrl.isValid() ? m_testUrl : QUrl(url));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("%1/%2").arg(QString::fromLatin1(build::kProductName).replace(u' ', u'-'),
                                                                                 QLatin1String(build::kVersion)));
    req.setTransferTimeout(10000);
    QNetworkReply* reply = m_nam->post(req, discordPayload(summary));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
        const QString message = ok ? QStringLiteral("Discord notification sent.")
                                   : QStringLiteral("Discord notification failed: %1").arg(
                                         status > 0 ? QStringLiteral("HTTP %1").arg(status) : redactWebhookUrl(reply->errorString()));
        if (ok)
            notifyLog().info(message);
        else
            notifyLog().warn(message);
        emit delivered(ok, status, message);
    });
    return true;
}

} // namespace fw::notify
