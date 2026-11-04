#include "engines/backport/CanonicalTrio.h"

namespace fw::backport {

bool isCanonicalTrioName(const QString& fileName)
{
    for (const PinnedLibrary& p : kCanonicalTrio)
        if (fileName.compare(QLatin1String(p.name), Qt::CaseInsensitive) == 0)
            return true;
    return false;
}

const PinnedLibrary* canonicalTrioMember(const QString& fileName, qint64 size, const QString& sha256)
{
    for (const PinnedLibrary& p : kCanonicalTrio)
        if (fileName.compare(QLatin1String(p.name), Qt::CaseInsensitive) == 0 && size == p.size
            && sha256.compare(QLatin1String(p.sha256), Qt::CaseInsensitive) == 0)
            return &p;
    return nullptr;
}

} // namespace fw::backport
