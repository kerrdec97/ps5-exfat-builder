#pragma once

// Synthetic ELF/SELF fixtures from docs/BACKPORT_PARITY.md §6.5, built from
// the format tables in code. No game data, no Sony binaries.

#include "engines/backport/SelfFormat.h"
#include "engines/backport/SelfWriter.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QtEndian>

#include <cstring>
#include <initializer_list>

namespace fwtest::bp {

struct Ph {
    quint32 type, flags;
    quint64 offset, vaddr, paddr, filesz, memsz, align;
};

inline void put16(QByteArray& b, qsizetype off, quint16 v) { qToLittleEndian<quint16>(v, b.data() + off); }
inline void put32(QByteArray& b, qsizetype off, quint32 v) { qToLittleEndian<quint32>(v, b.data() + off); }
inline void put64(QByteArray& b, qsizetype off, quint64 v) { qToLittleEndian<quint64>(v, b.data() + off); }
inline quint32 get32(const QByteArray& b, qsizetype off) { return qFromLittleEndian<quint32>(b.constData() + off); }
inline quint64 get64(const QByteArray& b, qsizetype off) { return qFromLittleEndian<quint64>(b.constData() + off); }

inline QByteArray sha(const QByteArray& d)
{
    return QCryptographicHash::hash(d, QCryptographicHash::Sha256).toHex();
}

inline void writeHeader(QByteArray& b, quint16 type, quint64 entry, quint16 phnum, quint64 phoff = 0x40)
{
    const char ident[7] = {0x7F, 'E', 'L', 'F', 2, 1, 1};
    std::memcpy(b.data(), ident, 7);
    put16(b, 0x10, type);
    put16(b, 0x12, 0x3E);
    put32(b, 0x14, 1);
    put64(b, 0x18, entry);
    put64(b, 0x20, phoff);
    put64(b, 0x28, 0);
    put32(b, 0x30, 0);
    put16(b, 0x34, 0x40);
    put16(b, 0x36, 0x38);
    put16(b, 0x38, phnum);
    put16(b, 0x3A, 0x40);
    put16(b, 0x3C, 0);
    put16(b, 0x3E, 0);
}

inline void writePhdrs(QByteArray& b, std::initializer_list<Ph> phs, quint64 phoff = 0x40)
{
    qsizetype o = qsizetype(phoff);
    for (const Ph& p : phs) {
        put32(b, o + 0x00, p.type);
        put32(b, o + 0x04, p.flags);
        put64(b, o + 0x08, p.offset);
        put64(b, o + 0x10, p.vaddr);
        put64(b, o + 0x18, p.paddr);
        put64(b, o + 0x20, p.filesz);
        put64(b, o + 0x28, p.memsz);
        put64(b, o + 0x30, p.align);
        o += 0x38;
    }
}

// F1: executable, 0x5A20 bytes, PROCPARAM inside the second PT_LOAD.
// `phoff` != 0x40 gives F10 (header padded, phdrs further out).
inline QByteArray f1(quint64 phoff = 0x40, quint32 ps5 = 0x07000038, quint32 ps4 = 0x09590001)
{
    QByteArray b(0x5A20, '\0');
    writeHeader(b, 0xFE10, 0x1000, 5, phoff);
    writePhdrs(b,
               {{1, 5, 0x1000, 0, 0, 0x4800, 0x4800, 0x4000},
                {1, 6, 0x5800, 0x8000, 0x8000, 0x100, 0x100, 0x4000},
                {0x61000001, 4, 0x5800, 0x8000, 0x8000, 0x60, 0x60, 8},
                {0x61000000, 4, 0x5900, 0, 0, 0x100, 0x100, 0x10},
                {0x6FFFFF01, 4, 0x5A00, 0, 0, 0x20, 0x20, 1}},
               phoff);
    for (qsizetype o = 0x1000; o < 0x5800; ++o)
        b[o] = char((o * 7) & 0xFF);
    put64(b, 0x5800, 0x60);
    put32(b, 0x5808, 0x4942524F);
    put32(b, 0x580C, 1);
    put32(b, 0x5810, ps4);
    put32(b, 0x5814, ps5);
    for (qsizetype o = 0x5900; o < 0x5A00; ++o)
        b[o] = char(0xAB);
    std::memcpy(b.data() + 0x5A00, "VERSIONDATA-0123456789abcdefXYZ!", 32);
    return b;
}

// F2: PRX, 0x3200 bytes, MODULE_PARAM, libc NID pattern in DYNLIBDATA.
// `seed` changes the code bytes (distinct fakelib libraries).
inline QByteArray f2(int seed = 1, quint32 ps5 = 0x08000041, quint32 ps4 = 0x10590001)
{
    QByteArray b(0x3200, '\0');
    writeHeader(b, 0xFE18, 0, 4);
    writePhdrs(b, {{1, 5, 0x1000, 0, 0, 0x1000, 0x1000, 0x4000},
                   {1, 6, 0x2000, 0x4000, 0x4000, 0x80, 0x80, 0x4000},
                   {0x61000002, 4, 0x2000, 0x4000, 0x4000, 0x40, 0x40, 8},
                   {0x61000000, 4, 0x3000, 0, 0, 0x200, 0x200, 0x10}});
    for (qsizetype o = 0x1000; o < 0x2000; ++o)
        b[o] = char((o * 13 + seed) & 0xFF);
    put64(b, 0x2000, 0x40);
    put32(b, 0x2008, 0x3C13F4BF);
    put32(b, 0x200C, 1);
    put32(b, 0x2010, ps4);
    put32(b, 0x2014, ps5);
    const char dyn[] = "printf#A#B\0" "4h6F1LLbTiw#A#B\0" "malloc#A#B";
    std::memcpy(b.data() + 0x3000, dyn, sizeof dyn); // includes the final NUL
    return b;
}

// F3: F2 whose module param size is 0 ("no param, skipping").
inline QByteArray f3()
{
    QByteArray b = f2();
    put64(b, 0x2000, 0);
    return b;
}
// F4: F1 with an invalid magic.
inline QByteArray f4()
{
    QByteArray b = f1();
    put32(b, 0x5808, 0xDEADBEEF);
    return b;
}
// F5: F1 with param size 0x10.
inline QByteArray f5()
{
    QByteArray b = f1();
    put64(b, 0x5800, 0x10);
    return b;
}
// F6: F1 whose PROCPARAM phdr became PT_TLS.
inline QByteArray f6()
{
    QByteArray b = f1();
    put32(b, 0x40 + 2 * 0x38, 0x7);
    return b;
}
// F13: F1 with a second PROCPARAM at another offset (inside the first LOAD).
inline QByteArray f13()
{
    QByteArray b = f1();
    // Turn the version phdr into a second PROCPARAM at 0x1100.
    const qsizetype ph = 0x40 + 4 * 0x38;
    put32(b, ph + 0x00, 0x61000001);
    put64(b, ph + 0x08, 0x1100);
    put64(b, ph + 0x20, 0x60);
    put64(b, ph + 0x28, 0x60);
    put64(b, 0x1100, 0x60);
    put32(b, 0x1108, 0x4942524F);
    put32(b, 0x110C, 1);
    put32(b, 0x1110, 0x09590001);
    put32(b, 0x1114, 0x07000038);
    return b;
}

inline QByteArray selfOf(const QByteArray& elf)
{
    auto r = fw::backport::makeFakeSelf(elf);
    return r.ok() ? r.value() : QByteArray();
}

// Sets `bits` on every data entry of a SELF; XORs data with `xorKey` when non-zero.
inline QByteArray markDataEntries(QByteArray s, quint64 bits, quint8 xorKey)
{
    const int n = qFromLittleEndian<quint16>(s.constData() + 0x18);
    for (int i = 0; i < n; ++i) {
        const qsizetype o = 0x20 + qsizetype(i) * 0x20;
        quint64 props = get64(s, o);
        if (!(props & fw::backport::self::props::kHasBlocks))
            continue;
        put64(s, o, props | bits);
        if (xorKey) {
            const quint64 off = get64(s, o + 8), len = get64(s, o + 16);
            for (quint64 k = 0; k < len; ++k)
                s[qsizetype(off + k)] = char(quint8(s[qsizetype(off + k)]) ^ xorKey);
        }
    }
    return s;
}
// F7: encrypted SELF(F2).
inline QByteArray f7() { return markDataEntries(selfOf(f2()), fw::backport::self::props::kEncrypted, 0x5A); }
// F8: compressed SELF(F2).
inline QByteArray f8() { return markDataEntries(selfOf(f2()), fw::backport::self::props::kCompressed, 0); }
// F9: SELF(F1) with the PS5 magic.
inline QByteArray f9()
{
    QByteArray s = selfOf(f1());
    const char m[4] = {0x54, 0x14, char(0xF5), char(0xEE)};
    std::memcpy(s.data(), m, 4);
    return s;
}

inline bool writeBytes(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return f.write(data) == data.size();
}

inline QByteArray readBytes(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

// A fakelib library: a valid PRX (F2 layout) with distinct code bytes.
inline QByteArray fakeLib(int seed) { return f2(seed); }

// F1 with a sixth program header, PT_DYNAMIC, inside PT_SCE_DYNLIBDATA
// (0x5900): DT_SCE_STRTAB / DT_SCE_STRSZ, one DT_NEEDED per `needed` name and
// one DT_SCE_NEEDED_MODULE per `modules` name, then DT_NULL; the string table
// follows the dynamic table inside DYNLIBDATA (#41 fixtures).
inline QByteArray f1WithImports(const QStringList& needed, const QStringList& modules)
{
    QByteArray b = f1();
    put16(b, 0x38, 6);
    const quint64 entries = quint64(needed.size() + modules.size() + 3);
    const quint64 dynSize = entries * 16;
    writePhdrs(b, {{0x2, 4, 0x5900, 0, 0, dynSize, dynSize, 8}}, 0x40 + 5 * 0x38);
    for (qsizetype o = 0x5900; o < 0x5A00; ++o)
        b[o] = '\0';
    QByteArray strtab(1, '\0');
    QList<quint64> neededOff, moduleOff;
    for (const QString& n : needed) {
        neededOff << quint64(strtab.size());
        strtab += n.toLatin1() + '\0';
    }
    for (const QString& n : modules) {
        moduleOff << quint64(strtab.size());
        strtab += n.toLatin1() + '\0';
    }
    Q_ASSERT(dynSize + quint64(strtab.size()) <= 0x100);
    qsizetype o = 0x5900;
    const auto entry = [&](qint64 tag, quint64 val) {
        put64(b, o, quint64(tag));
        put64(b, o + 8, val);
        o += 16;
    };
    entry(0x61000035, dynSize);              // DT_SCE_STRTAB (offset in DYNLIBDATA)
    entry(0x61000037, quint64(strtab.size())); // DT_SCE_STRSZ
    for (quint64 off : neededOff)
        entry(1, off);                       // DT_NEEDED
    for (int i = 0; i < moduleOff.size(); ++i)
        entry(0x6100000F, (quint64(i + 1) << 48) | (quint64(1) << 32) | moduleOff[i]); // DT_SCE_NEEDED_MODULE
    entry(0, 0);                             // DT_NULL
    std::memcpy(b.data() + 0x5900 + dynSize, strtab.constData(), size_t(strtab.size()));
    return b;
}

} // namespace fwtest::bp
