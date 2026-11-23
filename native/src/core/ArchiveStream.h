#pragma once

// Streaming an archived dump straight into an image (GitHub #37), with no
// copy of the game on disk.
//
// An image builder plans its layout from the archive's list alone
// (listArchiveGame(), ArchiveSource.h: names, sizes and times are listed
// without decoding anything), writes everything but the files' data, and
// then calls streamGameInto(): ONE sequential pass through the archive (what
// a solid 7z or RAR needs) that writes each file's bytes at the offsets the
// plan gave it, in whatever order the archive stores them. A ZIP, which can
// be read in any order, is read in the order of the image instead.
//
// Every entry's checksum is verified while it is decoded (CRC-32 of a ZIP
// entry, the 7z and RAR checksums), as when unpacking; an entry that is
// damaged, larger or smaller than listed, not listed or listed twice fails
// the pass. While a file is written its bytes are digested (ContentDigest):
// the build reads the finished image back and compares each file with that
// digest, since there is no folder to compare it with.
//
// Memory: one write buffer of `bufferBytes` (plus what the decoder of the
// archive's method needs, as when unpacking). Nothing is written anywhere but
// into the image.

#include "core/ArchiveSource.h"
#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QString>

#include <vector>

namespace fw::archive {

// A run of one file's bytes in the image.
struct Extent {
    qint64 fileOffset = 0;  // where the run starts in the file
    qint64 imageOffset = 0; // where it goes in the image
    qint64 length = 0;
};

// Where an item of GameListing::items goes. `write` false: not in the image
// (excluded, or replaced by a file the build made); it is passed over. The
// extents of a written file cover it exactly, in file order (none for an
// empty file).
struct Placement {
    bool write = false;
    std::vector<Extent> extents;
};

// A 64-bit digest of a file's bytes, the same however they are cut into
// pieces: CRC-32 and Adler-32 (zlib) side by side. It tells a file that was
// written or read back wrongly from the one decoded from the archive; it is
// not meant to stand against someone forging data.
class ContentDigest {
public:
    void add(const char* data, qint64 size);
    quint64 value() const { return (quint64(m_crc) << 32) | m_adler; }

private:
    quint32 m_crc = 0;
    quint32 m_adler = 1;
};

struct StreamStats {
    qint64 files = 0;            // files written
    qint64 bytes = 0;            // their bytes
    std::vector<quint64> digests; // per item; 0 for an item not written
    qint64 elapsedMs = 0;
};

// Writes the game's files into `image`, an existing file that already has its
// full length (the builder's metadata pass made it). Placements are indexed
// like game.items. Runs of zeros of 1 MiB or more are left as they are (the
// image is new, so they read back as zeros). Progress: bytes and files
// written. The image is flushed, not synced to the device.
Result<StreamStats> streamGameInto(const GameListing& game, const std::vector<Placement>& placements, const QString& image,
                                   qint64 bufferBytes, const CancellationToken& token, ProgressReporter* progress = nullptr);

// What a build log says about a streamed archive: "Building straight from
// Game.7z (one pass, nothing is unpacked to disk)".
QString streamingLogLine(const QString& archive);
// What it says when the archive is unpacked first, and why.
QString unpackFirstLogLine(const QString& archive, const QString& why);

} // namespace fw::archive
