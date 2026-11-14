#pragma once

// An archive used as a build source (GitHub #67, #37): many people keep their
// dumps packed. The image build jobs accept "<dump>.zip", "<dump>.7z" or
// "<dump>.rar" as `source`; they check it here first (validate), then unpack
// it into the job's private temporary folder (run) and build from the game
// inside exactly as from a folder. Every entry's checksum is verified while it
// is unpacked, names that would leave the folder are refused, and the
// temporary copy is removed with the job's temp folder.
//
// ZIP archives are read by the app's own reader (zip::Reader); 7z and RAR
// archives by libarchive (ArchiveReader.h, which lists what is not read:
// password-protected archives, solid RAR 4, RAR 5 dictionaries over 64 MiB).
// A multi-part RAR is given by its first part. The file is told apart by its
// first bytes, so a 7z named ".zip" is still read.
//
// The game inside is the shortest folder that holds eboot.bin or
// sce_sys/param.json ("" for an archive of the game folder's contents, or
// "PPSA01234-app0/" for an archive of the folder itself). An archive with
// several games is refused with their names.
//
// The functions named "...Zip..." came first and are kept for their callers;
// each takes all three formats and has an "...Archive..." twin.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace fw::archive {

// An existing file named *.zip, *.7z or *.rar.
bool isArchiveSource(const QString& path);
bool isZipSource(const QString& path);

// "zip", "7z", "rar": the suffixes isArchiveSource() accepts.
QStringList archiveSourceSuffixes();
// For file dialogs: "Archives (*.zip *.7z *.rar)".
QString archiveSourceFilter();

// The name a build takes from its source: a folder's whole name
// ("Astro Bot v1.020"), an archive's name without its suffix, and without the
// part number of a multi-part RAR ("Game.part1.rar" -> "Game").
QString sourceName(const QString& path);

struct ZipGame {
    QString rootPrefix;      // "" or "Folder/" (with the trailing slash)
    qint64 bytes = 0;        // uncompressed size of the game's files
    qint64 files = 0;
    bool hasEboot = false;
    bool hasParam = false;
    QByteArray paramJson;    // sce_sys/param.json, when present (< 16 MiB) and cheap to read
    QStringList otherGames;  // further game folders found in the archive (the first 10)
    qint64 moreGames = 0;    // further game folders beyond those otherGames names
    qint64 links = 0;        // 7z, RAR: symbolic and hard links in the game; they are not unpacked
};
using ArchiveGame = ZipGame;

// Reads the list of files (and param.json); nothing is unpacked.
//
// In a 7z or RAR archive param.json may lie behind other data that has to be
// decoded first. It is read only while that stays cheap (256 MiB of the
// archive, five seconds); past that the game is returned without paramJson,
// as for an archive that has none. The list of a 7z or RAR archive is kept
// until the file changes, so asking again costs nothing.
Result<ArchiveGame> inspectArchive(const QString& path, const CancellationToken& token = {});
Result<ZipGame> inspectZip(const QString& zipPath);

// validate(): the archive is readable and holds exactly one game. Does not
// read param.json.
Status checkArchiveSource(const QString& path, const CancellationToken& token = {});
Status checkZipSource(const QString& zipPath);

// run(): unpacks the game (and nothing else the archive holds) into
// <tempDir>/zip-source and returns the game folder. Checks free space on the
// temp drive first, keeping `freeSpaceMarginBytes` in reserve (jobs pass
// storage.freeSpaceMarginMiB).
Result<QString> unpackArchiveSource(const QString& path, const QString& tempDir, const CancellationToken& token,
                                    ProgressReporter* progress = nullptr,
                                    qint64 freeSpaceMarginBytes = 256LL * 1024 * 1024);
Result<QString> unpackZipSource(const QString& zipPath, const QString& tempDir, const CancellationToken& token,
                                ProgressReporter* progress = nullptr,
                                qint64 freeSpaceMarginBytes = 256LL * 1024 * 1024);

// Tests only: what reading param.json out of a 7z or RAR archive may cost
// (-1: no limit), and forget every list read so far.
void setArchiveInspectBudgetForTesting(qint64 archiveBytes, int milliseconds);

} // namespace fw::archive
