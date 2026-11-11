#include "app/services/GameInfo.h"

#include "core/ArchiveSource.h"

#include "core/FileUtil.h"
#include "core/Tr.h"
#include "engines/gameinfo/GameInfo.h"
#include "engines/gameinfo/Version.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>

namespace fw::ui {

QString GameSummary::metaLine() const
{
    QStringList parts;
    if (!titleId.isEmpty())
        parts << titleId;
    if (!version.isEmpty())
        parts << QStringLiteral("v%1").arg(version);
    if (bytes > 0)
        parts << formatBytes(bytes);
    if (files > 0)
        parts << (files == 1 ? FW_TR("1 file") : FW_TR("%1 files").arg(formatCount(files)));
    if (!sdk.isEmpty())
        parts << QStringLiteral("SDK %1").arg(sdk);
    return parts.join(QStringLiteral(" · "));
}

QJsonObject GameSummary::toJson() const
{
    return {{QStringLiteral("folder"), folder}, {QStringLiteral("title"), title}, {QStringLiteral("titleId"), titleId},
            {QStringLiteral("contentId"), contentId}, {QStringLiteral("version"), version}, {QStringLiteral("sdk"), sdk},
            {QStringLiteral("requiredFw"), requiredFw}, {QStringLiteral("sizeBytes"), bytes}, {QStringLiteral("files"), files},
            {QStringLiteral("isGame"), isGame}, {QStringLiteral("platform"), platform}, {QStringLiteral("kind"), kind},
            {QStringLiteral("coverPath"), coverPath}};
}

GameSummary GameSummary::fromJson(const QJsonObject& o)
{
    GameSummary s;
    s.folder = QDir::fromNativeSeparators(o.value(QStringLiteral("folder")).toString());
    s.title = o.value(QStringLiteral("title")).toString();
    s.titleId = o.value(QStringLiteral("titleId")).toString();
    s.contentId = o.value(QStringLiteral("contentId")).toString();
    s.version = o.value(QStringLiteral("version")).toString();
    // Accepts the UI's own summary, backport.scan games and library.scan games.
    s.sdk = o.value(QStringLiteral("sdk")).toVariant().toString();
    if (s.sdk.isEmpty())
        s.sdk = o.value(QStringLiteral("sdkVersion")).toString();
    s.requiredFw = o.value(QStringLiteral("requiredFw")).toString();
    if (s.requiredFw.isEmpty())
        s.requiredFw = o.value(QStringLiteral("requiredSystemVersion")).toString();
    s.platform = o.value(QStringLiteral("platform")).toString();
    s.kind = o.value(QStringLiteral("kind")).toString();
    s.coverPath = o.value(QStringLiteral("coverPath")).toString();
    s.bytes = qint64(o.value(QStringLiteral("sizeBytes")).toDouble());
    s.files = qint64(o.value(QStringLiteral("files")).toDouble());
    s.isGame = o.value(QStringLiteral("isGame")).toBool(true);
    s.exists = true;
    return s;
}

// The fields the app shows, from sce_sys/param.json.
static void fillFromParamJson(GameSummary& s, const QByteArray& json)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    s.titleId = o.value(QStringLiteral("titleId")).toString();
    s.contentId = o.value(QStringLiteral("contentId")).toString();
    s.version = o.value(QStringLiteral("contentVersion")).toString();
    const QJsonObject loc = o.value(QStringLiteral("localizedParameters")).toObject();
    const QString defLang = loc.value(QStringLiteral("defaultLanguage")).toString(QStringLiteral("en-US"));
    s.title = loc.value(defLang).toObject().value(QStringLiteral("titleName")).toString();
    if (s.title.isEmpty())
        s.title = loc.value(QStringLiteral("en-US")).toObject().value(QStringLiteral("titleName")).toString();
    if (s.title.isEmpty())
        s.title = o.value(QStringLiteral("titleName")).toString();
    // Packed BCD ("0x1270000000000000" is 12.70), read like the legacy app.
    if (const auto v = gameinfo::parseVersionField(o.value(QStringLiteral("sdkVersion"))); v.isValid())
        s.sdk = v.toString();
    for (const char* key : {"requiredSystemSoftwareVersion", "systemVersion"})
        if (const auto v = gameinfo::parseVersionField(o.value(QLatin1String(key))); v.isValid()) {
            s.requiredFw = v.toString();
            break;
        }
}

GameSummary readGameSummary(const QString& folder, bool countFiles, const CancellationToken& token,
                            const std::function<void(qint64, qint64)>& onProgress, bool findRoots)
{
    GameSummary s;
    s.folder = folder;
    const QFileInfo fi(folder);
    if (folder.isEmpty() || !fi.exists()) {
        s.error = FW_TR("The folder does not exist.");
        return s;
    }
    s.exists = true;
    if (archive::isZipSource(folder)) {
        // An archived dump (#67): read it without unpacking.
        auto g = archive::inspectZip(folder);
        if (!g.ok()) {
            s.error = g.error().message;
            return s;
        }
        s.isGame = g->hasEboot || g->hasParam;
        s.bytes = g->bytes;
        s.files = g->files;
        if (!g->paramJson.isEmpty())
            fillFromParamJson(s, g->paramJson);
        if (!g->otherGames.isEmpty())
            s.error = FW_TR("This archive holds more than one game.");
        if (s.titleId.isEmpty()) {
            static const QRegularExpression re(QStringLiteral("(CUSA|PPSA|PPLH)[-_ ]?(\\d{5})"), QRegularExpression::CaseInsensitiveOption);
            if (const auto m = re.match(fi.fileName()); m.hasMatch())
                s.titleId = (m.captured(1) + m.captured(2)).toUpper();
        }
        if (s.title.isEmpty())
            s.title = fi.completeBaseName();
        return s;
    }
    if (!fi.isDir()) {
        s.error = FW_TR("This is a file, not a game folder.");
        return s;
    }
    const QString param = QDir(folder).filePath(QStringLiteral("sce_sys/param.json"));
    const bool hasEboot = QFile::exists(QDir(folder).filePath(QStringLiteral("eboot.bin")));
    s.isGame = QFile::exists(param) || hasEboot;
    if (QFile::exists(param)) {
        QFile f(param);
        if (f.size() < 16 * 1024 * 1024 && f.open(QIODevice::ReadOnly))
            fillFromParamJson(s, f.readAll());
    }
    if (s.titleId.isEmpty()) {
        static const QRegularExpression re(QStringLiteral("(CUSA|PPSA|PPLH)[-_ ]?(\\d{5})"), QRegularExpression::CaseInsensitiveOption);
        if (const auto m = re.match(fi.fileName()); m.hasMatch())
            s.titleId = (m.captured(1) + m.captured(2)).toUpper();
    }
    if (s.title.isEmpty())
        s.title = fi.fileName();
    if (!s.isGame && findRoots) {
        // A wrapper folder: look for game folders below it instead of totalling
        // what may be a whole drive (#84, #53).
        const gameinfo::RootSearch r = gameinfo::findGameRoots(folder, token, 5, 4000);
        s.gameRoots = r.candidates;
        s.rootsTruncated = r.truncated;
        s.cancelled = r.cancelled;
        return s;
    }
    if (countFiles) {
        QElapsedTimer clock;
        clock.start();
        qint64 lastReport = 0;
        QDirIterator it(folder, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            if ((s.files & 0xFF) == 0 && token.isCancelled()) {
                s.cancelled = true;
                break;
            }
            it.next();
            s.bytes += it.fileInfo().size();
            ++s.files;
            if (onProgress && clock.elapsed() - lastReport >= 250) {
                lastReport = clock.elapsed();
                onProgress(s.files, s.bytes);
            }
            if (s.files > 2'000'000)
                break;
        }
    }
    return s;
}

GameInfoReader::GameInfoReader(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<GameSummary>();
}

GameInfoReader::~GameInfoReader()
{
    if (m_cancel)
        m_cancel->cancel();
}

void GameInfoReader::cancel()
{
    if (m_cancel)
        m_cancel->cancel();
    ++m_generation;
    m_busy = false;
}

void GameInfoReader::summarize(const QString& folder, bool countFiles)
{
    if (m_cancel)
        m_cancel->cancel(); // the previous walk stops at its next file
    m_cancel = std::make_shared<CancellationSource>();
    const CancellationToken token = m_cancel->token();
    const quint64 gen = ++m_generation;
    m_busy = true;
    const bool findRoots = m_findRoots;
    QPointer<GameInfoReader> self(this);
    (void)QtConcurrent::run([self, folder, countFiles, gen, token, findRoots] {
        const auto progress = [self, gen](qint64 files, qint64 bytes) {
            if (!self)
                return;
            QMetaObject::invokeMethod(QCoreApplication::instance(), [self, gen, files, bytes] {
                if (self && gen == self->m_generation)
                    emit self->progress(files, bytes);
            }, Qt::QueuedConnection);
        };
        const GameSummary s = readGameSummary(folder, countFiles, token, progress, findRoots);
        if (!self || token.isCancelled())
            return;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, s, gen] {
            if (!self || gen != self->m_generation)
                return;
            self->m_busy = false;
            emit self->ready(s);
        }, Qt::QueuedConnection);
    });
}

} // namespace fw::ui
