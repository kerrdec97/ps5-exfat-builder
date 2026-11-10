// Streaming I/O for the PFS builder. GPL-3.0-or-later (part of the Lazy_MkPFS port).

#include "engines/pfs/PfsIo.h"

#include "core/FileUtil.h"

#include <QFileInfo>

#include <algorithm>
#include <cstring>

namespace fw::pfs {

// ---- FileSeqSource ------------------------------------------------------------------------

FileSeqSource::FileSeqSource(QString path, qint64 expectedSize)
    : m_path(std::move(path)), m_file(toLongPath(m_path)), m_size(expectedSize)
{
}

Status FileSeqSource::open()
{
    if (!m_file.isOpen() && !m_file.open(QIODevice::ReadOnly))
        return Error(ErrorCode::IoError,
                     QStringLiteral("Could not open %1.").arg(QFileInfo(m_path).fileName()), m_file.errorString());
    const qint64 actual = m_file.size();
    if (m_size < 0)
        m_size = actual;
    else if (actual != m_size)
        return Error(ErrorCode::IoError,
                     QStringLiteral("%1 changed while the image was being built. Close any program using the "
                                    "source and try again.")
                         .arg(QFileInfo(m_path).fileName()),
                     QStringLiteral("%1: expected %2 bytes, found %3").arg(m_path).arg(m_size).arg(actual));
    m_pos = 0;
    return success();
}

Result<qint64> FileSeqSource::read(char* dst, qint64 n)
{
    if (!m_file.isOpen())
        FW_TRY(open());
    const qint64 want = std::min(n, m_size - m_pos);
    qint64 got = 0;
    while (got < want) {
        const qint64 r = m_file.read(dst + got, want - got);
        if (r < 0)
            return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QFileInfo(m_path).fileName()),
                         m_file.errorString());
        if (r == 0)
            return Error(ErrorCode::IoError,
                         QStringLiteral("%1 became shorter while it was being read. Close programs that change it and try again.")
                             .arg(QFileInfo(m_path).fileName()),
                         m_path);
        got += r;
    }
    m_pos += got;
    return got;
}

Status FileSeqSource::rewind()
{
    if (!m_file.isOpen())
        return open();
    if (!m_file.seek(0))
        return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QFileInfo(m_path).fileName()),
                     m_file.errorString());
    m_pos = 0;
    return success();
}

// ---- MemorySeqSource ----------------------------------------------------------------------

Result<qint64> MemorySeqSource::read(char* dst, qint64 n)
{
    const qint64 take = std::min(n, qint64(m_data.size()) - m_pos);
    if (take > 0)
        std::memcpy(dst, m_data.constData() + m_pos, size_t(take));
    m_pos += std::max<qint64>(0, take);
    return std::max<qint64>(0, take);
}

std::unique_ptr<SeqSource> openPlanFile(const SourceFile& f)
{
    if (f.generated)
        return std::make_unique<MemorySeqSource>(f.content, QString::fromLatin1(f.relPath));
    return std::make_unique<FileSeqSource>(f.absPath, f.size);
}

// ---- FolderImageSource ----------------------------------------------------------------

FolderImageSource::FolderImageSource(const FolderLayout& L, const SourcePlan& plan) : m_plan(plan)
{
    const qint64 bs = L.options.blockSize;
    auto add = [&](qint64 block, qint64 blocks, QByteArray mem, int file) {
        Segment s;
        s.offset = block * bs;
        s.length = blocks * bs;
        s.mem = std::move(mem);
        s.file = file;
        m_segments.push_back(std::move(s));
    };
    add(0, 1, layoutHeader(L), -1);
    add(1, L.inodeBlockCount, inodeTableBytes(L.inodes, int(bs), L.inodeBlockCount), -1);
    const Inode& sr = L.inodes[size_t(L.superInode)];
    add(sr.db[0], sr.blocks, L.superBlob, -1);
    const Inode& fpt = L.inodes[size_t(L.fptInode)];
    add(fpt.db[0], fpt.blocks, L.fptBlob, -1);
    if (L.collisionInode >= 0) {
        const Inode& col = L.inodes[size_t(L.collisionInode)];
        add(col.db[0], col.blocks, L.collisionBlob, -1);
    } else {
        add(fpt.db[0] + fpt.blocks, 1, QByteArray(), -1); // reserved empty block
    }
    for (const FolderLayout::Dir& d : L.dirs) {
        const Inode& ino = L.inodes[size_t(d.inode)];
        add(ino.db[0], ino.blocks, d.blob, -1);
    }
    for (size_t i = 0; i < plan.files.size(); ++i) {
        const Inode& ino = L.inodes[size_t(L.fileInode[i])];
        add(ino.db[0], ino.blocks, QByteArray(), int(i));
    }
    m_size = L.ndblock * bs;
}

Status FolderImageSource::rewind()
{
    m_pos = 0;
    m_seg = 0;
    m_open.reset();
    m_openRead = 0;
    return success();
}

Result<qint64> FolderImageSource::read(char* dst, qint64 n)
{
    qint64 done = 0;
    while (done < n && m_pos < m_size && m_seg < m_segments.size()) {
        const Segment& s = m_segments[m_seg];
        if (m_pos < s.offset) { // gap (never expected): zeros
            const qint64 take = std::min(n - done, s.offset - m_pos);
            std::memset(dst + done, 0, size_t(take));
            done += take;
            m_pos += take;
            continue;
        }
        const qint64 within = m_pos - s.offset;
        const qint64 take = std::min(n - done, s.length - within);
        char* out = dst + done;
        if (s.file < 0) {
            const qint64 memAvail = std::max<qint64>(0, qint64(s.mem.size()) - within);
            const qint64 fromMem = std::min(take, memAvail);
            if (fromMem > 0)
                std::memcpy(out, s.mem.constData() + within, size_t(fromMem));
            if (take > fromMem)
                std::memset(out + fromMem, 0, size_t(take - fromMem));
        } else {
            const SourceFile& f = m_plan.files[size_t(s.file)];
            const qint64 dataAvail = std::max<qint64>(0, f.size - within);
            const qint64 fromFile = std::min(take, dataAvail);
            if (fromFile > 0) {
                if (!m_open) {
                    m_open = openPlanFile(f);
                    m_openRead = 0;
                }
                FW_TRY_ASSIGN(const qint64 got, m_open->read(out, fromFile));
                if (got != fromFile)
                    return Error(ErrorCode::IoError,
                                 QStringLiteral("%1 became shorter while it was being read. Close programs that change it and try again.")
                                     .arg(QString::fromLatin1(f.relPath)));
                m_openRead += got;
                if (onSourceBytes)
                    onSourceBytes(got);
            }
            if (take > fromFile)
                std::memset(out + fromFile, 0, size_t(take - fromFile));
        }
        done += take;
        m_pos += take;
        if (m_pos >= s.offset + s.length) {
            m_open.reset();
            m_openRead = 0;
            ++m_seg;
        }
    }
    return done;
}

// ---- ImageWriter --------------------------------------------------------------------------

ImageWriter::ImageWriter(QString path, qint64 bufferBytes) : m_path(std::move(path)), m_file(toLongPath(m_path))
{
    m_buf.resize(qsizetype(bufferBytes));
}

ImageWriter::~ImageWriter()
{
    if (m_file.isOpen())
        m_file.close();
}

Status ImageWriter::open()
{
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Unbuffered))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create %1.").arg(QFileInfo(m_path).fileName()),
                     m_file.errorString());
    m_bufStart = 0;
    m_bufLen = 0;
    m_fileEnd = 0;
    return success();
}

Status ImageWriter::flushBuffer()
{
    if (m_bufLen == 0)
        return success();
    if (m_file.pos() != m_bufStart && !m_file.seek(m_bufStart))
        return Error(ErrorCode::IoError, QStringLiteral("Could not write the image."), m_file.errorString());
    qint64 off = 0;
    while (off < m_bufLen) {
        const qint64 w = m_file.write(m_buf.constData() + off, m_bufLen - off);
        if (w <= 0)
            return Error(ErrorCode::IoError,
                         QStringLiteral("Could not write the image. The drive may be full or disconnected."),
                         m_file.errorString());
        off += w;
    }
    m_bufStart += m_bufLen;
    m_bufLen = 0;
    return success();
}

Status ImageWriter::writeAt(qint64 offset, const char* data, qint64 len)
{
    if (len <= 0)
        return success();
    // Never leave a gap past the end of the file: its content is undefined on
    // some platforms, and PFS padding must be zero.
    if (offset > m_fileEnd)
        FW_TRY(writeZeros(m_fileEnd, offset - m_fileEnd));
    m_fileEnd = std::max(m_fileEnd, offset + len);
    const qint64 cap = m_buf.size();
    if (m_bufLen > 0 && offset == m_bufStart + m_bufLen && m_bufLen + len <= cap) {
        std::memcpy(m_buf.data() + m_bufLen, data, size_t(len));
        m_bufLen += len;
        return success();
    }
    FW_TRY(flushBuffer());
    if (len >= cap) {
        if (!m_file.seek(offset))
            return Error(ErrorCode::IoError, QStringLiteral("Could not write the image."), m_file.errorString());
        qint64 off = 0;
        while (off < len) {
            const qint64 w = m_file.write(data + off, len - off);
            if (w <= 0)
                return Error(ErrorCode::IoError,
                             QStringLiteral("Could not write the image. The drive may be full or disconnected."),
                             m_file.errorString());
            off += w;
        }
        m_bufStart = offset + len;
        return success();
    }
    m_bufStart = offset;
    std::memcpy(m_buf.data(), data, size_t(len));
    m_bufLen = len;
    return success();
}

Status ImageWriter::writeZeros(qint64 offset, qint64 len)
{
    static const QByteArray zeros(1 << 20, '\0');
    while (len > 0) {
        const qint64 take = std::min<qint64>(len, zeros.size());
        FW_TRY(writeAt(offset, zeros.constData(), take));
        offset += take;
        len -= take;
    }
    return success();
}

Status ImageWriter::flush()
{
    FW_TRY(flushBuffer());
    if (!m_file.flush())
        return Error(ErrorCode::IoError, QStringLiteral("Could not write the image."), m_file.errorString());
    return success();
}

Status ImageWriter::resize(qint64 size)
{
    if (size > m_fileEnd)
        FW_TRY(writeZeros(m_fileEnd, size - m_fileEnd));
    FW_TRY(flushBuffer());
    if (!m_file.resize(size))
        return Error(ErrorCode::IoError,
                     QStringLiteral("Could not set the image size. The drive may be full."), m_file.errorString());
    m_fileEnd = size;
    return success();
}

Status ImageWriter::close()
{
    FW_TRY(flush());
    m_file.close();
    return success();
}

} // namespace fw::pfs
