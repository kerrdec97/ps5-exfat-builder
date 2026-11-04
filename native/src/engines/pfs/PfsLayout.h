#pragma once

// Metadata layout of an unsigned PFS image: inode numbering, directory
// entries, flat_path_table, collision_resolver and block allocation.
//
// Port of Lazy_MkPFS lazy_mkpfs/build.py (build_pfs, unsigned branch, and
// build_pfs_stream_single_file) and inspect.py (make_fpt_and_collision_blob).
// GPL-3.0-or-later.
//
// Block order of a folder image:
//   0                      header
//   1 .. ibc               inode table (D32 inodes, 0xA8 bytes, never split across blocks)
//   1+ibc                  super-root directory (flat_path_table, [collision_resolver], uroot)
//   next                   flat_path_table (8 bytes per path, sorted by hash)
//   next                   collision_resolver, or one reserved empty block
//   next                   uroot, then every other directory (case-insensitive path order)
//   next                   file payloads in case-insensitive path order, each block-aligned

#include "core/Result.h"
#include "engines/pfs/PfsFormat.h"
#include "engines/pfs/PfsSource.h"

#include <QByteArray>

#include <vector>

namespace fw::pfs {

struct LayoutOptions {
    int blockSize = 0x10000;
    int version = kVersionPs5;
    bool caseInsensitive = true;
    qint64 time = 0;
};

struct FolderLayout {
    struct Dir {
        QByteArray rel; // "" for uroot
        QByteArray name;
        int parent = -1;
        std::vector<int> childDirs;
        std::vector<int> childFiles; // indices into SourcePlan::files
        int inode = -1;
        QByteArray blob; // directory entries
    };

    LayoutOptions options;
    std::vector<Dir> dirs; // index 0 = uroot, then case-insensitive path order
    std::vector<int> fileInode; // plan file index -> inode index
    std::vector<Inode> inodes;  // inode table order; inodes[i].number == i
    int superInode = 0;
    int fptInode = 1;
    int collisionInode = -1;
    int urootInode = 2;
    QByteArray superBlob;
    QByteArray fptBlob;
    QByteArray collisionBlob;
    qint64 inodeBlockCount = 0;
    qint64 firstFileBlock = 0; // after metadata and directories
    qint64 ndblock = 0;        // valid after every file is placed

    quint16 mode() const { return options.caseInsensitive ? kModeCaseInsensitive : 0; }
};

// Builds directories, inodes, dirents and the FPT for `plan`, then allocates
// every metadata and directory block. Files still need placeFile().
Result<FolderLayout> buildFolderLayout(const SourcePlan& plan, const LayoutOptions& options);

// Records file `fileIndex` at `startBlock`. Returns the next free block.
qint64 placeFile(FolderLayout& layout, int fileIndex, qint64 startBlock, qint64 rawSize, qint64 storedSize,
                 bool compressed);

// Final checks (D32 pointer ranges) once every file is placed.
Status finishLayout(FolderLayout& layout, qint64 ndblock);

QByteArray inodeTableBytes(const std::vector<Inode>& inodes, int blockSize, qint64 inodeBlockCount);
QByteArray layoutHeader(const FolderLayout& layout);

inline qint64 payloadBlocks(qint64 stored, int blockSize)
{
    return stored > 0 ? std::max<qint64>(1, ceilDiv(stored, blockSize)) : 1;
}

// Uncompressed image size of `plan` (every file stored raw).
qint64 uncompressedImageSize(const FolderLayout& layout, const SourcePlan& plan);

// ---- single-file image (Lazy build_pfs_stream_single_file) --------------------------
struct SingleFileLayout {
    LayoutOptions options;
    QByteArray name;
    std::vector<Inode> inodes; // super root, fpt, uroot, file
    QByteArray superBlob, fptBlob, urootBlob;
    qint64 payloadBlock = 0;   // first block of the file payload (6)

    quint16 mode() const { return options.caseInsensitive ? kModeCaseInsensitive : 0; }
};

Result<SingleFileLayout> buildSingleFileLayout(const QByteArray& name, const LayoutOptions& options);
// Sets the file inode for the stored payload and returns the final block count.
qint64 finishSingleFile(SingleFileLayout& layout, qint64 rawSize, qint64 storedSize, bool compressed);
// Everything before the payload (payloadBlock * blockSize bytes), given the final ndblock.
QByteArray singleFileMetadata(const SingleFileLayout& layout, qint64 ndblock);

} // namespace fw::pfs
