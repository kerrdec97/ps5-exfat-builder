#include "engines/dlc/DlcIdentity.h"

#include <QRegularExpression>

namespace fw::dlc {

bool isTitleId(const QString& t)
{
    static const QRegularExpression re(QStringLiteral("\\A(?:PPSA|CUSA)[0-9]{5}\\z"));
    return re.match(t).hasMatch();
}

bool isContentId(const QString& c)
{
    static const QRegularExpression re(QStringLiteral("\\A[A-Z]{2}[0-9]{4}-(?:PPSA|CUSA)[0-9]{5}_[0-9]{2}-[A-Z0-9_]{16}\\z"));
    return re.match(c).hasMatch();
}

bool isPublisherPrefix(const QString& p)
{
    static const QRegularExpression re(QStringLiteral("\\A[A-Z]{2}[0-9]{4}\\z"));
    return re.match(p).hasMatch();
}

QString titleIdOf(const QString& c) { return isContentId(c) ? c.mid(7, 9) : QString(); }
QString suffixOf(const QString& c) { return isContentId(c) ? c.mid(20, 16) : QString(); }
bool isPs5TitleId(const QString& t) { return t.startsWith(QStringLiteral("PPSA")); }

Status validateIdentity(const QString& titleId, const QString& contentId)
{
    if (!isTitleId(titleId))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Title ID must be PPSA or CUSA followed by exactly five digits."));
    if (!isContentId(contentId))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("Content ID must use XX0000-PPSA00000_00-XXXXXXXXXXXXXXXX (or CUSA), with a "
                                    "16-character uppercase alphanumeric/underscore suffix."));
    if (contentId.mid(7, 9) != titleId)
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("Title ID must match the game identifier embedded in the Content ID."));
    return success();
}

QString regionCode(const QString& c) { return isContentId(c) ? c.left(2) : QString(); }

QString regionText(const QString& c)
{
    if (!isContentId(c))
        return QStringLiteral("Unknown");
    const QString p = c.left(2);
    if (p == u"UP") return QStringLiteral("Americas (inferred from UP)");
    if (p == u"EP") return QStringLiteral("Europe (inferred from EP)");
    if (p == u"JP") return QStringLiteral("Japan (inferred from JP)");
    if (p == u"HP") return QStringLiteral("Asia (inferred from HP)");
    if (p == u"KP") return QStringLiteral("Korea (inferred from KP)");
    return QStringLiteral("Unknown (prefix %1)").arg(p);
}

Result<QString> retargetContentId(const QString& original, const QString& targetTitleId, const QString& prefix)
{
    if (!isContentId(original))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("The original Content ID is malformed; enter a complete target Content ID manually."));
    if (!isTitleId(targetTitleId))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("The target Title ID is malformed."));
    if (original.mid(7, 4) != targetTitleId.left(4))
        return Error(ErrorCode::InvalidArgument,
                     QStringLiteral("A PS4 identifier cannot be converted into a PS5 identifier, or vice versa."));
    const QString p = prefix.isEmpty() ? original.left(6) : prefix;
    if (!isPublisherPrefix(p))
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Publisher prefix must contain two uppercase letters and four digits."));
    return QStringLiteral("%1-%2%3").arg(p, targetTitleId, original.mid(16));
}

} // namespace fw::dlc
