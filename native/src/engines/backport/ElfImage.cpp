#include "engines/backport/ElfImage.h"

#include "engines/backport/Common.h"

#include <QtEndian>

#include <cstring>

namespace fw::backport {

namespace elf {
bool hasElfMagic(QByteArrayView data)
{
    return data.size() >= 4 && quint8(data[0]) == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F';
}
} // namespace elf

namespace {
template <typename T>
T rd(QByteArrayView d, quint64 off)
{
    return qFromLittleEndian<T>(d.data() + off);
}
} // namespace

ElfHeader ElfImage::readHeader(QByteArrayView d)
{
    ElfHeader h;
    std::memcpy(h.ident, d.data(), 16);
    h.type = rd<quint16>(d, 0x10);
    h.machine = rd<quint16>(d, 0x12);
    h.version = rd<quint32>(d, 0x14);
    h.entry = rd<quint64>(d, 0x18);
    h.phoff = rd<quint64>(d, 0x20);
    h.shoff = rd<quint64>(d, 0x28);
    h.flags = rd<quint32>(d, 0x30);
    h.ehsize = rd<quint16>(d, 0x34);
    h.phentsize = rd<quint16>(d, 0x36);
    h.phnum = rd<quint16>(d, 0x38);
    h.shentsize = rd<quint16>(d, 0x3A);
    h.shnum = rd<quint16>(d, 0x3C);
    h.shstrndx = rd<quint16>(d, 0x3E);
    return h;
}

ProgramHeader ElfImage::readPhdr(QByteArrayView d, quint64 off, int index)
{
    ProgramHeader p;
    p.index = index;
    p.type = rd<quint32>(d, off + 0x00);
    p.flags = rd<quint32>(d, off + 0x04);
    p.offset = rd<quint64>(d, off + 0x08);
    p.vaddr = rd<quint64>(d, off + 0x10);
    p.paddr = rd<quint64>(d, off + 0x18);
    p.filesz = rd<quint64>(d, off + 0x20);
    p.memsz = rd<quint64>(d, off + 0x28);
    p.align = rd<quint64>(d, off + 0x30);
    return p;
}

bool ElfImage::contains(quint64 offset, quint64 length) const
{
    const quint64 size = quint64(m_size);
    return offset <= size && length <= size - offset;
}

Result<ElfImage> ElfImage::parse(QByteArrayView data, Strictness strictness)
{
    const auto invalid = [](const QString& why) { return makeError(Code::ElfInvalid, why); };
    const auto unsupported = [](const QString& why) { return makeError(Code::SignUnsupported, why); };

    if (!elf::hasElfMagic(data))
        return invalid(QStringLiteral("it is not an ELF file"));
    if (data.size() < elf::kEhdrSize)
        return invalid(QStringLiteral("the ELF header is truncated"));

    ElfImage img;
    img.m_size = data.size();
    img.m_header = readHeader(data);
    const ElfHeader& h = img.m_header;

    if (h.ident[4] != 2)
        return unsupported(QStringLiteral("it is not a 64-bit ELF (class %1)").arg(h.ident[4]));
    if (h.ident[5] != 1)
        return unsupported(QStringLiteral("it is not a little-endian ELF (data encoding %1)").arg(h.ident[5]));
    if (h.phentsize != elf::kPhdrSize)
        return strictness == Strictness::Signable
            ? unsupported(QStringLiteral("unsupported program header size 0x%1").arg(h.phentsize, 0, 16))
            : invalid(QStringLiteral("unsupported program header size 0x%1").arg(h.phentsize, 0, 16));
    if (strictness == Strictness::Signable) {
        if (h.machine != elf::EM_X86_64)
            return unsupported(QStringLiteral("it is not an x86-64 executable (e_machine 0x%1)").arg(h.machine, 0, 16));
        if (h.version != 1)
            return unsupported(QStringLiteral("unsupported ELF version %1").arg(h.version));
        if (h.shentsize != 0 && h.shentsize != elf::kShdrSize)
            return unsupported(QStringLiteral("unsupported section header size 0x%1").arg(h.shentsize, 0, 16));
        if (h.type != elf::ET_EXEC && h.type != elf::ET_SCE_EXEC && h.type != elf::ET_SCE_EXEC_ASLR
            && h.type != elf::ET_SCE_DYNAMIC)
            return unsupported(QStringLiteral("unsupported ELF type 0x%1").arg(h.type, 0, 16));
    }
    if (h.phnum == 0)
        return invalid(QStringLiteral("it has no program headers"));
    const quint64 tableBytes = quint64(h.phnum) * elf::kPhdrSize;
    if (!img.contains(h.phoff, tableBytes))
        return invalid(QStringLiteral("the program header table lies outside the file"));

    img.m_phdrs.reserve(h.phnum);
    for (int i = 0; i < h.phnum; ++i)
        img.m_phdrs.push_back(readPhdr(data, h.phoff + quint64(i) * elf::kPhdrSize, i));
    return img;
}

} // namespace fw::backport
