#include "engines/fpkg/PackageHeader.h"

#include "core/FileUtil.h"

#include <QFile>
#include <QtEndian>

namespace fw::fpkg {

QString HeaderCheck::summary() const
{
    return problems.isEmpty() ? QStringLiteral("header OK (unsigned debug package with the plaintext marker)")
                              : problems.join(QStringLiteral("; "));
}

HeaderCheck checkPackageHeader(const QString& path)
{
    HeaderCheck c;
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly)) {
        c.problems << QStringLiteral("the package could not be opened (%1)").arg(f.errorString());
        return c;
    }
    c.size = f.size();
    const QByteArray head = f.read(0x60);
    if (c.size < 4096) {
        c.problems << QStringLiteral("the file is only %1 bytes").arg(c.size);
        return c;
    }
    if (!head.startsWith(kFihMagic)) {
        c.problems << QStringLiteral("this is not a PS5 package (no FIH header)");
        return c;
    }
    c.hasFih = true;
    if (head[5] != 0)
        c.problems << QStringLiteral("the package is signed (byte 5 = %1), so it is not a debug package").arg(int(quint8(head[5])));
    c.superblockOffset = qFromLittleEndian<quint64>(head.constData() + 0x20);
    if (c.superblockOffset > quint64(c.size) - 896) { // size >= 4096 here; written so the sum cannot wrap
        c.problems << QStringLiteral("the superblock offset 0x%1 lies outside the file").arg(c.superblockOffset, 0, 16);
        return c;
    }
    if (!f.seek(qint64(c.superblockOffset))) {
        c.problems << QStringLiteral("the superblock could not be read");
        return c;
    }
    const QByteArray block = f.read(0x380);
    if (block.size() < 0x380) {
        c.problems << QStringLiteral("the superblock could not be read");
        return c;
    }
    c.mode = qFromLittleEndian<quint16>(block.constData() + 0x1C);
    if (c.mode != kSuperblockMode)
        c.problems << QStringLiteral("unexpected superblock mode %1 (expected %2)").arg(c.mode).arg(kSuperblockMode);
    c.plaintext = block.mid(0x370, 16) == kPlaintextMarker;
    if (!c.plaintext)
        c.problems << QStringLiteral("the plaintext debug marker is missing: these Publishing Tools do not make "
                                     "installable debug packages");
    c.ok = c.problems.isEmpty();
    return c;
}

} // namespace fw::fpkg
