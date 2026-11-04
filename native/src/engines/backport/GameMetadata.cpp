#include "engines/backport/GameMetadata.h"

#include "core/FileUtil.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QtEndian>

namespace fw::backport {

QString GameMetadata::requiredFwText() const
{
    if (!requiredFwMajor)
        return {};
    return QStringLiteral("%1.%2").arg(*requiredFwMajor).arg(requiredFwMinor.value_or(0), 2, 10, QLatin1Char('0'));
}

std::optional<std::pair<int, int>> parsePs5VersionField(const QVariant& raw)
{
    if (!raw.isValid() || raw.isNull())
        return std::nullopt;
    const QString s = raw.toString().trimmed();
    if (s.isEmpty())
        return std::nullopt;
    if (s.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive)) {
        QString digits = s.mid(2);
        digits.remove(u'_');
        digits.remove(u'-');
        digits.remove(u' ');
        if (digits.size() < 2)
            return std::nullopt;
        const QString majorStr = digits.left(2);
        const QString minorStr = digits.size() >= 4 ? digits.mid(2, 2) : QStringLiteral("00");
        static const QRegularExpression dec(QStringLiteral("^[0-9]+$"));
        if (!dec.match(majorStr).hasMatch() || !dec.match(minorStr).hasMatch()) {
            bool ok = false;
            const quint64 v = digits.toULongLong(&ok, 16);
            if (!ok)
                return std::nullopt;
            const int nbits = int(digits.size()) * 4;
            const int major = nbits >= 16 ? int((v >> (nbits - 8)) & 0xFF) : int(v & 0xFF);
            return std::make_pair(major, 0);
        }
        return std::make_pair(majorStr.toInt(), minorStr.toInt());
    }
    bool ok = false;
    const int n = s.toInt(&ok, 10);
    if (ok && n >= 0 && n <= 99)
        return std::make_pair(n, 0);
    return std::nullopt;
}

namespace {

QString firstString(const QJsonObject& o, std::initializer_list<const char*> keys)
{
    for (const char* k : keys) {
        const QJsonValue v = o.value(QLatin1String(k));
        if (v.isString() && !v.toString().trimmed().isEmpty())
            return v.toString().trimmed();
    }
    return {};
}

bool readParamJson(const QString& path, GameMetadata& m)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QByteArray data = f.read(4 * 1024 * 1024);
    if (data.startsWith("\xEF\xBB\xBF"))
        data.remove(0, 3);
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject())
        return false;
    const QJsonObject o = doc.object();
    const QString id = firstString(o, {"titleId", "title_id", "TITLE_ID"}).toUpper();
    if (id.isEmpty())
        return false;
    m.titleId = id;
    const QJsonObject loc = o.value(QStringLiteral("localizedParameters")).toObject();
    QString title = loc.value(QStringLiteral("en-US")).toObject().value(QStringLiteral("titleName")).toString();
    if (title.isEmpty()) {
        const QString def = loc.value(QStringLiteral("defaultLanguage")).toString();
        if (!def.isEmpty())
            title = loc.value(def).toObject().value(QStringLiteral("titleName")).toString();
    }
    if (title.isEmpty())
        title = firstString(o, {"titleName", "title", "TITLE"});
    m.title = title.trimmed();
    m.version = firstString(o, {"contentVersion", "appVersion", "APP_VER"});
    for (const char* k : {"sdkVersion", "sdk_version", "SDK_VER"}) {
        const auto v = parsePs5VersionField(o.value(QLatin1String(k)).toVariant());
        if (v && v->first >= 1 && v->first <= 30) {
            m.paramSdk = v->first;
            break;
        }
    }
    for (const char* k : {"requiredSystemSoftwareVersion", "systemVersion", "SYSTEM_VER"}) {
        const auto v = parsePs5VersionField(o.value(QLatin1String(k)).toVariant());
        if (v && v->first >= 1 && v->first <= 30) {
            m.requiredFwMajor = v->first;
            m.requiredFwMinor = v->second;
            break;
        }
    }
    m.source = path;
    return true;
}

bool readParamSfo(const QString& path, GameMetadata& m)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray d = f.read(1024 * 1024);
    if (d.size() < 20 || d.mid(1, 3) != "PSF")
        return false;
    const auto u16 = [&](qsizetype o) { return qFromLittleEndian<quint16>(d.constData() + o); };
    const auto u32 = [&](qsizetype o) { return qFromLittleEndian<quint32>(d.constData() + o); };
    const quint32 keyTable = u32(8), dataTable = u32(12), count = u32(16);
    if (count > 200 || keyTable > quint32(d.size()) || dataTable > quint32(d.size()))
        return false;
    QString id, title, ver;
    for (quint32 i = 0; i < count; ++i) {
        const qsizetype base = 20 + qsizetype(i) * 16;
        if (base + 16 > d.size())
            break;
        const quint32 keyStart = keyTable + u16(base);
        const quint32 maxLen = u32(base + 8);
        const quint32 valStart = dataTable + u32(base + 12);
        if (keyStart >= quint32(d.size()))
            continue;
        const QByteArray key = d.mid(keyStart, 64).split('\0').front();
        if (maxLen == 0 || quint64(valStart) + maxLen > quint64(d.size()))
            continue;
        QByteArray val = d.mid(valStart, maxLen);
        while (val.endsWith('\0'))
            val.chop(1);
        if (key == "TITLE_ID")
            id = QString::fromLatin1(val).trimmed();
        else if ((key == "TITLE" || key == "TITLE_00" || key == "TITLE_01") && title.isEmpty())
            title = QString::fromUtf8(val).trimmed();
        else if (key == "APP_VER" || (key == "VERSION" && ver.isEmpty()))
            ver = QString::fromLatin1(val).trimmed();
    }
    if (id.isEmpty())
        return false;
    m.titleId = id.toUpper();
    m.title = title;
    m.version = ver;
    m.source = path;
    return true;
}

} // namespace

GameMetadata readGameMetadata(const QString& game)
{
    GameMetadata m;
    const QString root = QDir::cleanPath(QFileInfo(game).absoluteFilePath());
    const QString parent = QFileInfo(root).absolutePath();
    const QStringList dirs = {QDir(root).filePath(QStringLiteral("sce_sys")), root,
                              QDir(parent).filePath(QStringLiteral("sce_sys")), parent};
    for (const QString& d : dirs) {
        const QString p = QDir(d).filePath(QStringLiteral("param.json"));
        if (QFileInfo(p).isFile() && readParamJson(p, m))
            return m;
    }
    for (const QString& d : dirs) {
        const QString p = QDir(d).filePath(QStringLiteral("param.sfo"));
        if (QFileInfo(p).isFile() && readParamSfo(p, m))
            return m;
    }
    return m;
}

QString zipPrefix(const GameMetadata& meta, const QString& folderName)
{
    static const QRegularExpression badTitle(QStringLiteral("[\\\\/:*?\"<>|™®©]"));
    static const QRegularExpression idRe(QStringLiteral("((?:PPSA|CUSA|PPSB)\\d+)"),
                                         QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression verRe(QStringLiteral("(\\d{2}\\.\\d{3}\\.\\d{3})"));

    QString ppsa = meta.titleId.toUpper();
    QString title = meta.title.trimmed();
    QString version = meta.version.trimmed();
    title.remove(badTitle);
    title = title.trimmed();

    if (ppsa.isEmpty()) {
        const auto m = idRe.match(folderName);
        if (m.hasMatch())
            ppsa = m.captured(1).toUpper();
    }
    if (version.isEmpty()) {
        const auto m = verRe.match(folderName);
        if (m.hasMatch())
            version = m.captured(1);
    }
    if (title.isEmpty() && !folderName.isEmpty()) {
        QString t = folderName;
        t.remove(QRegularExpression(QStringLiteral("(?:PPSA|CUSA|PPSB)\\d+"), QRegularExpression::CaseInsensitiveOption));
        t.remove(QRegularExpression(QStringLiteral("\\([\\d.]+\\)")));
        t.remove(QRegularExpression(QStringLiteral("\\[[^\\]]*\\]")));
        t.remove(QRegularExpression(QStringLiteral("\\([^)]*\\)")));
        t.remove(QRegularExpression(QStringLiteral("\\b\\d+xx\\b")));
        t.remove(QRegularExpression(QStringLiteral("\\b\\d{2}\\.\\d{3}\\.\\d{3}\\b")));
        t.remove(QRegularExpression(QStringLiteral("[-_]+app\\d*$"), QRegularExpression::CaseInsensitiveOption));
        t.remove(QRegularExpression(QStringLiteral("\\.part\\d+$"), QRegularExpression::CaseInsensitiveOption));
        t.remove(QRegularExpression(QStringLiteral("\\s+\\d+$")));
        t.replace(QRegularExpression(QStringLiteral("[\\[\\](){}\\-]+")), QStringLiteral(" "));
        t = t.simplified();
        // strip('.-_ ')
        while (!t.isEmpty() && QStringLiteral(".-_ ").contains(t.front()))
            t.remove(0, 1);
        while (!t.isEmpty() && QStringLiteral(".-_ ").contains(t.back()))
            t.chop(1);
        title = t;
    }
    if (!ppsa.isEmpty() && !title.isEmpty() && !version.isEmpty())
        return QStringLiteral("%1 %2 (%3)").arg(ppsa, title, version);
    if (!ppsa.isEmpty() && !title.isEmpty())
        return QStringLiteral("%1 %2").arg(ppsa, title);
    if (!ppsa.isEmpty())
        return ppsa;
    return folderName.isEmpty() ? QStringLiteral("Unknown Game") : folderName;
}

QString manifestTitleId(const GameMetadata& meta, const QString& game)
{
    QString tid = meta.titleId.trimmed();
    if (tid.isEmpty())
        tid = QFileInfo(QDir::cleanPath(game)).fileName();
    QString out;
    for (const QChar c : tid)
        if (c.isLetterOrNumber() || c == u'-' || c == u'_')
            out.append(c);
    return out.isEmpty() ? QStringLiteral("UNKNOWN") : out;
}

} // namespace fw::backport
