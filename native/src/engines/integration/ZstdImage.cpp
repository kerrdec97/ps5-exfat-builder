#include "engines/integration/ZstdImage.h"

#include "core/FileUtil.h"
#include "core/Tr.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace fw::integration {

bool isZstdImageName(const QString& path)
{
    return QFileInfo(path).suffix().compare(QLatin1String("zstd"), Qt::CaseInsensitive) == 0;
}

ZstdImageKind sniffZstdImage(const QByteArray& head)
{
    const auto u = [&](int i) { return quint8(head.at(i)); };
    // A zstd frame, or a skippable frame (magic 0x184D2A50..5F, little endian),
    // which a seekable zstd file may begin with.
    if (head.size() >= 4 && u(0) == 0x28 && u(1) == 0xB5 && u(2) == 0x2F && u(3) == 0xFD)
        return ZstdImageKind::ZstdStream;
    if (head.size() >= 4 && (u(0) & 0xF0) == 0x50 && u(1) == 0x2A && u(2) == 0x4D && u(3) == 0x18)
        return ZstdImageKind::ZstdStream;
    // The exFAT boot sector's file system name (Microsoft exFAT specification
    // 3.1.2), the one thing Kraken Mount MOD checks before it attaches a .zstd.
    if (head.size() >= 11 && head.mid(3, 8) == QByteArrayLiteral("EXFAT   "))
        return ZstdImageKind::Exfat;
    return ZstdImageKind::Other;
}

Result<ZstdImageKind> sniffZstdImageFile(const QString& path)
{
    QFile f(toLongPath(path));
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QDir::toNativeSeparators(path)), f.errorString());
    return sniffZstdImage(f.read(512));
}

Status checkZstdImage(const QString& path)
{
    if (!QFileInfo(path).isFile())
        return Error(ErrorCode::NotFound, QStringLiteral("%1 does not exist.").arg(QDir::toNativeSeparators(path)));
    FW_TRY_ASSIGN(const ZstdImageKind kind, sniffZstdImageFile(path));
    const QString name = QFileInfo(path).fileName();
    switch (kind) {
    case ZstdImageKind::Exfat:
        return success();
    case ZstdImageKind::ZstdStream:
        return Error(ErrorCode::Unsupported, FW_TR("%1 is a zstd-compressed stream, not a mountable image. Decompress it first.").arg(name),
                     QStringLiteral("zstd frame magic at byte 0"));
    case ZstdImageKind::Other:
        break;
    }
    return Error(ErrorCode::Unsupported, FW_TR("%1 is not an exFAT image. Only .zstd files that hold an exFAT image can be opened.").arg(name),
                 QStringLiteral("no exFAT boot sector (\"EXFAT   \" at bytes 3 to 10)"));
}

QString imageFormatByName(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == u"exfat" || ext == u"zstd")
        return QStringLiteral("exfat");
    if (ext == u"ffpkg" || ext == u"ffpfs" || ext == u"ffpfsc")
        return ext;
    return {};
}

} // namespace fw::integration
