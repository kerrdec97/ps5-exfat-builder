// SELF writer (fake signing). Port of make_fself.py by john-tornblom
// (ps5-payload-dev/sdk, GNU GPL version 3), as modified by Nazky
// (@NazkyYT). Distributed under the GNU GPL version 3. See SelfWriter.h.

#include "engines/backport/SelfWriter.h"

#include "engines/backport/Common.h"
#include "engines/backport/ElfImage.h"

#include <QCryptographicHash>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <vector>

namespace fw::backport {

namespace {

// A growable buffer with file-like seek/write semantics (zero-filled gaps),
// so the write order of make_fself is reproduced exactly.
class ByteWriter {
public:
    void seek(quint64 pos) { m_pos = pos; }
    quint64 tell() const { return m_pos; }
    void write(QByteArrayView bytes)
    {
        const quint64 end = m_pos + quint64(bytes.size());
        if (quint64(m_buf.size()) < end)
            m_buf.append(QByteArray(qsizetype(end - quint64(m_buf.size())), '\0'));
        if (!bytes.isEmpty())
            std::memcpy(m_buf.data() + m_pos, bytes.data(), size_t(bytes.size()));
        m_pos = end;
    }
    template <typename T>
    void put(T v)
    {
        char b[sizeof(T)];
        qToLittleEndian<T>(v, b);
        write(QByteArrayView(b, qsizetype(sizeof(T))));
    }
    void zeros(qsizetype n) { write(QByteArray(n, '\0')); }
    QByteArray take() { return std::move(m_buf); }

private:
    QByteArray m_buf;
    quint64 m_pos = 0;
};

struct Entry {
    quint64 props = 0;
    quint64 offset = 0;
    quint64 filesz = 0;
    quint64 memsz = 0;
    QByteArrayView data;
    quint64 metaBlocks = 0; // meta entries: number of empty digests
    bool isMeta = false;
};

} // namespace

Result<QByteArray> makeFakeSelf(QByteArrayView elfData, const SignParams& params)
{
    FW_TRY_ASSIGN(const ElfImage img, ElfImage::parse(elfData, Strictness::Signable));
    ElfHeader eh = img.header();
    eh.shnum = 0; // section headers are ignored

    // Segment bytes as make_fself reads them: [p_offset, p_offset + p_filesz).
    // A truncated segment is refused instead of silently shortened.
    const auto segment = [&](const ProgramHeader& ph) -> QByteArrayView {
        if (ph.filesz == 0)
            return {};
        return elfData.sliced(qsizetype(ph.offset), qsizetype(ph.filesz));
    };
    QByteArrayView versionData;
    bool haveVersion = false;
    std::vector<const ProgramHeader*> signedSegments;
    for (const ProgramHeader& ph : img.programHeaders()) {
        const bool relevant = ph.type == elf::PT_SCE_VERSION || elf::isSelfSegmentType(ph.type);
        if (relevant && ph.filesz > 0 && !img.contains(ph.offset, ph.filesz))
            return makeError(Code::ElfInvalid,
                             QStringLiteral("program header %1 points past the end of the file (the file is cut short)")
                                 .arg(ph.index));
        if (ph.type == elf::PT_SCE_VERSION) {
            versionData = segment(ph); // the last one wins, as in make_fself
            haveVersion = true;
        }
        if (elf::isSelfSegmentType(ph.type))
            signedSegments.push_back(&ph);
    }

    const quint64 numEntries = quint64(signedSegments.size()) * 2;
    if (numEntries > 0xFFFF)
        return makeError(Code::SignUnsupported, QStringLiteral("it has too many segments for a SELF"));
    const quint64 elfHeaderSize = self::alignUp(
        std::max<quint64>(eh.ehsize, eh.phoff + quint64(eh.phentsize) * eh.phnum), 16);
    quint64 headerSize = self::kCommonHeaderSize + numEntries * self::kEntrySize;
    headerSize += std::max<quint64>(eh.ehsize, eh.phoff + quint64(eh.phentsize) * eh.phnum);
    headerSize = self::alignUp(headerSize, 16);
    headerSize += self::kExInfoSize + self::kNpdrmBlockSize;
    const quint64 metaSize = numEntries * self::kMetaBlockSize + self::kMetaFooterSize + self::kSignatureSize;
    if (headerSize > 0xFFFF || metaSize > 0xFFFF)
        return makeError(Code::SignUnsupported,
                         QStringLiteral("its headers are too large for a SELF (0x%1 bytes)").arg(headerSize, 0, 16));

    // Entries: a meta entry (empty digests) and a data entry per segment.
    std::vector<Entry> entries;
    entries.reserve(size_t(numEntries));
    quint64 offset = headerSize + metaSize;
    for (size_t k = 0; k < signedSegments.size(); ++k) {
        const ProgramHeader& ph = *signedSegments[k];
        const quint64 metaIndex = quint64(k) * 2;
        Entry meta;
        meta.isMeta = true;
        meta.props = self::props::withSegmentIndex(self::props::kSigned | self::props::kHasDigests, metaIndex + 1);
        meta.metaBlocks = self::alignUp(ph.filesz, self::kBlockSize) / self::kBlockSize;
        meta.offset = offset;
        meta.filesz = meta.memsz = meta.metaBlocks * self::kDigestSize;
        offset = self::alignUp(offset + meta.filesz, 16);

        Entry dataEntry;
        quint64 p = self::props::kSigned | self::props::kHasBlocks;
        p |= quint64(2) << self::props::kBlockSizeShift; // 0x4000 blocks: ilog2(0x4000) - 12
        dataEntry.props = self::props::withSegmentIndex(p, quint64(ph.index));
        dataEntry.data = segment(ph);
        dataEntry.offset = offset;
        dataEntry.filesz = dataEntry.memsz = ph.filesz;
        offset = self::alignUp(offset + dataEntry.filesz, 16);

        entries.push_back(meta);
        entries.push_back(dataEntry);
    }
    const quint64 fileSize = offset;

    ByteWriter w;
    // Common + extended header.
    w.put<quint32>(self::kMagicFake);
    w.put<quint8>(self::kVersion);
    w.put<quint8>(self::kMode);
    w.put<quint8>(self::kEndian);
    w.put<quint8>(self::kAttributes);
    w.put<quint32>(self::kKeyType);
    w.put<quint16>(quint16(headerSize));
    w.put<quint16>(quint16(metaSize));
    w.put<quint64>(fileSize);
    w.put<quint16>(quint16(numEntries));
    w.put<quint16>(self::kFlags);
    w.zeros(4);
    for (const Entry& e : entries) {
        w.put<quint64>(e.props);
        w.put<quint64>(e.offset);
        w.put<quint64>(e.filesz);
        w.put<quint64>(e.memsz);
    }

    // ELF header copy and program header table.
    const quint64 elfOffset = w.tell();
    w.write(ElfImage::serializeHeader(eh));
    for (const ProgramHeader& ph : img.programHeaders()) {
        w.seek(elfOffset + eh.phoff + quint64(ph.index) * eh.phentsize);
        w.write(ElfImage::serializePhdr(ph));
    }
    w.seek(elfOffset + elfHeaderSize);

    // Extended info: paid, ptype, app version, fw version, SHA-256 of the ELF.
    w.put<quint64>(params.paid);
    w.put<quint64>(params.ptype);
    w.put<quint64>(params.appVersion);
    w.put<quint64>(params.fwVersion);
    w.write(QCryptographicHash::hash(elfData, QCryptographicHash::Sha256));

    // NPDRM control block: type 3, then zeros (content id, random pad).
    w.put<quint16>(self::kNpdrmControlType);
    w.zeros(self::kNpdrmBlockSize - 2);

    // Meta blocks (zero), meta footer, empty signature.
    w.zeros(qsizetype(numEntries * self::kMetaBlockSize));
    w.zeros(0x30);
    w.put<quint32>(self::kMetaFooterUnk1);
    w.zeros(self::kMetaFooterSize - 0x34);
    w.zeros(self::kSignatureSize);

    // Segment data (meta entries hold empty digests).
    for (const Entry& e : entries) {
        w.seek(e.offset);
        if (e.isMeta)
            w.zeros(qsizetype(e.filesz));
        else
            w.write(e.data);
    }
    if (w.tell() < fileSize) {
        w.seek(fileSize - 1);
        w.zeros(1);
    }
    if (haveVersion)
        w.write(versionData);
    return w.take();
}

} // namespace fw::backport
