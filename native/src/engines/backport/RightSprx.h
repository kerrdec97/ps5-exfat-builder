#pragma once

// Standard right.sprx substitute (docs/BACKPORT_PARITY.md matrix #35).
// Ported 2026-11-04 from the owner's Backport engine (make_baseline.py,
// RIGHT_SPRX_STANDARD_SUBSTITUTE, release 0.3.0; issues #2/#7/#8).
//
// Some dumps carry a sce_sys/about/right.sprx (Sony's generic legal/"about"
// module) that cannot be converted: a 12752-byte variant whose param magic
// reads as garbage (0x79A6EB9E), most likely still encrypted (MGS4, 007 First
// Light, Planet of Lana). The same standard file is in 107 of the owner's 379
// original dumps, and every delivered working package ships it converted.
// When the dump's copy cannot be converted, the standard copy takes its place
// and is converted the normal way (unwrap, SDK patch, fake-sign, V1-V3); the
// dump's copy is kept in the originals ZIP and Restore puts it back.
//
// The standard file is Sony's, so it is never bundled or downloaded: the user
// points backport.standardRightSprx at their own copy, and only the file with
// the pinned SHA-256 is accepted. Whether a console accepts a substituted
// build has no recorded hardware result.

#include <QString>

namespace fw::backport {

inline constexpr const char* kRightSprxRel = "sce_sys/about/right.sprx";
inline constexpr const char* kStandardRightSprxSha256 =
    "81544cbbc69f844f58bbff2cfaf8ad3ccce7dddaf58604cc359ced19d19629d0";
inline constexpr const char* kRightSprxFamily = "RIGHT_SPRX_STANDARD_SUBSTITUTE";

// True for the game-root sce_sys/about/right.sprx (letter case ignored).
bool isRightSprx(const QString& rel);

// Empty when `path` is a readable file whose SHA-256 is `expectedSha256`
// (the pinned value unless a test passes its own); otherwise why it is not
// the standard right.sprx (names the file).
QString standardRightSprxProblem(const QString& path,
                                 const QString& expectedSha256 = QString::fromLatin1(kStandardRightSprxSha256));

} // namespace fw::backport
