#pragma once

// The SDK version in the process / module param of a PS5 executable
// (docs/BACKPORT_PARITY.md §2.5), read only. Clean-room implementation from
// the documented facts (offsets, magics, outcomes); no code from the
// unlicensed patcher was used.
//
// Param struct at p_offset of a PT_SCE_PROCPARAM / PT_SCE_MODULE_PARAM
// segment: +0x00 u64 size (low u32 read), +0x08 u32 magic, +0x10 u32 PS4
// SDK, +0x14 u32 PS5 SDK.

#include "core/Result.h"
#include "engines/backport/SdkTable.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace fw::backport {

inline constexpr quint32 kProcParamMagic = 0x4942524F;   // "ORBI"
inline constexpr quint32 kModuleParamMagic = 0x3C13F4BF;
inline constexpr quint32 kMinParamSize = 0x18;

enum class ParamKind { Process, Module };

struct ParamInfo {
    int phdrIndex = -1;
    ParamKind kind = ParamKind::Process;
    quint64 offset = 0;  // file offset of the struct
    quint32 size = 0;
    quint32 magic = 0;
    quint32 ps4 = 0;
    quint32 ps5 = 0;
};

struct ParamScan {
    std::vector<ParamInfo> params;  // every param found, in program header order
    bool moduleWithoutParam = false; // a module param of size 0 ended the walk
};

// Walks the program headers and reads each param.
// Errors: ParamMissing ("No process or module param segment found"),
// ParamSizeInvalid ("Unexpected param struct size 0x…"), ParamMagicInvalid
// ("Invalid param magic 0x…"), ParamOutOfRange, and the ElfImage errors.
Result<ParamScan> findParams(QByteArrayView elf);

// The PS5 SDK major recorded in an ELF (process param first). nullopt when
// there is no readable param or the value is not BCD.
std::optional<int> elfSdkMajor(QByteArrayView elf, quint32* ps5Out = nullptr, quint32* ps4Out = nullptr);

} // namespace fw::backport
