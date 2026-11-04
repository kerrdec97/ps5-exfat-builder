#pragma once

// PFS image reader: opens an image (a file, or a file inside another image),
// validates its structure, lists the tree and streams file contents,
// decompressing PFSC blocks.
//
// Validation is a port of Lazy_MkPFS lazy_mkpfs/inspect.py inspect_pfs_image
// (validate_inode_layout, parse_superroot_and_indexes, build_tree_from_uroot,
// validate_fpt_maps, orphan check) and iter_inode_logical_blocks.
// GPL-3.0-or-later.
//
// Damage is reported as ErrorCode::CorruptData. Signed and encrypted images
// are not supported (ErrorCode::Unsupported): no tool available here produces
// them (Lazy_MkPFS's signed build path fails), so that code could not be tested.

#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/pfs/PfsFormat.h"

#include <QByteArray>
#include <QFile>
#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace fw::pfs {

class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual qint64 size() const = 0;
    // Bytes at the start of the source that are really stored; the rest of
    // size() reads as zeros (a short image). Metadata must lie in this part.
    virtual qint64 presentSize() const { return size(); }
    // Reads exactly `len` bytes at `offset` (fails past the end).
    virtual Status readAt(qint64 offset, char* dst, qint64 len) = 0;
    virtual QString describe() const = 0;
};

class FileByteSource final : public ByteSource {
public:
    explicit FileByteSource(QString path);
    Status open();
    qint64 size() const override { return m_size; }
    Status readAt(qint64 offset, char* dst, qint64 len) override;
    QString describe() const override { return m_path; }

private:
    QString m_path;
    QFile m_file;
    qint64 m_size = 0;
    std::mutex m_mutex;
};

struct PfsHeaderInfo {
    qint64 version = 0;
    qint64 magic = 0;
    quint16 mode = 0;
    int blockSize = 0;
    qint64 nblock = 0;
    qint64 inodeCount = 0;
    qint64 ndblock = 0;
    qint64 inodeBlockCount = 0;
    int readonly = 0;

    bool isSigned() const { return (mode & kModeSigned) != 0; }
    bool caseInsensitive() const { return (mode & kModeCaseInsensitive) != 0; }
    int inodeBits() const { return (mode & kMode64BitInodes) ? 64 : 32; }
};

struct PfsEntry {
    QByteArray path; // relative to uroot, '/' separators
    bool isDir = false;
    quint32 inode = 0;
};

class PfsImage {
public:
    // Parses and validates the whole structure (no file contents are read).
    static Result<std::unique_ptr<PfsImage>> open(std::shared_ptr<ByteSource> source);

    const PfsHeaderInfo& header() const { return m_header; }
    const std::vector<Inode>& inodes() const { return m_inodes; }
    const std::vector<PfsEntry>& files() const { return m_files; } // sorted by path
    const std::vector<PfsEntry>& dirs() const { return m_dirs; }   // sorted by path, without uroot
    const PfsEntry* findFile(const QByteArray& path) const;
    const QStringList& warnings() const { return m_warnings; }
    std::shared_ptr<ByteSource> source() const { return m_source; }
    QString describe() const { return m_source->describe(); }

    // Streams the logical content of a file inode in chunks of at most 4 MiB.
    using Sink = std::function<Status(const char* data, qint64 len)>;
    Status readFile(const Inode& inode, const Sink& sink, const CancellationToken& token) const;

    // Random access to a file's logical bytes (used to open a nested image).
    // The returned source refers to this image, which must outlive it.
    Result<std::shared_ptr<ByteSource>> openFile(const Inode& inode) const;

    // Reads image bytes; inside ndblock but past the end of a short image the
    // bytes read as zeros.
    Status readBytes(qint64 offset, char* dst, qint64 len) const;

    // Bytes of the image that are really stored (at most ndblock blocks); a
    // short image reads as zeros after them. Metadata and PFSC block tables
    // must lie inside this part.
    qint64 presentBytes() const;

    // Bytes the image file is shorter than the header says (Lazy RAM-mode images).
    qint64 missingTailBytes() const { return m_missingTail; }

private:
    PfsImage() = default;
    Status parse();
    Result<QByteArray> readPayload(const Inode& inode) const;
    // readBytes() limited to presentBytes(): metadata is never zero-filled.
    Status readMetadata(qint64 offset, char* dst, qint64 len, const QString& what) const;
    void problem(const QString& what) { m_problems << what; }

    std::shared_ptr<ByteSource> m_source;
    PfsHeaderInfo m_header;
    std::vector<Inode> m_inodes;
    std::vector<PfsEntry> m_files;
    std::vector<PfsEntry> m_dirs;
    std::map<QByteArray, size_t> m_fileIndex;
    QStringList m_problems;
    QStringList m_warnings;
    qint64 m_missingTail = 0;
};

// One stored PFSC payload, randomly accessible by logical offset.
class PfscReader {
public:
    // `base` is the absolute offset of the payload inside `source`, `stored`
    // its stored size and `logical` the inode's logical size. The header, the
    // block offset table and every stored block must lie inside
    // source->presentSize(), so a crafted header cannot make the reader
    // allocate or zero-fill more than the image holds.
    static Result<std::unique_ptr<PfscReader>> open(std::shared_ptr<ByteSource> source, qint64 base, qint64 stored,
                                                    qint64 logical);
    qint64 logicalSize() const { return m_logical; }
    qint64 blockCount() const { return m_blockCount; }
    // Decodes logical block `index` (64 KiB) into `out`.
    Status decodeBlock(qint64 index, char* out);

private:
    PfscReader() = default;
    std::shared_ptr<ByteSource> m_source;
    qint64 m_base = 0;
    qint64 m_logical = 0;
    qint64 m_blockCount = 0;
    std::vector<qint64> m_offsets;
    QByteArray m_stored;
    struct InflaterHolder;
    std::shared_ptr<InflaterHolder> m_inflater;
};

} // namespace fw::pfs
