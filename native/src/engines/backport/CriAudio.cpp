#include "engines/backport/CriAudio.h"

#include "engines/backport/Common.h"
#include "engines/backport/ElfImage.h"

#include <vector>

namespace fw::backport {

namespace {

constexpr int kRackLen = 16;
constexpr quint32 kPfExec = 0x1;
constexpr quint64 kMaxGap = 0x40;
constexpr int kChannelBytes[3] = {4, 9, 14};

struct Hit {
    quint64 va = 0;
    quint64 off = 0;
};

// Non-overlapping matches, left to right, inside one segment's file bytes.
void collect(QByteArrayView data, const ProgramHeader& ph, int channels, std::vector<Hit>& out)
{
    quint64 j = 0;
    while (j + kRackLen <= ph.filesz) {
        if (isCriRack(data.sliced(qsizetype(ph.offset + j), kRackLen), channels)) {
            out.push_back({ph.vaddr + j, ph.offset + j});
            j += kRackLen;
        } else {
            ++j;
        }
    }
}

} // namespace

bool isCriRack(QByteArrayView b, int channels)
{
    if (b.size() < kRackLen)
        return false;
    const auto u = [&b](int i) { return quint8(b[i]); };
    const quint8 n = quint8(channels == 12 ? 0x0C : 0x08);
    return u(0) == 0xC7 && u(1) == 0x45 && u(3) == 0x00 && u(4) == n && u(5) == 0x00 && u(6) == 0x00 // [2] any
        && u(7) == 0xC7 && u(9) == n && u(10) == 0x00 && u(11) == 0x00 && u(12) == 0x00                // [8] any
        && u(13) == 0x6A && u(14) == n && u(15) == 0x58;
}

Result<CriRackAnalysis> analyzeCriRack(QByteArrayView data)
{
    CriRackAnalysis a;
    if (!data.contains(QByteArrayView("CRI Atom")))
        return a; // not a CRI Atom module: nothing to report
    FW_TRY_ASSIGN(const ElfImage img, ElfImage::parse(data, Strictness::Lenient));
    std::vector<Hit> twelve, eight;
    for (const ProgramHeader& ph : img.programHeaders()) {
        if (ph.type != elf::PT_LOAD || !(ph.flags & kPfExec) || ph.filesz == 0)
            continue;
        if (!img.contains(ph.offset, ph.filesz))
            return makeError(Code::ElfInvalid, QStringLiteral("segment %1 lies outside the file").arg(ph.index));
        collect(data, ph, 12, twelve);
        collect(data, ph, 8, eight);
    }
    if (twelve.empty()) {
        for (const Hit& x : eight)
            for (const Hit& y : eight)
                if (y.va > x.va && y.va - x.va <= kMaxGap)
                    a.state = CriRackState::AlreadyFixed;
        return a;
    }
    if (twelve.size() != 1) {
        a.state = CriRackState::Unknown;
        a.why = QStringLiteral("%1 12-channel CRI racks (expected 1)").arg(twelve.size());
        return a;
    }
    const Hit& t = twelve.front();
    bool adjacent = false;
    for (const Hit& e : eight)
        if (t.va > e.va && t.va - e.va <= kMaxGap)
            adjacent = true;
    if (!adjacent) {
        a.state = CriRackState::Unknown;
        a.why = QStringLiteral("12-channel CRI rack at 0x%1 without the adjacent 8-channel rack").arg(t.va, 0, 16);
        return a;
    }
    a.state = CriRackState::Patch;
    a.va = t.va;
    a.fileOffset = t.off;
    return a;
}

Result<CriRackAnalysis> applyCriRackFix(QByteArray& elf)
{
    FW_TRY_ASSIGN(const CriRackAnalysis a, analyzeCriRack(elf));
    if (a.state != CriRackState::Patch)
        return a;
    if (!isCriRack(QByteArrayView(elf).sliced(qsizetype(a.fileOffset), kRackLen), 12))
        return makeError(Code::Internal, QStringLiteral("the CRI rack at 0x%1 changed before it was patched").arg(a.va, 0, 16));
    for (int i : kChannelBytes)
        elf[qsizetype(a.fileOffset) + i] = char(0x08);
    return a;
}

} // namespace fw::backport
