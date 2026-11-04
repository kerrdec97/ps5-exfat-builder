#pragma once

// Bounds-checked view of an ELF64 executable (docs/BACKPORT_PARITY.md §2.4).
// Original code. `Strictness::Signable` applies every header rule the SELF
// writer needs; `Lenient` only what reading program headers needs.

#include "core/Result.h"

#include <QByteArray>
#include <QByteArrayView>

#include <vector>

namespace fw::backport {

namespace elf {
inline constexpr quint32 PT_LOAD = 0x1;
inline constexpr quint32 PT_DYNAMIC = 0x2;
inline constexpr quint32 PT_INTERP = 0x3;
inline constexpr quint32 PT_TLS = 0x7;
inline constexpr quint32 PT_SCE_RELA = 0x60000000;
inline constexpr quint32 PT_SCE_DYNLIBDATA = 0x61000000;
inline constexpr quint32 PT_SCE_PROCPARAM = 0x61000001;
inline constexpr quint32 PT_SCE_MODULE_PARAM = 0x61000002;
inline constexpr quint32 PT_SCE_RELRO = 0x61000010;
inline constexpr quint32 PT_SCE_COMMENT = 0x6FFFFF00;
inline constexpr quint32 PT_SCE_VERSION = 0x6FFFFF01;
inline constexpr quint32 PT_GNU_EH_FRAME = 0x6474E550;
inline constexpr quint32 PT_GNU_STACK = 0x6474E551;

inline constexpr quint16 ET_EXEC = 0x2;
inline constexpr quint16 ET_SCE_EXEC = 0xFE00;
inline constexpr quint16 ET_SCE_EXEC_ASLR = 0xFE10;
inline constexpr quint16 ET_SCE_DYNAMIC = 0xFE18;
inline constexpr quint16 EM_X86_64 = 0x3E;

inline constexpr int kEhdrSize = 0x40;
inline constexpr int kPhdrSize = 0x38;
inline constexpr int kShdrSize = 0x40;

// True for the program header types that become SELF segments (§2.4).
bool isSelfSegmentType(quint32 type);
// ELF magic 7F 45 4C 46.
bool hasElfMagic(QByteArrayView data);
} // namespace elf

struct ElfHeader {
    quint8 ident[16] = {};
    quint16 type = 0;
    quint16 machine = 0;
    quint32 version = 0;
    quint64 entry = 0;
    quint64 phoff = 0;
    quint64 shoff = 0;
    quint32 flags = 0;
    quint16 ehsize = 0;
    quint16 phentsize = 0;
    quint16 phnum = 0;
    quint16 shentsize = 0;
    quint16 shnum = 0;
    quint16 shstrndx = 0;
};

struct ProgramHeader {
    int index = 0;
    quint32 type = 0;
    quint32 flags = 0;
    quint64 offset = 0;
    quint64 vaddr = 0;
    quint64 paddr = 0;
    quint64 filesz = 0;
    quint64 memsz = 0;
    quint64 align = 0;
};

enum class Strictness { Lenient, Signable };

class ElfImage {
public:
    // Parses the header and program header table. Errors carry
    // Code::ElfInvalid (structure) or Code::SignUnsupported (a valid ELF the
    // signer cannot rewrite: class, data encoding, machine, type, entry sizes).
    static Result<ElfImage> parse(QByteArrayView data, Strictness strictness = Strictness::Signable);

    const ElfHeader& header() const { return m_header; }
    const std::vector<ProgramHeader>& programHeaders() const { return m_phdrs; }
    qint64 fileSize() const { return m_size; }

    // True when [offset, offset + length) lies inside the file.
    bool contains(quint64 offset, quint64 length) const;

    // Serialises a header the way the SELF writer stores its copy:
    // e_ident[9..14] written as zero, the other fields as given.
    static QByteArray serializeHeader(const ElfHeader& h);
    static QByteArray serializePhdr(const ProgramHeader& p);
    static ElfHeader readHeader(QByteArrayView data); // no validation; needs 0x40 bytes
    static ProgramHeader readPhdr(QByteArrayView data, quint64 offset, int index);

private:
    ElfHeader m_header;
    std::vector<ProgramHeader> m_phdrs;
    qint64 m_size = 0;
};

} // namespace fw::backport
