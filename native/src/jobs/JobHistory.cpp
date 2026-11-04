#include "jobs/JobHistory.h"

#include "core/FileUtil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

namespace fw {

QJsonObject HistoryEntry::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), id);
    o.insert(QStringLiteral("kind"), kind);
    o.insert(QStringLiteral("title"), title);
    o.insert(QStringLiteral("state"), state);
    o.insert(QStringLiteral("verification"), verification);
    if (!summary.isEmpty()) o.insert(QStringLiteral("summary"), summary);
    if (!error.isEmpty()) o.insert(QStringLiteral("error"), error);
    o.insert(QStringLiteral("outputs"), QJsonArray::fromStringList(outputs));
    o.insert(QStringLiteral("started"), started.toString(Qt::ISODate));
    o.insert(QStringLiteral("finished"), finished.toString(Qt::ISODate));
    return o;
}

HistoryEntry HistoryEntry::fromJson(const QJsonObject& o)
{
    HistoryEntry h;
    h.id = o.value(QStringLiteral("id")).toString();
    h.kind = o.value(QStringLiteral("kind")).toString();
    h.title = o.value(QStringLiteral("title")).toString();
    h.state = o.value(QStringLiteral("state")).toString();
    h.verification = o.value(QStringLiteral("verification")).toString();
    h.summary = o.value(QStringLiteral("summary")).toString();
    h.error = o.value(QStringLiteral("error")).toString();
    for (const auto& v : o.value(QStringLiteral("outputs")).toArray())
        h.outputs << v.toString();
    h.started = QDateTime::fromString(o.value(QStringLiteral("started")).toString(), Qt::ISODate);
    h.finished = QDateTime::fromString(o.value(QStringLiteral("finished")).toString(), Qt::ISODate);
    return h;
}

JobHistory::JobHistory(QString path, int maxEntries) : m_path(std::move(path)), m_max(std::max(10, maxEntries)) {}

Status JobHistory::append(const JobRecord& r)
{
    HistoryEntry h;
    h.id = r.id;
    h.kind = r.kind;
    h.title = r.title;
    h.state = jobStateName(r.state);
    h.verification = r.outcome ? verificationName(r.outcome->verification) : verificationName(Verification::NotRun);
    h.summary = r.outcome ? r.outcome->summary : QString();
    h.error = r.error ? r.error->message : QString();
    h.outputs = r.outcome ? r.outcome->outputs : QStringList();
    h.started = r.started;
    h.finished = r.finished;

    std::lock_guard lock(m_mutex);
    QDir().mkpath(QFileInfo(m_path).absolutePath());
    QFile f(m_path);
    if (!f.open(QIODevice::Append))
        return Error(ErrorCode::IoError, QStringLiteral("Could not write the job history."), f.errorString());
    f.write(QJsonDocument(h.toJson()).toJson(QJsonDocument::Compact));
    f.write("\n");
    f.close();
    compactIfNeeded();
    return success();
}

std::vector<HistoryEntry> JobHistory::recent(int limit) const
{
    std::lock_guard lock(m_mutex);
    std::vector<HistoryEntry> out;
    QFile f(m_path);
    if (!f.open(QIODevice::ReadOnly))
        return out;
    const QList<QByteArray> lines = f.readAll().split('\n');
    for (auto it = lines.crbegin(); it != lines.crend() && int(out.size()) < limit; ++it) {
        if (it->trimmed().isEmpty())
            continue;
        const QJsonDocument d = QJsonDocument::fromJson(*it);
        if (d.isObject())
            out.push_back(HistoryEntry::fromJson(d.object()));
    }
    return out;
}

Status JobHistory::clear()
{
    std::lock_guard lock(m_mutex);
    if (QFile::exists(m_path) && !QFile::remove(m_path))
        return Error(ErrorCode::IoError, QStringLiteral("Could not clear the job history."));
    return success();
}

Status JobHistory::exportCsv(const QString& path) const
{
    auto entries = recent(1000000);
    std::reverse(entries.begin(), entries.end());
    const auto cell = [](QString v) {
        if (v.contains(u'"') || v.contains(u',') || v.contains(u'\n') || v.contains(u'\r')) {
            v.replace(QStringLiteral("\""), QStringLiteral("\"\""));
            v = u'"' + v + u'"';
        }
        return v;
    };
    QString csv = QStringLiteral("finished,started,title,kind,state,verification,summary,error,outputs\r\n");
    for (const auto& e : entries) {
        csv += QStringList{cell(e.finished.toString(Qt::ISODate)), cell(e.started.toString(Qt::ISODate)), cell(e.title),
                           cell(e.kind), cell(e.state), cell(e.verification), cell(e.summary), cell(e.error),
                           cell(e.outputs.join(QStringLiteral("; ")))}
                   .join(u',');
        csv += QStringLiteral("\r\n");
    }
    return writeFileAtomic(path, QByteArray("\xEF\xBB\xBF") + csv.toUtf8()); // BOM: Excel reads UTF-8
}

void JobHistory::compactIfNeeded()
{
    // Called with m_mutex held.
    QFile f(m_path);
    if (f.size() < 2 * 1024 * 1024)
        return;
    if (!f.open(QIODevice::ReadOnly))
        return;
    QList<QByteArray> lines = f.readAll().split('\n');
    f.close();
    lines.removeAll(QByteArray());
    if (lines.size() <= m_max)
        return;
    lines = lines.mid(lines.size() - m_max);
    (void)writeFileAtomic(m_path, lines.join('\n') + '\n');
}

} // namespace fw
