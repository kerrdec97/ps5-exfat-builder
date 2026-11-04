#include "app/services/GameInfo.h"

#include "core/FileUtil.h"

#include <QDir>
#include <QDirIterator>
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
        parts << QStringLiteral("%1 files").arg(formatCount(files));
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

static QString hexVersionToDotted(const QJsonValue& v)
{
    // "0x0900000000000000" → "9.00"; numbers are accepted too.
    QString s = v.isString() ? v.toString() : (v.isDouble() ? QString::number(qint64(v.toDouble())) : QString());
    if (s.isEmpty())
        return {};
    bool ok = false;
    const quint64 n = s.startsWith(QLatin1String("0x"), Qt::CaseInsensitive) ? s.mid(2).toULongLong(&ok, 16) : s.toULongLong(&ok);
    if (!ok)
        return {};
    const int major = int((n >> 56) & 0xFF);
    const int minor = int((n >> 48) & 0xFF);
    return QStringLiteral("%1.%2").arg(major).arg(minor, 2, 16, QLatin1Char('0')).toUpper().replace(QLatin1Char('X'), QLatin1Char('x'));
}

GameSummary readGameSummary(const QString& folder, bool countFiles)
{
    GameSummary s;
    s.folder = folder;
    const QFileInfo fi(folder);
    if (folder.isEmpty() || !fi.exists()) {
        s.error = QStringLiteral("The folder does not exist.");
        return s;
    }
    s.exists = true;
    if (!fi.isDir()) {
        s.error = QStringLiteral("This is a file, not a game folder.");
        return s;
    }
    const QString param = QDir(folder).filePath(QStringLiteral("sce_sys/param.json"));
    const bool hasEboot = QFile::exists(QDir(folder).filePath(QStringLiteral("eboot.bin")));
    s.isGame = QFile::exists(param) || hasEboot;
    if (QFile::exists(param)) {
        QFile f(param);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
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
            s.sdk = hexVersionToDotted(o.value(QStringLiteral("sdkVersion")));
            s.requiredFw = hexVersionToDotted(o.value(QStringLiteral("requiredSystemSoftwareVersion")));
        }
    }
    if (s.titleId.isEmpty()) {
        static const QRegularExpression re(QStringLiteral("(CUSA|PPSA|PPLH)[-_ ]?(\\d{5})"), QRegularExpression::CaseInsensitiveOption);
        const auto m = re.match(fi.fileName());
        if (m.hasMatch())
            s.titleId = (m.captured(1) + m.captured(2)).toUpper();
    }
    if (s.title.isEmpty())
        s.title = fi.fileName();
    if (countFiles) {
        QDirIterator it(folder, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            s.bytes += it.fileInfo().size();
            ++s.files;
            if (s.files > 2'000'000)
                break;
        }
    }
    return s;
}

QString defaultImageStem(const GameSummary& s)
{
    QString title = s.title;
    title.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9]+")), QStringLiteral("_"));
    title = title.trimmed();
    while (title.startsWith(u'_'))
        title.remove(0, 1);
    while (title.endsWith(u'_'))
        title.chop(1);
    QStringList parts;
    if (!s.titleId.isEmpty())
        parts << s.titleId;
    if (!title.isEmpty())
        parts << title;
    if (!s.version.isEmpty())
        parts << QStringLiteral("v%1").arg(s.version);
    if (parts.isEmpty())
        parts << QStringLiteral("image");
    return sanitizeFileName(parts.join(u'_'));
}

GameInfoReader::GameInfoReader(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<GameSummary>();
}

void GameInfoReader::summarize(const QString& folder, bool countFiles)
{
    const quint64 gen = ++m_generation;
    m_busy = true;
    QPointer<GameInfoReader> self(this);
    (void)QtConcurrent::run([self, folder, countFiles, gen] {
        const GameSummary s = readGameSummary(folder, countFiles);
        if (!self)
            return;
        QMetaObject::invokeMethod(self.data(), [self, s, gen] {
            if (!self || gen != self->m_generation)
                return;
            self->m_busy = false;
            emit self->ready(s);
        }, Qt::QueuedConnection);
    });
}

} // namespace fw::ui
