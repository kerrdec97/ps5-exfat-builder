#include "engines/backport/SceParam.h"

#include "engines/backport/Common.h"
#include "engines/backport/ElfImage.h"

#include <QtEndian>

namespace fw::backport {

namespace {

QString hex8(quint32 v)
{
    return QStringLiteral("0x") + QStringLiteral("%1").arg(v, 8, 16, QLatin1Char('0')).toUpper();
}

bool hasSelfMagic(QByteArrayView d)
{
    if (d.size() < 4)
        return false;
    const quint32 m = qFromLittleEndian<quint32>(d.data());
    return m == 0x1D3D154Fu || m == 0xEEF51454u;
}

} // namespace

Result<ParamScan> findParams(QByteArrayView data)
{
    if (!elf::hasElfMagic(data)) {
        if (hasSelfMagic(data))
            return makeError(Code::ElfInvalid, QStringLiteral("it is a signed file (expected an unsigned ELF)"));
        return makeError(Code::ElfInvalid, QStringLiteral("it is not an ELF file"));
    }
    FW_TRY_ASSIGN(const ElfImage img, ElfImage::parse(data, Strictness::Lenient));

    ParamScan scan;
    for (const ProgramHeader& ph : img.programHeaders()) {
        if (ph.type != elf::PT_SCE_PROCPARAM && ph.type != elf::PT_SCE_MODULE_PARAM)
            continue;
        const ParamKind kind = ph.type == elf::PT_SCE_PROCPARAM ? ParamKind::Process : ParamKind::Module;
        if (!img.contains(ph.offset, 4))
            return makeError(Code::ParamOutOfRange,
                             QStringLiteral("the param segment at 0x%1 lies outside the file").arg(ph.offset, 0, 16));
        const quint32 size = qFromLittleEndian<quint32>(data.data() + ph.offset);
        if (size == 0 && kind == ParamKind::Module) {
            // A module without a param: nothing to patch; the walk ends here.
            scan.moduleWithoutParam = true;
            break;
        }
        if (size < kMinParamSize)
            return makeError(Code::ParamSizeInvalid,
                             QStringLiteral("Unexpected param struct size 0x%1").arg(QString::number(size, 16).toUpper()));
        if (!img.contains(ph.offset, kMinParamSize) || ph.filesz < kMinParamSize)
            return makeError(Code::ParamOutOfRange,
                             QStringLiteral("the param at 0x%1 is cut short (segment 0x%2 bytes)")
                                 .arg(ph.offset, 0, 16)
                                 .arg(ph.filesz, 0, 16));
        const quint32 magic = qFromLittleEndian<quint32>(data.data() + ph.offset + 0x08);
        const quint32 expected = kind == ParamKind::Process ? kProcParamMagic : kModuleParamMagic;
        if (magic != expected)
            return makeError(Code::ParamMagicInvalid, QStringLiteral("Invalid param magic %1").arg(hex8(magic)));
        ParamInfo info;
        info.phdrIndex = ph.index;
        info.kind = kind;
        info.offset = ph.offset;
        info.size = size;
        info.magic = magic;
        info.ps4 = qFromLittleEndian<quint32>(data.data() + ph.offset + 0x10);
        info.ps5 = qFromLittleEndian<quint32>(data.data() + ph.offset + 0x14);
        scan.params.push_back(info);
    }
    if (scan.params.empty() && !scan.moduleWithoutParam)
        return makeError(Code::ParamMissing, QStringLiteral("No process or module param segment found"));
    return scan;
}

std::optional<int> PatchReport::sdkBefore() const
{
    for (const PatchEntry& e : patched)
        if (e.kind == ParamKind::Process)
            return bcdMajor(e.oldPs5);
    if (!patched.empty())
        return bcdMajor(patched.front().oldPs5);
    return std::nullopt;
}

bool PatchReport::changed() const
{
    for (const PatchEntry& e : patched)
        if (e.oldPs4 != e.newPs4 || e.oldPs5 != e.newPs5)
            return true;
    return false;
}

Result<PatchReport> patchSdk(QByteArray& elfData, const SdkPair& pair)
{
    FW_TRY_ASSIGN(const ParamScan scan, findParams(QByteArrayView(elfData)));
    PatchReport report;
    char* base = elfData.data();
    for (const ParamInfo& p : scan.params) {
        PatchEntry e;
        e.phdrIndex = p.phdrIndex;
        e.kind = p.kind;
        e.oldPs4 = p.ps4;
        e.oldPs5 = p.ps5;
        e.newPs4 = pair.ps4;
        e.newPs5 = pair.ps5;
        qToLittleEndian<quint32>(pair.ps5, base + p.offset + 0x14);
        qToLittleEndian<quint32>(pair.ps4, base + p.offset + 0x10);
        report.messages << QStringLiteral("PS5 SDK version %1 -> %2, PS4 version %3 -> %4")
                               .arg(hex8(e.oldPs5), hex8(e.newPs5), hex8(e.oldPs4), hex8(e.newPs4));
        report.patched.push_back(e);
    }
    if (scan.moduleWithoutParam) {
        report.skippedNoParam = true;
        report.messages << QStringLiteral("Module file has no param, skipping");
    }
    return report;
}

std::optional<int> elfSdkMajor(QByteArrayView data, quint32* ps5Out, quint32* ps4Out)
{
    auto scan = findParams(data);
    if (!scan.ok() || scan->params.empty())
        return std::nullopt;
    const ParamInfo* chosen = &scan->params.front();
    for (const ParamInfo& p : scan->params)
        if (p.kind == ParamKind::Process) {
            chosen = &p;
            break;
        }
    if (ps5Out)
        *ps5Out = chosen->ps5;
    if (ps4Out)
        *ps4Out = chosen->ps4;
    return bcdMajor(chosen->ps5);
}

} // namespace fw::backport
