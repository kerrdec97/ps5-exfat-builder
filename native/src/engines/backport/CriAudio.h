#pragma once

// CRI Unity audio fix: the 12-channel speaker rack becomes 8 channels
// (docs/BACKPORT_PARITY.md matrix #40). Ported 2026-11-04 from the owner's
// Backport engine (families_v2.cri_audio_rack, plan label
// "MODERN_AUDIO_CHANNEL_LAYOUT (CRI Unity rack 12ch -> 8ch)", release 0.3.1,
// issues #10/#11).
//
// The CRI Atom 2.27/2.28 Unity plugin (usually Media/Plugins/
// cri_ware_unity.prx) picks a 12-channel (7.1.4) rack that AudioOut2 on
// FW 4.5x cannot open, so the game is silent. The rack is set up by
//     c7 45 ?? 00 0c 00 00   mov dword [rbp+d8], 0xC00
//     c7 ?? 0c 00 00 00      mov dword [r/m], 12
//     6a 0c 58               push 12; pop rax
// and the plugin has the same sequence for its 8-channel rack (0x800, 8, 8)
// just before it. The fix changes the three immediates of the 12-channel one
// to the 8-channel values (3 bytes); the native AudioOut2 calls are kept.
// Hardware: PAC-MAN WORLD 2 Re-PAC PPSA18189 V8 (owner-confirmed), STORY OF
// SEASONS: Grand Bazaar PPSA22715 and WONDERBOY Asha in Monster World
// PPSA27841 (audio restored on a base PS5 at FW 4.50, 2026-10-18). The audio
// stays 7.1 on newer firmware as well.
//
// Applied only when every rule holds (otherwise nothing changes):
//  1. the decoded file contains the ASCII text "CRI Atom";
//  2. in the executable PT_LOAD segments there is exactly one 12-channel
//     sequence (non-overlapping matches, left to right);
//  3. an 8-channel sequence starts 1..0x40 bytes before it.
// No 12-channel sequence but two 8-channel ones within 0x40 bytes means the
// file is already fixed. Anything else is "unknown": reported, not patched.

#include "core/Result.h"

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

namespace fw::backport {

inline constexpr const char* kCriRackFamily = "MODERN_AUDIO_CHANNEL_LAYOUT (CRI Unity rack 12ch -> 8ch)";

enum class CriRackState { NotApplicable, Patch, AlreadyFixed, Unknown };

struct CriRackAnalysis {
    CriRackState state = CriRackState::NotApplicable;
    quint64 va = 0;          // the 12-channel sequence (Patch)
    quint64 fileOffset = 0;
    QString why;             // Unknown: what did not match
};

// True when `bytes` (16) is the 12-channel (channels = 12) or 8-channel
// (channels = 8) rack sequence; the two wildcard bytes are ignored.
bool isCriRack(QByteArrayView bytes, int channels);

// Read-only. Errors: ElfInvalid / SignUnsupported for a file that is not a
// little-endian ELF64 with readable program headers.
Result<CriRackAnalysis> analyzeCriRack(QByteArrayView elf);

// Applies the fix when analyzeCriRack() says Patch (the three bytes are
// checked again before they change); returns the analysis either way.
Result<CriRackAnalysis> applyCriRackFix(QByteArray& elf);

} // namespace fw::backport
