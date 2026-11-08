#pragma once

// A ZIP archive used as a build source (GitHub #67, #37): many people keep
// their dumps zipped. The image build jobs accept "<dump>.zip" as `source`;
// they check it here first (validate), then unpack it into the job's private
// temporary folder (run) and build from the game inside exactly as from a
// folder. Every entry is CRC-checked while it is unpacked, names that would
// leave the folder are refused (zip::Reader), and the temporary copy is
// removed with the job's temp folder.
//
// The game inside is the shortest folder that holds eboot.bin or
// sce_sys/param.json ("" for a ZIP of the game folder's contents, or
// "PPSA01234-app0/" for a ZIP of the folder itself). A ZIP with several games
// is refused with their names.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace fw::archive {

bool isZipSource(const QString& path); // an existing file named *.zip

struct ZipGame {
    QString rootPrefix;      // "" or "Folder/" (with the trailing slash)
    qint64 bytes = 0;        // uncompressed size of the game's files
    qint64 files = 0;
    bool hasEboot = false;
    bool hasParam = false;
    QByteArray paramJson;    // sce_sys/param.json, when present (< 16 MiB)
    QStringList otherGames;  // further game folders found in the archive
};

// Reads the central directory (and param.json); nothing is unpacked.
Result<ZipGame> inspectZip(const QString& zipPath);

// validate(): the archive is readable and holds exactly one game.
Status checkZipSource(const QString& zipPath);

// run(): unpacks the game into <tempDir>/zip-source and returns the game
// folder. Checks free space on the temp drive first.
Result<QString> unpackZipSource(const QString& zipPath, const QString& tempDir, const CancellationToken& token,
                                ProgressReporter* progress = nullptr);

} // namespace fw::archive
