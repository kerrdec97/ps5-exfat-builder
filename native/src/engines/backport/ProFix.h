#pragma once

// PS5 Pro fix, shader-assert part (docs/BACKPORT_PARITY.md matrix #32).
// Ported 2026-11-04 from the owner's Backport engine (families_v2.py
// SHADER_ASSERT_TRAP_NOP_FW_LT12, auto_complete.pro_fix_options).
//
// SDK-embedded GPU shader asserts have the form "s_cbranch_<cond> +1;
// s_trap 1". The Quality60 fix set of Assassin's Creed Black Flag Resynced
// V16 (WORKING on base FW 4.50/9.00 and Pro FW 9.05/10.01/13.60, owner,
// 2026-10-16) turns each trap into "s_nop 0". A site is found by its bytes
// alone, without a disassembler: in every PT_LOAD with file bytes, the dword
// 01 00 92 BF (s_trap 1) at a 4-aligned virtual address whose preceding
// dword has low 16 bits 0x0001 (skips exactly one instruction) and high 16
// bits 0xBF84..0xBF89 (s_cbranch_scc0/scc1/vccz/vccnz/execz/execnz).
// Unguarded traps, s_branch (0xBF82) and other skip counts are not sites.
// The same algorithm finds 164 sites in Mortal Shell II V3 and 137 in Little
// Nightmares III V4 (the owner's engine reproduces both).
//
// Only the root eboot.bin is patched, and only when the user turns the
// option on (never automatically): the fix is confirmed on one title, and on
// its own it did not clear a base FW 9.00 Unreal GPU hang (SDK 5 did). The
// Backport GUI's second Pro fix part, the Quality60 AGCDriver scratch-ring
// hook, is a third-party build of a Sony library and is not included.

#include "core/Result.h"

#include <QByteArray>
#include <QByteArrayView>

#include <vector>

namespace fw::backport {

inline constexpr const char* kShaderTrapFamily = "SHADER_ASSERT_TRAP_NOP_FW_LT12";
inline constexpr quint32 kShaderTrap = 0xBF920001u; // s_trap 1 (bytes 01 00 92 BF)
inline constexpr quint32 kShaderNop = 0xBF800000u;  // s_nop 0 (bytes 00 00 80 BF)

struct ShaderTrapSite {
    quint64 va = 0;
    quint64 fileOffset = 0;
};

// True when `guard` is "s_cbranch_<scc0|scc1|vccz|vccnz|execz|execnz> +1".
bool isShaderTrapGuard(quint32 guard);

// Every site of a decoded ELF64, sorted by file offset, one per file offset.
// Errors: ElfInvalid / SignUnsupported when `elf` is not a little-endian
// ELF64 whose program headers can be read.
Result<std::vector<ShaderTrapSite>> findShaderTrapSites(QByteArrayView elf);

// Replaces the trap of every site with s_nop 0, checking each pre-image
// before it changes. Returns the number of sites patched; 0 when the traps
// are already no-ops (idempotent).
Result<int> applyShaderTrapNops(QByteArray& elf);

} // namespace fw::backport
