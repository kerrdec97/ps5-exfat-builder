#pragma once

// Structural check of a built PS5 package. Reads only the outer header and
// needs no keys:
//  * at least 4096 bytes, magic 7F 'F' 'I' 'H' at offset 0;
//  * byte 5 (signed flag) is 0: a debug package is not signed;
//  * the outer superblock offset (u64 LE at 0x20) + 896 lies inside the file;
//  * the superblock mode (u16 LE at +0x1C) is 13;
//  * a plaintext debug package carries "PPRPLAIN-NOAUTH!" at +0x370.
// A package without the marker was made by a Publishing Tools build that does
// not produce installable debug packages.

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace fw::fpkg {

inline const QByteArray kFihMagic = QByteArray("\x7F" "FIH", 4);
inline const QByteArray kPlaintextMarker = QByteArray("PPRPLAIN-NOAUTH!");
inline constexpr int kSuperblockMode = 13;

struct HeaderCheck {
    bool ok = false;
    bool hasFih = false;
    bool plaintext = false;
    qint64 size = 0;
    quint64 superblockOffset = 0;
    int mode = -1;
    QStringList problems;
    QString summary() const; // problems joined, or "header OK"
};

HeaderCheck checkPackageHeader(const QString& path);

} // namespace fw::fpkg
