// PFS / FFPFSC image builder.
//
// Port of Lazy_MkPFS lazy_mkpfs/build.py (build_pfs, build_pfs_stream_single_file)
// and compression.py (_encode_pfsc_into_handle, _analyze_pfsc_file_storage),
// derived from PSBrew/MkPFS. GPL-3.0-or-later.
//
// Compression runs as an ordered pipeline: the calling thread reads 1 MiB
// chunks in image order and submits them to a bounded worker pool; it then
// consumes the results strictly in submission order and writes them. Every
// 64 KiB block is compressed independently, so the bytes written do not
// depend on the number of workers.

#include "engines/pfs/PfsBuilder.h"

#include "engines/pfs/PfsIo.h"
#include "engines/pfs/PfscCodec.h"

#include <QElapsedTimer>
#include <QFileInfo>

#include <deque>

namespace fw::pfs {

namespace {

constexpr qint64 kInt32Max = 0x7FFFFFFF;
constexpr qint64 kCopyChunk = 4 * 1024 * 1024;

struct PfscResult {
    qint64 stored = 0;
    bool compressed = false;
    double gain = 0.0;
    qint64 encodedEnd = 0;    // end of the encoded stream written, relative to the payload start
    qint64 allCompressed = 0; // header + every block compressed
};

struct PipelineItem {
    bool encode = false; // false: stored raw, handled by onRaw
    qint64 size = 0;
    SeqSource* borrowed = nullptr;
    std::function<std::unique_ptr<SeqSource>()> open;
    QString label;
};

Status copyRaw(SeqSource& src, ImageWriter& out, qint64 offset, const CancellationToken& token,
               const std::function<void(qint64)>& advance)
{
    QByteArray buf(qsizetype(std::min<qint64>(kCopyChunk, std::max<qint64>(src.size(), 1))), Qt::Uninitialized);
    qint64 remaining = src.size();
    while (remaining > 0) {
        if (!token.checkpoint())
            return Error::cancelled();
        const qint64 take = std::min<qint64>(remaining, buf.size());
        FW_TRY_ASSIGN(const qint64 got, src.read(buf.data(), take));
        if (got != take)
            return Error(ErrorCode::IoError, QStringLiteral("%1 ended early.").arg(src.describe()));
        FW_TRY(out.writeAt(offset, buf.constData(), got));
        offset += got;
        remaining -= got;
        if (advance)
            advance(got);
    }
    return success();
}

class Pipeline {
public:
    Pipeline(CompressPool& pool, ImageWriter& out, const PfscParams& params, const CancellationToken& token,
             const BuildHooks& hooks, bool advanceRaw)
        : m_pool(pool), m_out(out), m_params(params), m_token(token), m_hooks(hooks), m_advanceRaw(advanceRaw)
    {
    }

    using BaseFn = std::function<qint64(int)>;
    using EncodedFn = std::function<Status(int, const PfscResult&)>;
    using RawFn = std::function<Status(int)>;

    Status run(const std::vector<PipelineItem>& items, const BaseFn& baseFor, const EncodedFn& onEncoded,
               const RawFn& onRaw)
    {
        struct Pending {
            int item = 0;
            bool first = false;
            bool last = false;
            std::shared_ptr<CompressPool::Task> task; // null: raw item
        };
        std::deque<Pending> queue;
        const size_t window = size_t(std::max(4, m_pool.threads() * 3));

        int prodItem = 0;
        qint64 prodOff = 0;
        std::unique_ptr<SeqSource> owned;
        SeqSource* prodSrc = nullptr;

        // Consumer state for the item being written.
        qint64 base = 0, pos = 0, blockCount = 0, headerSize = 0, compressedBlocks = 0, allCompressed = 0;
        std::vector<qint64> offsets;

        const int n = int(items.size());
        for (;;) {
            // ---- produce ----
            while (queue.size() < window && prodItem < n) {
                const PipelineItem& it = items[size_t(prodItem)];
                if (!it.encode || it.size <= 0) {
                    queue.push_back({prodItem, true, true, nullptr});
                    ++prodItem;
                    continue;
                }
                if (!prodSrc) {
                    if (it.borrowed) {
                        prodSrc = it.borrowed;
                        FW_TRY(prodSrc->rewind());
                    } else {
                        owned = it.open();
                        prodSrc = owned.get();
                    }
                    prodOff = 0;
                }
                if (!m_token.checkpoint())
                    return Error::cancelled();
                const qint64 take = std::min<qint64>(qint64(kBlocksPerTask) * kPfscBlock, it.size - prodOff);
                const int nblocks = int(ceilDiv(take, kPfscBlock));
                auto task = m_pool.makeTask(nblocks);
                m_clock.restart();
                FW_TRY_ASSIGN(const qint64 got, prodSrc->read(task->raw.data(), take));
                readNs += m_clock.nsecsElapsed();
                if (got != take)
                    return Error(ErrorCode::IoError, QStringLiteral("%1 ended early.").arg(prodSrc->describe()));
                if (qint64(task->raw.size()) > take)
                    std::memset(task->raw.data() + take, 0, size_t(task->raw.size() - take));
                task->rawBytes = take;
                m_pool.submit(task);
                const bool first = prodOff == 0;
                prodOff += take;
                const bool last = prodOff >= it.size;
                queue.push_back({prodItem, first, last, task});
                if (last) {
                    owned.reset();
                    prodSrc = nullptr;
                    ++prodItem;
                }
            }
            if (queue.empty())
                break;

            // ---- consume in order ----
            Pending p = std::move(queue.front());
            queue.pop_front();
            const PipelineItem& it = items[size_t(p.item)];
            if (!p.task) {
                if (m_hooks.item)
                    m_hooks.item(it.label);
                FW_TRY(onRaw(p.item));
                continue;
            }
            m_clock.restart();
            m_pool.wait(p.task);
            waitNs += m_clock.nsecsElapsed();
            m_clock.restart();
            if (!m_token.checkpoint())
                return Error::cancelled();
            const CompressPool::Task& t = *p.task;
            if (!t.error.isEmpty())
                return Error(ErrorCode::Internal, QStringLiteral("Compression failed."), t.error);
            if (p.first) {
                if (m_hooks.item)
                    m_hooks.item(it.label);
                base = baseFor(p.item);
                blockCount = ceilDiv(it.size, kPfscBlock);
                headerSize = pfscHeaderSize(blockCount);
                offsets.clear();
                offsets.reserve(size_t(blockCount + 1));
                offsets.push_back(headerSize);
                pos = base + headerSize;
                compressedBlocks = 0;
                allCompressed = 0;
            }
            for (int b = 0; b < t.nblocks; ++b) {
                const int clen = t.outLen[size_t(b)];
                allCompressed += clen;
                const bool useCompressed = pfscStoreCompressed(clen, m_params.blockGain);
                const char* data = useCompressed ? m_pool.compressed(t, b) : t.block(b);
                const qint64 len = useCompressed ? clen : kPfscBlock;
                if (useCompressed)
                    ++compressedBlocks;
                FW_TRY(m_out.writeAt(pos, data, len));
                pos += len;
                offsets.push_back(offsets.back() + len);
            }
            writeNs += m_clock.nsecsElapsed();
            if (m_advanceRaw && m_hooks.advance)
                m_hooks.advance(t.rawBytes);
            if (p.last) {
                PfscResult r;
                const qint64 raw = it.size;
                const qint64 encoded = offsets.back();
                r.encodedEnd = encoded;
                r.allCompressed = headerSize + allCompressed;
                r.stored = raw;
                if (compressedBlocks > 0 && encoded < raw) {
                    r.gain = (double(raw - encoded) / double(raw)) * 100.0;
                    if (r.gain >= double(m_params.minFileGain)) {
                        const QByteArray header = pfscHeaderArea(blockCount, offsets);
                        FW_TRY(m_out.writeAt(base, header.constData(), header.size()));
                        r.stored = encoded;
                        r.compressed = true;
                    }
                }
                FW_TRY(onEncoded(p.item, r));
            }
        }
        return success();
    }

private:
public:
    // Time the calling thread spent reading input, waiting for compression and writing.
    qint64 readNs = 0, waitNs = 0, writeNs = 0;

private:
    QElapsedTimer m_clock;
    CompressPool& m_pool;
    ImageWriter& m_out;
    const PfscParams& m_params;
    const CancellationToken& m_token;
    const BuildHooks& m_hooks;
    bool m_advanceRaw;
};

int effectiveThreads(int threads) { return std::max(1, std::min(threads, 256)); }

// Lazy build_pfs_stream_single_file.
Status writeSingleFile(SeqSource& src, const QByteArray& name, const BuildOptions& o, const PfscParams& p,
                       ImageWriter& out, const CancellationToken& token, const BuildHooks& hooks, bool advanceRaw,
                       BuildStats& st)
{
    FW_TRY_ASSIGN(SingleFileLayout L, buildSingleFileLayout(name, o.layout));
    const qint64 bs = o.layout.blockSize;
    const qint64 base = L.payloadBlock * bs;
    const qint64 raw = src.size();
    {
        const QByteArray placeholder = singleFileMetadata(L, 0);
        FW_TRY(out.writeAt(0, placeholder.constData(), placeholder.size()));
    }
    const bool compress = p.compress && raw > 0 && raw >= p.minCompressBytes()
        && !(p.skipExecutables && skipExecutableCompression(name, name));
    const auto advance = advanceRaw ? hooks.advance : std::function<void(qint64)>();

    PfscResult r;
    r.stored = raw;
    if (compress) {
        CompressPool pool(effectiveThreads(o.threads), o.level);
        Pipeline pipeline(pool, out, p, token, hooks, advanceRaw);
        std::vector<PipelineItem> items(1);
        items[0].encode = true;
        items[0].size = raw;
        items[0].borrowed = &src;
        items[0].label = QString::fromLatin1(name);
        FW_TRY(pipeline.run(
            items, [&](int) { return base; },
            [&](int, const PfscResult& res) {
                r = res;
                return success();
            },
            [](int) { return success(); }));
        st.readSeconds += pipeline.readNs / 1e9;
        st.waitSeconds += pipeline.waitNs / 1e9;
        st.writeSeconds += pipeline.writeNs / 1e9;
        if (!r.compressed) {
            // Lazy writes the raw file over the attempt; bytes of the attempt
            // beyond the file's end stay in its last block.
            src.silenceProgress();
            FW_TRY(src.rewind());
            FW_TRY(copyRaw(src, out, base, token, {}));
            if (raw < bs)
                FW_TRY(out.writeZeros(base + raw, bs - raw));
        }
    } else {
        FW_TRY(src.rewind());
        if (hooks.item)
            hooks.item(QString::fromLatin1(name));
        FW_TRY(copyRaw(src, out, base, token, advance));
        if (raw < bs)
            FW_TRY(out.writeZeros(base + raw, bs - raw));
    }

    const qint64 nd = finishSingleFile(L, raw, r.stored, r.compressed);
    if (nd > kInt32Max)
        return Error(ErrorCode::Unsupported, QStringLiteral("The source is too large for a PFS image."));
    const QByteArray meta = singleFileMetadata(L, nd);
    FW_TRY(out.writeAt(0, meta.constData(), meta.size()));
    FW_TRY(out.resize(nd * bs));

    st.files = 1;
    st.compressedFiles = r.compressed ? 1 : 0;
    st.sourceBytes = raw;
    st.storedBytes = r.stored;
    st.allCompressedBytes = r.allCompressed;
    st.containerCompressed = r.compressed;
    st.imageBytes = nd * bs;
    return success();
}

Result<FolderLayout> rawFolderLayout(const SourcePlan& plan, const LayoutOptions& lo)
{
    FW_TRY_ASSIGN(FolderLayout L, buildFolderLayout(plan, lo));
    qint64 cursor = L.firstFileBlock;
    for (int i = 0; i < int(plan.files.size()); ++i)
        cursor = placeFile(L, i, cursor, plan.files[size_t(i)].size, plan.files[size_t(i)].size, false);
    FW_TRY(finishLayout(L, cursor));
    return L;
}

// Lazy build_pfs with compress=False.
Status writeRawFolder(const SourcePlan& plan, const BuildOptions& o, ImageWriter& out, const CancellationToken& token,
                      const BuildHooks& hooks, BuildStats& st)
{
    FW_TRY_ASSIGN(FolderLayout L, rawFolderLayout(plan, o.layout));
    FolderImageSource img(L, plan);
    img.onSourceBytes = hooks.advance;
    QByteArray buf(qsizetype(kCopyChunk), Qt::Uninitialized);
    qint64 off = 0;
    while (off < img.size()) {
        if (!token.checkpoint())
            return Error::cancelled();
        const qint64 take = std::min<qint64>(kCopyChunk, img.size() - off);
        FW_TRY_ASSIGN(const qint64 got, img.read(buf.data(), take));
        if (got != take)
            return Error(ErrorCode::Internal, QStringLiteral("The folder image ended early."));
        FW_TRY(out.writeAt(off, buf.constData(), got));
        off += got;
    }
    FW_TRY(out.resize(img.size()));
    st.files = qint64(plan.files.size());
    st.sourceBytes = plan.totalBytes;
    st.storedBytes = plan.totalBytes;
    st.allCompressedBytes = plan.totalBytes;
    st.imageBytes = img.size();
    return success();
}

// Lazy build_pfs with compress=True (multi-worker path).
Status writeCompressedFolder(const SourcePlan& plan, const BuildOptions& o, ImageWriter& out,
                             const CancellationToken& token, const BuildHooks& hooks, BuildStats& st)
{
    FW_TRY_ASSIGN(FolderLayout L, buildFolderLayout(plan, o.layout));
    const qint64 bs = o.layout.blockSize;
    const PfscParams& p = o.files;

    // Metadata that does not depend on the payloads, in block order.
    auto put = [&](qint64 block, const QByteArray& blob) { return out.writeAt(block * bs, blob.constData(), blob.size()); };
    FW_TRY(put(L.inodes[size_t(L.superInode)].db[0], L.superBlob));
    FW_TRY(put(L.inodes[size_t(L.fptInode)].db[0], L.fptBlob));
    if (L.collisionInode >= 0)
        FW_TRY(put(L.inodes[size_t(L.collisionInode)].db[0], L.collisionBlob));
    for (const FolderLayout::Dir& d : L.dirs)
        FW_TRY(put(L.inodes[size_t(d.inode)].db[0], d.blob));

    const qint64 minBytes = p.minCompressBytes();
    std::vector<PipelineItem> items(plan.files.size());
    for (size_t i = 0; i < plan.files.size(); ++i) {
        const SourceFile& f = plan.files[i];
        PipelineItem& it = items[i];
        it.size = f.size;
        it.label = QString::fromLatin1(f.relPath);
        it.encode = p.compress && f.size > 0 && !(p.skipExecutables && skipExecutableCompression(f.name(), f.relPath))
            && f.size >= minBytes;
        it.open = [&f] { return openPlanFile(f); };
    }

    qint64 cursor = L.firstFileBlock;
    CompressPool pool(effectiveThreads(o.threads), o.level);
    Pipeline pipeline(pool, out, p, token, hooks, true);
    FW_TRY(pipeline.run(
        items, [&](int) { return cursor * bs; },
        [&](int i, const PfscResult& r) -> Status {
            const SourceFile& f = plan.files[size_t(i)];
            const qint64 base = cursor * bs;
            if (!r.compressed) {
                // Lazy discards the attempt and copies the file: rewrite it raw
                // and clear what the attempt left behind.
                auto src = openPlanFile(f);
                FW_TRY(copyRaw(*src, out, base, token, {}));
                if (r.encodedEnd > f.size)
                    FW_TRY(out.writeZeros(base + f.size, r.encodedEnd - f.size));
            } else {
                ++st.compressedFiles;
            }
            st.storedBytes += r.stored;
            st.allCompressedBytes += r.allCompressed;
            cursor = placeFile(L, i, cursor, f.size, r.stored, r.compressed);
            return success();
        },
        [&](int i) -> Status {
            const SourceFile& f = plan.files[size_t(i)];
            auto src = openPlanFile(f);
            FW_TRY(copyRaw(*src, out, cursor * bs, token, hooks.advance));
            st.storedBytes += f.size;
            st.allCompressedBytes += f.size;
            cursor = placeFile(L, i, cursor, f.size, f.size, false);
            return success();
        }));
    st.readSeconds += pipeline.readNs / 1e9;
    st.waitSeconds += pipeline.waitNs / 1e9;
    st.writeSeconds += pipeline.writeNs / 1e9;
    FW_TRY(finishLayout(L, cursor));
    const QByteArray header = layoutHeader(L);
    FW_TRY(out.writeAt(0, header.constData(), header.size()));
    const QByteArray table = inodeTableBytes(L.inodes, int(bs), L.inodeBlockCount);
    FW_TRY(out.writeAt(bs, table.constData(), table.size()));
    FW_TRY(out.resize(cursor * bs));
    st.files = qint64(plan.files.size());
    st.sourceBytes = plan.totalBytes;
    st.imageBytes = cursor * bs;
    return success();
}

QByteArray singleFileName(const BuildInput& in)
{
    if (!in.fileName.isEmpty())
        return in.fileName;
    return QFileInfo(in.file).fileName().toLatin1();
}

} // namespace

qint64 progressTotal(const BuildInput& input)
{
    if (input.isFolder)
        return input.plan.totalBytes;
    return QFileInfo(input.file).size();
}

Result<qint64> estimateImageSize(const BuildInput& input, OutputFormat format, const BuildOptions& options)
{
    const qint64 bs = options.layout.blockSize;
    qint64 payload = 0;
    if (input.isFolder) {
        FW_TRY_ASSIGN(FolderLayout L, rawFolderLayout(input.plan, options.layout));
        const qint64 folderImage = L.ndblock * bs;
        if (format == OutputFormat::FolderImage)
            return folderImage;
        payload = folderImage;
    } else {
        payload = QFileInfo(input.file).size();
    }
    FW_TRY_ASSIGN(SingleFileLayout S, buildSingleFileLayout(QByteArrayLiteral("x"), options.layout));
    return S.payloadBlock * bs + payloadBlocks(payload, int(bs)) * bs;
}

Result<BuildStats> writeImage(const BuildInput& input, OutputFormat format, const BuildOptions& options,
                              const QString& path, const CancellationToken& token, const BuildHooks& hooks,
                              const Log& log)
{
    if (options.level < 0 || options.level > 9)
        return Error(ErrorCode::InvalidArgument, QStringLiteral("Compression level must be between 1 and 9."));
    QElapsedTimer timer;
    timer.start();
    BuildStats st;
    st.threads = effectiveThreads(options.threads);

    ImageWriter out(path);
    FW_TRY(out.open());

    if (input.isFolder && format == OutputFormat::FolderImage) {
        if (options.files.compress)
            FW_TRY(writeCompressedFolder(input.plan, options, out, token, hooks, st));
        else
            FW_TRY(writeRawFolder(input.plan, options, out, token, hooks, st));
    } else if (input.isFolder) {
        FW_TRY_ASSIGN(FolderLayout L, rawFolderLayout(input.plan, options.layout));
        FolderImageSource nested(L, input.plan);
        nested.onSourceBytes = hooks.advance;
        FW_TRY(writeSingleFile(nested, kNestedImageName, options, options.container, out, token, hooks, false, st));
        st.nestedBytes = nested.size();
        st.files = qint64(input.plan.files.size());
        st.sourceBytes = input.plan.totalBytes;
    } else {
        const QByteArray name = singleFileName(input);
        if (name.isEmpty() || !isAscii(QString::fromLatin1(name)) || name.contains('/'))
            return Error(ErrorCode::InvalidArgument,
                         QStringLiteral("PFS images only support plain ASCII file names. Rename %1 first.")
                             .arg(QFileInfo(input.file).fileName()));
        FileSeqSource src(input.file);
        FW_TRY(src.open());
        PfscParams p = options.container;
        if (format == OutputFormat::FolderImage)
            p.compress = false; // single file stored raw (Lazy main_file --no-compress)
        FW_TRY(writeSingleFile(src, name, options, p, out, token, hooks, true, st));
    }
    FW_TRY(out.close());
    st.seconds = double(timer.nsecsElapsed()) / 1e9;
    log.debug(QStringLiteral("PFS image written: %1 bytes in %2 s with %3 thread(s); writer thread: read %4 s, "
                             "waiting for compression %5 s, write %6 s")
                  .arg(st.imageBytes)
                  .arg(st.seconds, 0, 'f', 2)
                  .arg(st.threads)
                  .arg(st.readSeconds, 0, 'f', 2)
                  .arg(st.waitSeconds, 0, 'f', 2)
                  .arg(st.writeSeconds, 0, 'f', 2));
    return st;
}

} // namespace fw::pfs
