#pragma once

// An archive used as a build source (GitHub #67, #37): many people keep their
// dumps packed. The image build jobs accept "<dump>.zip", "<dump>.7z" or
// "<dump>.rar" as `source` and check it here first (validate). Then:
//
//  * exFAT, FFPKG and uncompressed PFS (.ffpfs, files stored raw) builds
//    STREAM it: listArchiveGame() gives the game from the archive's list
//    alone, the image is planned from that, and one pass through the archive
//    writes every file into the image (ArchiveStream.h). Nothing of the game
//    is written to disk outside the image, and the image is byte for byte the
//    one the unpacked folder gives. An .ffpfsc streams from a ZIP: its nested
//    image is compressed front to back, and a ZIP's files are read out of it
//    in that order (zip::EntryReader).
//  * zstd-pack builds (compressed exFAT, the ZSTD image, FFPKG and PFS with
//    packs) and the packed folder stream it too: the packer is fed in one
//    pass (ampr::packGameFromArchive, visitGameFiles) and its packs are byte
//    for byte the unpacked folder's. Only the packs and the loose files are
//    written (the work folder an image is then built from, as from a folder);
//    from a 7z or RAR, which give their files out of the packs' order, the
//    packs' compressed chunks also wait in a spool in the job's temporary
//    folder until the volumes are written in order.
//  * An .ffpfsc from a 7z or RAR, an .ffpfs with per-file compression, a game
//    that is itself packed inside the archive, and FPKG UNPACK it first
//    (unpackArchiveSource) into the job's private temporary folder and build
//    from the game inside exactly as from a folder: those need the files in
//    the image's order, or read the files at random, or (Sony's publishing
//    tool) read a folder. Their logs say so (unpackFirstLogLine). The
//    temporary copy is removed with the job's temp folder.
//
// Either way every entry's checksum is verified while it is decoded, and the
// same names are refused (ones that would leave the folder, that Windows
// cannot store or tell apart).
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
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace fw::archive {

struct ArchiveListing;

// An existing file named *.zip, *.7z or *.rar.
bool isArchiveSource(const QString& path);
bool isZipSource(const QString& path);
// An archive source read by the app's ZIP reader (by its first bytes, as
// listArchiveGame() tells them apart): its files can be read in any order.
// False for a 7z or RAR, which give their files in the order they store them.
bool isZipFormatArchive(const QString& path);

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

// run() of a build that cannot stream: unpacks the game (and nothing else the archive holds) into
// <tempDir>/zip-source and returns the game folder. Checks free space on the
// temp drive first, keeping `freeSpaceMarginBytes` in reserve (jobs pass
// storage.freeSpaceMarginMiB).
Result<QString> unpackArchiveSource(const QString& path, const QString& tempDir, const CancellationToken& token,
                                    ProgressReporter* progress = nullptr,
                                    qint64 freeSpaceMarginBytes = 256LL * 1024 * 1024);
Result<QString> unpackZipSource(const QString& zipPath, const QString& tempDir, const CancellationToken& token,
                                ProgressReporter* progress = nullptr,
                                qint64 freeSpaceMarginBytes = 256LL * 1024 * 1024);

// ---- the game, from the archive's list alone (builds that stream) --------

// One file or folder of the game, as it goes into an image.
struct GameItem {
    QString entry;       // its name in the archive; empty for a folder only implied by the names in it
    QString path;        // inside the game, '/' separated: what the unpacked folder would hold
    bool isDir = false;
    qint64 size = 0;     // files: as listed
    QDateTime modified;  // files: as listed, the time unpacking gives the file (invalid: none)
    int source = -1;     // files: index of the entry in the archive's list
};

struct GameListing {
    QString archive;     // the file the user chose
    bool zip = false;    // read by zip::Reader, else by libarchive (`listing`)
    std::shared_ptr<const ArchiveListing> listing;
    QString rootPrefix;  // as ZipGame::rootPrefix
    // Exactly what unpackArchiveSource() would make of the game: every file,
    // every folder (listed or implied by the names under it), no links;
    // sorted by path, so each folder comes before what it holds.
    std::vector<GameItem> items;
    qint64 files = 0, dirs = 0, bytes = 0;
    qint64 links = 0;    // never unpacked, so never in an image
    bool hasEboot = false; // eboot.bin at the top of the game

    // The item for a path inside the game (compared without regard to case), or -1.
    int find(const QString& path) const;
    // "Folder" for an archive of the game's folder; "" for one of its contents.
    QString rootName() const;
};

// The checks of checkArchiveSource() (readable, one game, every name safe and
// storable, no password), then the list. Nothing is decoded.
Result<GameListing> listArchiveGame(const QString& path, const CancellationToken& token = {});

// The whole contents of one small file of the game (`path` as in GameItem).
// In a 7z or RAR archive that decodes everything stored before it.
Result<QByteArray> readGameFile(const GameListing& game, const QString& path, qint64 maxSize,
                                const CancellationToken& token = {});

// Tests only: the whole archive as if it were the game (rootPrefix ""), with
// the name checks but without looking for eboot.bin or param.json, for the
// sample archives of libarchive's test suite.
Result<GameListing> listWholeArchiveForTesting(const QString& path, const CancellationToken& token = {});

// Tests only: what reading param.json out of a 7z or RAR archive may cost
// (-1: no limit), and forget every list read so far.
void setArchiveInspectBudgetForTesting(qint64 archiveBytes, int milliseconds);

} // namespace fw::archive
