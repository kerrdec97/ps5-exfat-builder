#pragma once

// Reads every directory of a PFS image one block at a time and parses each
// block from its start, as the LibProsperoPkg-based package builders do
// (UltraPack 2.7.0 report): an entry that crosses a block shows up as a
// problem there. Shared by the round-trip and golden tests.

#include "core/Cancellation.h"
#include "engines/pfs/PfsFormat.h"
#include "engines/pfs/PfsReader.h"

#include <QByteArray>
#include <QMap>
#include <QStringList>

namespace pfstest {

struct DirentBlockScan {
    QStringList problems;
    // Directory inode -> name -> how often it was found.
    QMap<quint32, QMap<QByteArray, int>> names;
};

inline DirentBlockScan scanDirentBlocks(const fw::pfs::PfsImage& img)
{
    DirentBlockScan scan;
    const qint64 bs = img.header().blockSize;
    for (const fw::pfs::Inode& ino : img.inodes()) {
        if (!ino.isDir() || ino.logicalSize() <= 0)
            continue;
        QByteArray payload;
        const fw::Status st = img.readFile(ino, [&](const char* d, qint64 n) {
            payload.append(d, n);
            return fw::success();
        }, fw::CancellationToken());
        if (!st.ok()) {
            scan.problems << st.error().toLogString();
            continue;
        }
        for (qint64 off = 0; off < payload.size(); off += bs) {
            const QString where = QStringLiteral("inode %1 block %2").arg(ino.number).arg(off / bs);
            for (const fw::pfs::ParsedDirent& d : fw::pfs::parseDirents(payload.mid(off, bs), scan.problems, where))
                ++scan.names[ino.number][d.name];
        }
    }
    return scan;
}

} // namespace pfstest
