#pragma once

// A game's files for the pack engine, independent of where they are stored: a
// folder, an image (the integration module adapts its FileTree readers), or a
// packed game seen through its packs (UnpackedGame).
//
// Paths are '/'-separated, relative to the game root, case as stored. A
// GameFiles object is used by one thread at a time.

#include "core/ArchiveSource.h"
#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/ampr/ZstdPack.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace fw::ampr {

struct GameItem {
    QString path;
    bool isDir = false;
    qint64 size = 0;
    qint64 mtime = 0; // seconds since the epoch, 0 = unknown
};

class GameFiles {
public:
    virtual ~GameFiles() = default;
    virtual QString describe() const = 0;
    virtual const std::vector<GameItem>& items() const = 0;
    // Exact path; -1 when absent.
    virtual int find(const QString& path) const = 0;
    // Reads up to `len` bytes of item `index` from `offset` (0 at the end).
    virtual Result<qint64> read(int index, qint64 offset, char* dst, qint64 len) = 0;
    // What the listing left out or could not read properly (skipped links, a
    // damaged image); for the job's warnings.
    virtual QStringList warnings() const { return {}; }
};

// A folder on disk: hidden and system files included; symbolic links and
// junctions followed (one that loops back into its own folder or has no
// target is left out with a warning). Fails when a folder cannot be listed.
Result<std::shared_ptr<GameFiles>> openFolderGame(const QString& folder, const CancellationToken& token);

// An archived dump's game (GitHub #37) from the archive's list alone: the
// files and folders unpackArchiveSource() would make of it (links left out),
// each file with the time the archive lists (or `noTime` when it lists none,
// as an unpacked file would get the time it was unpacked). Its files cannot be
// read at random: read() fails, and packGameFromArchive() reads them in one
// pass (archive::visitGameFiles).
class ArchiveGame final : public GameFiles {
public:
    ArchiveGame(archive::GameListing listing, qint64 noTime);
    QString describe() const override;
    const std::vector<GameItem>& items() const override { return m_items; }
    int find(const QString& path) const override { return m_index.value(path, -1); }
    Result<qint64> read(int index, qint64 offset, char* dst, qint64 len) override;
    QStringList warnings() const override;

    const archive::GameListing& listing() const { return m_listing; }
    // The listing's item of each of items().
    int listingItem(int index) const { return m_source[size_t(index)]; }

private:
    archive::GameListing m_listing;
    std::vector<GameItem> m_items;
    std::vector<int> m_source;
    QHash<QString, int> m_index;
};

// Random access to one item of a GameFiles (for pack volumes inside an image).
std::shared_ptr<RandomAccess> itemAccess(std::shared_ptr<GameFiles> files, int index);

enum class PackKind { None, Zstd, Lz4 };
QString packKindName(PackKind kind); // "zstd", "LZ4", ""
// What a game's root holds: ampr_zstd.index -> zstd, ampr_assets.index -> LZ4.
PackKind detectPackKind(const GameFiles& files);

// A packed game as plain files: the packed files are served from the packs
// (decoded on the fly) and the pack files themselves (index, volumes, LZ4
// manifest and its companions) are left out. Everything else is the base's.
class UnpackedGame final : public GameFiles {
public:
    static Result<std::shared_ptr<UnpackedGame>> open(std::shared_ptr<GameFiles> base, const CancellationToken& token);

    QString describe() const override;
    const std::vector<GameItem>& items() const override { return m_items; }
    int find(const QString& path) const override { return m_index.value(path, -1); }
    Result<qint64> read(int index, qint64 offset, char* dst, qint64 len) override;
    QStringList warnings() const override;

    PackKind kind() const { return m_kind; }
    qint64 packedFiles() const { return m_packedFiles; }
    qint64 packedBytes() const { return m_packedBytes; }
    QStringList droppedFiles() const { return m_dropped; } // the pack files left out

    class Decoder; // one pack format's file reader

private:
    UnpackedGame() = default;

    struct Source {
        bool packed = false;
        int index = -1; // base item, or the decoder's file
    };
    std::shared_ptr<GameFiles> m_base;
    std::shared_ptr<Decoder> m_decoder;
    PackKind m_kind = PackKind::None;
    std::vector<GameItem> m_items;
    std::vector<Source> m_sources;
    QHash<QString, int> m_index;
    qint64 m_packedFiles = 0;
    qint64 m_packedBytes = 0;
    QStringList m_dropped;
};

// The files a pack or unpack job may read as one tree: a packed game is
// unpacked, a plain one passed through.
Result<std::shared_ptr<GameFiles>> logicalGame(std::shared_ptr<GameFiles> files, const CancellationToken& token);

} // namespace fw::ampr
