#include "app/services/AppStores.h"

#include "core/FileUtil.h"
#include "core/Log.h"
#include "core/Paths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

#include <algorithm>
#include <optional>

namespace fw::ui {

namespace {

// A store file as JSON. One that cannot be parsed is not lost to the next
// save: it is kept next to the store as <name>.bad, and the log says so.
std::optional<QJsonObject> readStoreFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt; // no store yet
    const QByteArray bytes = f.readAll();
    f.close();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error == QJsonParseError::NoError && doc.isObject())
        return doc.object();
    const QString bad = path + QStringLiteral(".bad");
    QFile::remove(bad);
    const bool kept = QFile::copy(path, bad);
    Log(QStringLiteral("app")).warn(QStringLiteral("%1 could not be read (%2); %3")
                                        .arg(QDir::toNativeSeparators(path), err.error == QJsonParseError::NoError ? QStringLiteral("not a JSON object") : err.errorString(),
                                             kept ? QStringLiteral("a copy was kept as %1").arg(QFileInfo(bad).fileName()) : QStringLiteral("no copy could be kept")));
    return std::nullopt;
}

bool writeStoreFile(const QString& path, const QJsonObject& root)
{
    const Status st = writeFileAtomic(path, QJsonDocument(root).toJson());
    if (!st.ok())
        Log(QStringLiteral("app")).warn(QStringLiteral("%1 could not be saved: %2").arg(QDir::toNativeSeparators(path), st.error().toLogString()));
    return st.ok();
}

// Folder paths are compared as the file system does on Windows.
bool sameFolder(const QString& a, const QString& b)
{
    return QDir::cleanPath(a).compare(QDir::cleanPath(b), Qt::CaseInsensitive) == 0;
}

} // namespace

LibraryStore::LibraryStore(QObject* parent) : QObject(parent), m_path(QDir(Paths::dataDir()).filePath(QStringLiteral("library.json")))
{
    load();
}

void LibraryStore::setScanFolders(const QStringList& folders)
{
    m_folders.clear();
    for (const QString& folder : folders) {
        const QString f = QDir::cleanPath(folder);
        const bool known = std::any_of(m_folders.cbegin(), m_folders.cend(), [&f](const QString& have) { return sameFolder(have, f); });
        if (!folder.trimmed().isEmpty() && !known)
            m_folders << f;
    }
    save();
    emit changed();
}

void LibraryStore::addScanFolder(const QString& folder)
{
    const QString f = QDir::cleanPath(folder);
    if (folder.trimmed().isEmpty() || std::any_of(m_folders.cbegin(), m_folders.cend(), [&f](const QString& have) { return sameFolder(have, f); }))
        return;
    m_folders << f;
    save();
    emit changed();
}

void LibraryStore::removeScanFolder(const QString& folder)
{
    if (m_folders.removeIf([&folder](const QString& have) { return sameFolder(have, folder); }) > 0) {
        save();
        emit changed();
    }
}

void LibraryStore::setGames(std::vector<LibraryGame> games)
{
    m_games = std::move(games);
    save();
    emit changed();
}

void LibraryStore::upsertGame(const GameSummary& info, const QString& status)
{
    for (auto& g : m_games)
        if (g.info.folder == info.folder) {
            g.info = info;
            if (!status.isEmpty())
                g.status = status;
            g.lastSeen = QDateTime::currentDateTime();
            save();
            emit changed();
            return;
        }
    m_games.push_back({info, status, QDateTime::currentDateTime()});
    save();
    emit changed();
}

void LibraryStore::setBackportInfo(const QString& folder, const QStringList& issues, bool canRun, const QString& plan)
{
    for (auto& g : m_games)
        if (g.info.folder == folder) {
            g.issues = issues;
            g.canRun = canRun;
            g.plan = plan;
            save();
            emit changed();
            return;
        }
}

void LibraryStore::removeGame(const QString& folder)
{
    const auto before = m_games.size();
    m_games.erase(std::remove_if(m_games.begin(), m_games.end(), [&](const LibraryGame& g) { return g.info.folder == folder; }), m_games.end());
    if (m_games.size() != before) {
        save();
        emit changed();
    }
}

void LibraryStore::renameGame(const QString& folder, const QString& newFolder)
{
    for (auto& g : m_games)
        if (g.info.folder == folder) {
            g.info.folder = newFolder;
            save();
            emit changed();
            return;
        }
}

void LibraryStore::setLastScan(const QDateTime& t)
{
    m_lastScan = t;
    save();
    emit changed();
}

bool LibraryStore::load()
{
    const std::optional<QJsonObject> stored = readStoreFile(m_path);
    if (!stored)
        return false;
    const QJsonObject o = *stored;
    m_folders.clear();
    for (const auto& v : o.value(QStringLiteral("folders")).toArray())
        m_folders << v.toString();
    m_games.clear();
    for (const auto& v : o.value(QStringLiteral("games")).toArray()) {
        const QJsonObject g = v.toObject();
        LibraryGame lg;
        lg.info = GameSummary::fromJson(g.value(QStringLiteral("info")).toObject());
        lg.status = g.value(QStringLiteral("status")).toString();
        lg.lastSeen = QDateTime::fromString(g.value(QStringLiteral("lastSeen")).toString(), Qt::ISODate);
        for (const auto& i : g.value(QStringLiteral("issues")).toArray())
            lg.issues << i.toString();
        lg.canRun = g.value(QStringLiteral("canRun")).toBool(true);
        lg.plan = g.value(QStringLiteral("plan")).toString();
        m_games.push_back(lg);
    }
    m_lastScan = QDateTime::fromString(o.value(QStringLiteral("lastScan")).toString(), Qt::ISODate);
    return true;
}

bool LibraryStore::save() const
{
    QJsonArray games;
    for (const auto& g : m_games)
        games.append(QJsonObject{{QStringLiteral("info"), g.info.toJson()}, {QStringLiteral("status"), g.status},
                                 {QStringLiteral("lastSeen"), g.lastSeen.toString(Qt::ISODate)},
                                 {QStringLiteral("issues"), QJsonArray::fromStringList(g.issues)}, {QStringLiteral("canRun"), g.canRun},
                                 {QStringLiteral("plan"), g.plan}});
    const QJsonObject o{{QStringLiteral("folders"), QJsonArray::fromStringList(m_folders)}, {QStringLiteral("games"), games},
                        {QStringLiteral("lastScan"), m_lastScan.toString(Qt::ISODate)}};
    return writeStoreFile(m_path, o);
}

// ---------------------------------------------------------------------------

PayloadStore::PayloadStore(QObject* parent) : QObject(parent), m_path(QDir(Paths::dataDir()).filePath(QStringLiteral("payloads.json")))
{
    load();
}

void PayloadStore::add(const PayloadEntry& e)
{
    m_entries.push_back(e);
    save();
    emit changed();
}

void PayloadStore::update(int index, const PayloadEntry& e)
{
    if (index < 0 || index >= int(m_entries.size()))
        return;
    m_entries[size_t(index)] = e;
    save();
    emit changed();
}

void PayloadStore::remove(int index)
{
    if (index < 0 || index >= int(m_entries.size()))
        return;
    m_entries.erase(m_entries.begin() + index);
    save();
    emit changed();
}

void PayloadStore::markSent(const QString& path)
{
    bool any = false;
    for (auto& e : m_entries)
        if (e.path == path) {
            e.lastSent = QDateTime::currentDateTime();
            any = true;
        }
    if (!any)
        return; // removed from the list meanwhile: nothing changed
    save();
    emit changed();
}

bool PayloadStore::load()
{
    const std::optional<QJsonObject> stored = readStoreFile(m_path);
    if (!stored)
        return false;
    m_entries.clear();
    for (const auto& v : stored->value(QStringLiteral("payloads")).toArray()) {
        const QJsonObject o = v.toObject();
        PayloadEntry e;
        e.name = o.value(QStringLiteral("name")).toString();
        e.path = o.value(QStringLiteral("path")).toString();
        e.notes = o.value(QStringLiteral("notes")).toString();
        e.lastSent = QDateTime::fromString(o.value(QStringLiteral("lastSent")).toString(), Qt::ISODate);
        m_entries.push_back(e);
    }
    return true;
}

bool PayloadStore::save() const
{
    QJsonArray arr;
    for (const auto& e : m_entries)
        arr.append(QJsonObject{{QStringLiteral("name"), e.name}, {QStringLiteral("path"), e.path}, {QStringLiteral("notes"), e.notes},
                               {QStringLiteral("lastSent"), e.lastSent.toString(Qt::ISODate)}});
    return writeStoreFile(m_path, QJsonObject{{QStringLiteral("payloads"), arr}});
}

} // namespace fw::ui
