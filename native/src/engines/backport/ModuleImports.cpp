#include "engines/backport/ModuleImports.h"

#include "engines/backport/ElfImage.h"
#include "engines/backport/ExecutableScanner.h"

#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <optional>

namespace fw::backport {

namespace {

constexpr qsizetype kMaxName = 255;
constexpr quint64 kMaxDynamicEntries = 65536;

bool printableName(QByteArrayView s)
{
    if (s.size() < 2 || s.size() > kMaxName)
        return false;
    bool letter = false;
    for (char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x21 || u > 0x7E)
            return false;
        if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z'))
            letter = true;
    }
    return letter;
}

// The NUL-terminated string at `off` inside [base, base + size).
QByteArrayView stringAt(QByteArrayView data, quint64 base, quint64 size, quint64 off)
{
    if (off >= size)
        return {};
    const char* start = data.data() + base + off;
    const quint64 room = size - off;
    const void* nul = std::memchr(start, '\0', size_t(std::min<quint64>(room, kMaxName + 1)));
    if (!nul)
        return {};
    return QByteArrayView(start, static_cast<const char*>(nul) - start);
}

QString stripLibExt(const QString& name)
{
    for (const QString& ext : {QStringLiteral(".sprx"), QStringLiteral(".prx")})
        if (name.endsWith(ext, Qt::CaseInsensitive))
            return name.chopped(ext.size());
    return name;
}

} // namespace

ModuleImports readModuleImports(QByteArrayView elf)
{
    ModuleImports out;
    auto img = ElfImage::parse(elf, Strictness::Lenient);
    if (!img.ok())
        return out;
    const ProgramHeader* dynlib = nullptr;
    const ProgramHeader* dynamic = nullptr;
    for (const ProgramHeader& ph : img->programHeaders()) {
        if (ph.type == elf::PT_SCE_DYNLIBDATA && !dynlib)
            dynlib = &ph;
        else if (ph.type == elf::PT_DYNAMIC && !dynamic)
            dynamic = &ph;
    }
    if (!dynlib || dynlib->filesz == 0 || !img->contains(dynlib->offset, dynlib->filesz))
        return out;

    QStringList names;
    QSet<QString> seen;
    const auto add = [&](QByteArrayView s) {
        if (!printableName(s))
            return;
        const QString n = QString::fromLatin1(s.data(), s.size());
        if (!seen.contains(n)) {
            seen.insert(n);
            names << n;
        }
    };

    // 1. The dynamic table.
    if (dynamic && dynamic->filesz >= 16 && img->contains(dynamic->offset, dynamic->filesz)) {
        std::optional<quint64> strtab, strsz;
        std::vector<quint64> wanted;
        const quint64 count = std::min<quint64>(dynamic->filesz / 16, kMaxDynamicEntries);
        for (quint64 i = 0; i < count; ++i) {
            const char* e = elf.data() + dynamic->offset + i * 16;
            const auto tag = qFromLittleEndian<qint64>(e);
            const auto val = qFromLittleEndian<quint64>(e + 8);
            if (tag == dyn::DT_NULL)
                break;
            if (tag == dyn::DT_SCE_STRTAB)
                strtab = val;
            else if (tag == dyn::DT_SCE_STRSZ)
                strsz = val;
            else if (tag == dyn::DT_NEEDED)
                wanted.push_back(val);
            else if (tag == dyn::DT_SCE_NEEDED_MODULE)
                wanted.push_back(val & 0xFFFFFFFFu);
        }
        if (strtab && strsz && *strsz > 0 && *strtab <= dynlib->filesz && *strsz <= dynlib->filesz - *strtab) {
            const quint64 base = dynlib->offset + *strtab;
            for (quint64 off : wanted)
                add(stringAt(elf, base, *strsz, off));
        }
    }
    if (!names.isEmpty()) {
        out.names = names;
        out.fromDynamicTable = true;
        return out;
    }

    // 2. Fallback: the NUL-separated names in DYNLIBDATA (import NIDs, which
    // contain '#', are not module names).
    const QByteArrayView seg(elf.data() + dynlib->offset, qsizetype(dynlib->filesz));
    qsizetype start = 0;
    for (qsizetype i = 0; i <= seg.size(); ++i) {
        if (i == seg.size() || seg[i] == '\0') {
            const QByteArrayView tok = seg.sliced(start, i - start);
            if (!tok.contains('#'))
                add(tok);
            start = i + 1;
        }
    }
    out.names = names;
    return out;
}

std::vector<ModuleCaseMismatch> moduleCaseMismatches(const ScanResult& scan)
{
    std::vector<ModuleCaseMismatch> out;
    QStringList libs; // file names directly in sce_module/
    for (const QString& rel : scan.fileIndex) {
        const QString low = rel.toLower();
        if (!low.startsWith(QStringLiteral("sce_module/")) || low.count(u'/') != 1)
            continue;
        if (low.endsWith(QStringLiteral(".prx")) || low.endsWith(QStringLiteral(".sprx")))
            libs << rel.section(u'/', -1);
    }
    if (libs.isEmpty())
        return out;
    libs.sort();
    for (const ExecFile& e : scan.executables) {
        if (e.area != Area::Game)
            continue;
        QSet<QString> reported;
        for (const QString& name : e.importedModules) {
            const QString base = stripLibExt(name);
            if (base.isEmpty() || reported.contains(base.toLower()))
                continue;
            bool exact = false;
            QString differs;
            for (const QString& lib : std::as_const(libs)) {
                const QString lb = stripLibExt(lib);
                if (lb == base)
                    exact = true;
                else if (differs.isEmpty() && lb.compare(base, Qt::CaseInsensitive) == 0)
                    differs = lib;
            }
            if (exact || differs.isEmpty())
                continue;
            reported.insert(base.toLower());
            out.push_back({e.rel, name, QStringLiteral("sce_module/") + differs});
        }
    }
    return out;
}

} // namespace fw::backport
