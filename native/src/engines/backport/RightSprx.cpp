#include "engines/backport/RightSprx.h"

#include "core/FileUtil.h"
#include "engines/backport/Common.h"

#include <QDir>
#include <QFileInfo>

namespace fw::backport {

bool isRightSprx(const QString& rel)
{
    return relKey(rel) == QLatin1String(kRightSprxRel);
}

QString standardRightSprxProblem(const QString& path, const QString& expectedSha256)
{
    const QString p = path.trimmed();
    const QString shown = QDir::toNativeSeparators(p);
    if (p.isEmpty())
        return QStringLiteral("no standard right.sprx is set.");
    const QFileInfo fi(p);
    if (!fi.isFile())
        return QStringLiteral("%1 is not a file.").arg(shown);
    // The standard file is 12512 bytes; anything far larger is not it.
    auto data = readFileBytes(p, 1024 * 1024);
    if (!data.ok())
        return QStringLiteral("%1 could not be read (%2).").arg(shown, data.error().message);
    if (sha256Hex(data.value()) != expectedSha256.toLower())
        return QStringLiteral("%1 is not the standard right.sprx (SHA-256 must be %2).")
            .arg(shown, expectedSha256.toLower());
    return {};
}

} // namespace fw::backport
