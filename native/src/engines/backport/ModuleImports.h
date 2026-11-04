#pragma once

// Module-name case detector (docs/BACKPORT_PARITY.md matrix #41, Backport GUI
// issue #2: a launch crash caused by an import of "Il2CppUserAssemblies"
// while the file in sce_module/ is "Il2cppUserAssemblies.prx"). Detector
// only: the module names are compared with the files in sce_module/
// case-sensitively and a mismatch is reported as a scan issue. Nothing is
// renamed or patched; the fix stays blocked (§7).
//
// The imported module names come from the executable's dynamic table, which
// lies inside PT_SCE_DYNLIBDATA. Format facts used (public ELF/SCE dynamic
// tag layout, not from any unlicensed code): 16-byte entries {i64 tag,
// u64 value} at the PT_DYNAMIC p_offset, ended by DT_NULL; DT_NEEDED (1) is
// the string-table offset of a needed file name; DT_SCE_NEEDED_MODULE
// (0x6100000F) holds the string-table offset of a needed module name in its
// low 32 bits; DT_SCE_STRTAB (0x61000035) is the string table's offset from
// the start of PT_SCE_DYNLIBDATA and DT_SCE_STRSZ (0x61000037) its size.
// That PS5 executables use these values is UNVERIFIED on real titles, so
// when the table cannot be read the NUL-separated names in DYNLIBDATA are
// used instead (strings with '#' are import NIDs and are ignored).

#include "core/Result.h"

#include <QByteArrayView>
#include <QStringList>

#include <vector>

namespace fw::backport {

struct ScanResult;

namespace dyn {
inline constexpr qint64 DT_NULL = 0;
inline constexpr qint64 DT_NEEDED = 1;
inline constexpr qint64 DT_SCE_NEEDED_MODULE = 0x6100000F;
inline constexpr qint64 DT_SCE_STRTAB = 0x61000035;
inline constexpr qint64 DT_SCE_STRSZ = 0x61000037;
} // namespace dyn

struct ModuleImports {
    QStringList names;     // needed file and module names, in table order, de-duplicated
    bool fromDynamicTable = false; // false: read from the DYNLIBDATA strings (fallback)
};

// Never fails on hostile input: an ELF without DYNLIBDATA gives an empty list.
ModuleImports readModuleImports(QByteArrayView elf);

struct ModuleCaseMismatch {
    QString importer; // game-relative executable
    QString imported; // the name as the executable spells it
    QString file;     // sce_module/<file> whose name differs only in letter case
};

// Every import of a game executable that matches a library in sce_module/
// only when letter case is ignored (".prx"/".sprx" extensions ignored), with
// no file of exactly that name.
std::vector<ModuleCaseMismatch> moduleCaseMismatches(const ScanResult& scan);

} // namespace fw::backport
