#include "engines/fpkg/ContentNaming.h"

#include <QRegularExpression>

#include <algorithm>

namespace fw::fpkg {

bool isValidContentId(const QString& id)
{
    static const QRegularExpression re(QStringLiteral("^[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_[0-9]{2}-[A-Z0-9]{16}$"));
    return re.match(id).hasMatch();
}

bool hasStandardContentIdSuffix(const QString& id)
{
    return id.size() == 36 && id.mid(16, 3) == u"_00";
}

QString titleIdOf(const QString& contentId)
{
    return contentId.mid(7, 9);
}

std::optional<ContentVersion> ContentVersion::parse(const QString& text)
{
    static const QRegularExpression re(QStringLiteral("^([0-9]{2})\\.([0-9]{3})\\.([0-9]{3})$"));
    const auto m = re.match(text.trimmed());
    if (!m.hasMatch())
        return std::nullopt;
    ContentVersion v;
    v.major = m.captured(1).toInt();
    v.minor = m.captured(2).toInt();
    v.patch = m.captured(3).toInt();
    return v;
}

QString ContentVersion::text() const
{
    return QStringLiteral("%1.%2.%3")
        .arg(major, 2, 10, QLatin1Char('0'))
        .arg(minor, 3, 10, QLatin1Char('0'))
        .arg(patch, 3, 10, QLatin1Char('0'));
}

QString ContentVersion::packageComponent() const
{
    return QStringLiteral("%1%2").arg(major, 2, 10, QLatin1Char('0')).arg(minor, 2, 10, QLatin1Char('0'));
}

ContentVersion ContentVersion::next() const
{
    ContentVersion v = *this;
    v.patch = 0;
    if (v.minor >= 999) {
        v.minor = 0;
        v.major = std::min(99, v.major + 1);
    } else {
        ++v.minor;
    }
    return v;
}

std::optional<ContentVersion> ContentVersion::nextPatch() const
{
    ContentVersion v = *this;
    if (++v.patch > 999) {
        v.patch = 0;
        if (++v.minor > 999) {
            v.minor = 0;
            ++v.major;
        }
    }
    if (v.major > 99)
        return std::nullopt;
    return v;
}

int ContentVersion::compare(const ContentVersion& o) const
{
    if (major != o.major)
        return major < o.major ? -1 : 1;
    if (minor != o.minor)
        return minor < o.minor ? -1 : 1;
    if (patch != o.patch)
        return patch < o.patch ? -1 : 1;
    return 0;
}

QString packageStem(const QString& contentId, const ContentVersion& version)
{
    const QString vv = version.packageComponent();
    return QStringLiteral("%1-A%2-V%3").arg(contentId, vv, vv);
}

QString fullPackageName(const QString& stem) { return stem + QStringLiteral(".pkg"); }
QString updatePackageName(const QString& stem) { return QStringLiteral("UPDATE_") + stem + QStringLiteral(".pkg"); }
QString companionName(const QString& stem) { return stem + QStringLiteral(".remastered.pkg"); }

QString numberedName(const QString& fileName, int n)
{
    static const QString companionExt = QStringLiteral(".remastered.pkg");
    QString base = fileName;
    QString ext;
    if (fileName.endsWith(companionExt, Qt::CaseInsensitive)) {
        ext = fileName.right(companionExt.size());
        base.chop(companionExt.size());
    } else {
        const qsizetype dot = fileName.lastIndexOf(u'.');
        if (dot > 0) {
            ext = fileName.mid(dot);
            base = fileName.left(dot);
        }
    }
    return QStringLiteral("%1 (%2)%3").arg(base).arg(n).arg(ext);
}

} // namespace fw::fpkg
