#include "engines/backport/ProFix.h"

#include "engines/backport/Common.h"
#include "engines/backport/ElfImage.h"

#include <QtEndian>

#include <algorithm>

namespace fw::backport {

bool isShaderTrapGuard(quint32 guard)
{
    const quint32 hi = guard >> 16, lo = guard & 0xFFFFu;
    return lo == 0x0001u && hi >= 0xBF84u && hi <= 0xBF89u;
}

Result<std::vector<ShaderTrapSite>> findShaderTrapSites(QByteArrayView data)
{
    FW_TRY_ASSIGN(const ElfImage img, ElfImage::parse(data, Strictness::Lenient));
    std::vector<ShaderTrapSite> sites;
    for (const ProgramHeader& ph : img.programHeaders()) {
        if (ph.type != elf::PT_LOAD || ph.filesz == 0)
            continue;
        if (!img.contains(ph.offset, ph.filesz))
            return makeError(Code::ElfInvalid,
                             QStringLiteral("segment %1 lies outside the file").arg(ph.index));
        // j: offset inside the segment. The trap's address vaddr + j must be
        // 4-aligned and the guard dword (j - 4) must be inside the segment,
        // so j starts at the first aligned offset that is at least 4.
        const quint64 aligned = (4 - (ph.vaddr % 4)) % 4; // 0..3
        for (quint64 j = aligned + 4; j + 4 <= ph.filesz; j += 4) {
            const quint64 off = ph.offset + j;
            const quint32 dword = qFromLittleEndian<quint32>(data.data() + off);
            if (dword != kShaderTrap)
                continue;
            const quint32 guard = qFromLittleEndian<quint32>(data.data() + off - 4);
            if (!isShaderTrapGuard(guard))
                continue;
            sites.push_back({ph.vaddr + j, off});
        }
    }
    std::sort(sites.begin(), sites.end(),
              [](const ShaderTrapSite& a, const ShaderTrapSite& b) { return a.fileOffset < b.fileOffset; });
    sites.erase(std::unique(sites.begin(), sites.end(),
                            [](const ShaderTrapSite& a, const ShaderTrapSite& b) { return a.fileOffset == b.fileOffset; }),
                sites.end());
    return sites;
}

Result<int> applyShaderTrapNops(QByteArray& elf)
{
    FW_TRY_ASSIGN(const std::vector<ShaderTrapSite> sites, findShaderTrapSites(elf));
    for (const ShaderTrapSite& s : sites) {
        char* p = elf.data() + s.fileOffset;
        if (qFromLittleEndian<quint32>(p) != kShaderTrap)
            return makeError(Code::Internal, QStringLiteral("the shader trap at 0x%1 changed before it was patched")
                                                 .arg(s.va, 0, 16));
        qToLittleEndian<quint32>(kShaderNop, p);
    }
    return int(sites.size());
}

} // namespace fw::backport
