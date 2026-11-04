// SELF reader / unwrapper. Derivative of make_fself.py by john-tornblom
// (ps5-payload-dev/sdk, GNU GPL version 3) and decrypt_fself.py by Nazky.
// Distributed under the GNU GPL version 3. See SelfReader.h.

#include "engines/backport/SelfReader.h"

#include "engines/backport/Common.h"
#include "engines/backport/SelfFormat.h"

#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace fw::backport {

QString selfKindName(SelfKind kind)
{
    switch (kind) {
    case SelfKind::FakeSigned: return QStringLiteral("fake-signed");
    case SelfKind::Encrypted: return QStringLiteral("encrypted");
    case SelfKind::Compressed: return QStringLiteral("compressed");
    case SelfKind::Malformed: return QStringLiteral("malformed");
    }
    return QStringLiteral("malformed");
}

bool SelfEntry::isData() const { return (props & self::props::kHasBlocks) != 0; }
bool SelfEntry::isEncrypted() const { return (props & self::props::kEncrypted) != 0; }
bool SelfEntry::isCompressed() const { return (props & self::props::kCompressed) != 0; }
int SelfEntry::segmentIndex() const { return self::props::segmentIndex(props); }

namespace selfio {

namespace {
template <typename T>
T rd(QByteArrayView d, quint64 off)
{
    return qFromLittleEndian<T>(d.data() + off);
}

bool within(quint64 size, quint64 off, quint64 len)
{
    return off <= size && len <= size - off;
}
} // namespace

bool hasSelfMagic(QByteArrayView data)
{
    if (data.size() < 4)
        return false;
    const quint32 m = rd<quint32>(data, 0);
    return m == self::kMagicFake || m == self::kMagicPs5;
}

Result<SelfInfo> inspect(QByteArrayView data)
{
    if (!hasSelfMagic(data))
        return makeError(Code::SelfMalformed, QStringLiteral("it is not a SELF file"));
    if (data.size() < self::kCommonHeaderSize)
        return makeError(Code::SelfMalformed, QStringLiteral("the SELF header is truncated"));

    const quint64 size = quint64(data.size());
    SelfInfo info;
    info.magic = rd<quint32>(data, 0x00);
    info.version = quint8(data[4]);
    info.mode = quint8(data[5]);
    info.endian = quint8(data[6]);
    info.attributes = quint8(data[7]);
    info.keyType = rd<quint32>(data, 0x08);
    info.headerSize = rd<quint16>(data, 0x0C);
    info.metaSize = rd<quint16>(data, 0x0E);
    info.fileSize = rd<quint64>(data, 0x10);
    info.numEntries = rd<quint16>(data, 0x18);
    info.flags = rd<quint16>(data, 0x1A);

    const quint64 tableEnd = self::kCommonHeaderSize + quint64(info.numEntries) * self::kEntrySize;
    if (tableEnd > size) {
        info.kind = SelfKind::Malformed;
        info.problem = QStringLiteral("the SELF entry table is cut short");
        return info;
    }
    for (int i = 0; i < info.numEntries; ++i) {
        const quint64 o = self::kCommonHeaderSize + quint64(i) * self::kEntrySize;
        SelfEntry e;
        e.index = i;
        e.props = rd<quint64>(data, o);
        e.offset = rd<quint64>(data, o + 8);
        e.filesz = rd<quint64>(data, o + 16);
        e.memsz = rd<quint64>(data, o + 24);
        info.entries.push_back(e);
    }

    // The ELF header copy follows the entry table (16-byte aligned).
    info.elfOffset = self::alignUp(tableEnd, 16);
    if (within(size, info.elfOffset, elf::kEhdrSize)) {
        auto img = ElfImage::parse(data.sliced(qsizetype(info.elfOffset)), Strictness::Signable);
        if (img.ok()) {
            info.elfValid = true;
            info.elf = img->header();
            info.phdrs = img->programHeaders();
            const quint64 hsz = self::alignUp(
                std::max<quint64>(info.elf.ehsize, info.elf.phoff + quint64(info.elf.phentsize) * info.elf.phnum), 16);
            const quint64 ex = info.elfOffset + hsz;
            if (within(size, ex, self::kExInfoSize)) {
                info.paid = rd<quint64>(data, ex);
                info.ptype = rd<quint64>(data, ex + 8);
                info.appVersion = rd<quint64>(data, ex + 16);
                info.fwVersion = rd<quint64>(data, ex + 24);
                info.digest = data.sliced(qsizetype(ex + 32), self::kDigestSize).toByteArray();
            }
        } else if (info.problem.isEmpty()) {
            info.problem = QStringLiteral("its ELF header copy is not valid (%1)").arg(img.error().message);
        }
    } else {
        info.problem = QStringLiteral("the ELF header copy is cut short");
    }

    // Encryption and compression first: they mean the dump is not usable
    // whatever the rest of the header says (D2, S32).
    int dataEntries = 0;
    bool encrypted = false, compressed = false;
    for (const SelfEntry& e : info.entries) {
        if (!e.isData())
            continue;
        ++dataEntries;
        encrypted = encrypted || e.isEncrypted();
        compressed = compressed || e.isCompressed();
    }
    if (encrypted) {
        info.kind = SelfKind::Encrypted;
        info.problem = QStringLiteral("its segments are encrypted (the dump may still be encrypted)");
        return info;
    }
    if (compressed) {
        info.kind = SelfKind::Compressed;
        info.problem = QStringLiteral("its segments are compressed, which this tool does not unpack");
        return info;
    }
    if (!info.elfValid) {
        info.kind = SelfKind::Malformed;
        return info;
    }
    if (dataEntries == 0) {
        info.kind = SelfKind::Malformed;
        info.problem = QStringLiteral("it has no data segments");
        return info;
    }
    for (const SelfEntry& e : info.entries) {
        if (!e.isData())
            continue;
        if (!within(size, e.offset, e.filesz)) {
            info.kind = SelfKind::Malformed;
            info.problem = QStringLiteral("segment entry %1 lies outside the file").arg(e.index);
            return info;
        }
        const int si = e.segmentIndex();
        if (si < int(info.phdrs.size()) && e.filesz > info.phdrs[size_t(si)].filesz) {
            info.kind = SelfKind::Malformed;
            info.problem = QStringLiteral("segment entry %1 is larger than its program header").arg(e.index);
            return info;
        }
    }
    // Every program header the unwrap may place must describe a range inside
    // an executable this tool can hold (overflow-safe; kMaxExecBytes is the
    // same limit readFileBytes() applies), so unwrap() never allocates or
    // writes outside a sane buffer.
    for (const ProgramHeader& ph : info.phdrs) {
        if (!within(quint64(kMaxExecBytes), ph.offset, ph.filesz)) {
            info.kind = SelfKind::Malformed;
            info.problem = QStringLiteral("program header %1 points outside any executable this tool can rewrite "
                                          "(offset 0x%2, size 0x%3)")
                               .arg(ph.index)
                               .arg(ph.offset, 0, 16)
                               .arg(ph.filesz, 0, 16);
            return info;
        }
    }
    info.kind = SelfKind::FakeSigned;
    info.problem.clear();
    return info;
}

Result<QByteArray> unwrap(QByteArrayView data, QStringList* warnings)
{
    FW_TRY_ASSIGN(const SelfInfo info, inspect(data));
    switch (info.kind) {
    case SelfKind::FakeSigned: break;
    case SelfKind::Encrypted: return makeError(Code::SelfEncrypted, info.problem);
    case SelfKind::Compressed: return makeError(Code::SelfCompressed, info.problem);
    case SelfKind::Malformed: return makeError(Code::SelfMalformed, info.problem);
    }

    const quint64 size = quint64(data.size());
    QByteArray out;
    bool inBounds = true;
    // Writes `bytes` at `at`, zero-filling up to `padTo`; refuses anything past
    // kMaxExecBytes (checked without overflow) instead of writing it.
    const auto place = [&out, &inBounds](quint64 at, QByteArrayView bytes, quint64 padTo) {
        const quint64 n = quint64(bytes.size());
        if (!within(quint64(kMaxExecBytes), at, n) || padTo > quint64(kMaxExecBytes)) {
            inBounds = false;
            return;
        }
        const quint64 end = std::max<quint64>(at + n, padTo);
        if (quint64(out.size()) < end)
            out.append(QByteArray(qsizetype(end - quint64(out.size())), '\0'));
        if (n > 0)
            std::memcpy(out.data() + at, bytes.data(), size_t(n));
    };

    // ELF header at 0 and the program header table at e_phoff (P10: not
    // straight after the header).
    place(0, data.sliced(qsizetype(info.elfOffset), elf::kEhdrSize), 0);
    const quint64 tableBytes = quint64(info.elf.phnum) * elf::kPhdrSize;
    place(info.elf.phoff, data.sliced(qsizetype(info.elfOffset + info.elf.phoff), qsizetype(tableBytes)), 0);

    std::vector<SelfEntry> dataEntries;
    for (const SelfEntry& e : info.entries)
        if (e.isData())
            dataEntries.push_back(e);
    std::stable_sort(dataEntries.begin(), dataEntries.end(),
                     [](const SelfEntry& a, const SelfEntry& b) { return a.segmentIndex() < b.segmentIndex(); });

    std::vector<bool> covered(info.phdrs.size(), false);
    for (const SelfEntry& e : dataEntries) {
        const int si = e.segmentIndex();
        if (si >= int(info.phdrs.size()))
            continue; // as documented: an index past e_phnum is skipped
        const ProgramHeader& ph = info.phdrs[size_t(si)];
        covered[size_t(si)] = true;
        place(ph.offset, data.sliced(qsizetype(e.offset), qsizetype(e.filesz)), ph.offset + ph.filesz);
    }

    // PT_SCE_VERSION is not a SELF segment: its bytes follow file_size.
    for (const ProgramHeader& ph : info.phdrs) {
        if (ph.type != elf::PT_SCE_VERSION || ph.filesz == 0 || covered[size_t(ph.index)])
            continue;
        if (info.fileSize <= size && size - info.fileSize == ph.filesz) {
            place(ph.offset, data.sliced(qsizetype(info.fileSize), qsizetype(ph.filesz)), 0);
        } else if (warnings) {
            *warnings << QStringLiteral("The version segment (%1 bytes) does not match the %2 bytes after the SELF "
                                        "data; it was left out.")
                             .arg(ph.filesz)
                             .arg(info.fileSize <= size ? size - info.fileSize : 0);
        }
    }
    if (!inBounds)
        return makeError(Code::SelfMalformed, QStringLiteral("a segment lies outside any executable this tool can rewrite"));
    return out;
}

} // namespace selfio

} // namespace fw::backport
