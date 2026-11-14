#pragma once

// Names and paths read from inside an image or package are untrusted: a
// crafted entry such as "..\..\Windows\evil.dll" must never let an
// extraction write outside its output folder. These helpers check entry
// names, build output paths that provably stay inside a folder, and choose the
// name an entry gets on the destination drive.

#include "core/Result.h"

#include <QString>

namespace fw {

// True when `name` is one plain path component that is safe to create on
// every host: not empty, not "." or "..", only dots and spaces neither (Windows
// drops trailing dots and spaces, so ". ." would turn into "." or ".."), no
// '/' or '\', no ':' (a drive letter or an NTFS stream on Windows) and no
// control character (U+0000-U+001F).
bool isSafeEntryName(const QString& name);

// `root` + '/' + `relative` (a '/'-separated path whose every component must
// pass isSafeEntryName()), cleaned. Fails with InvalidArgument when the result
// would not lie strictly inside `root`.
Result<QString> childPathInside(const QString& root, const QString& relative);

// The name an image entry gets on disk. With Windows rules, characters
// Windows forbids become '_', trailing dots/spaces go and reserved device
// names (CON, NUL, COM1..., COM¹ COM² COM³, LPT¹..., CONIN$, CONOUT$, also with
// an extension) get a '_' prefix; otherwise names are unchanged.
QString hostFileName(const QString& name, bool windowsRules);

// True on Windows: names follow the Win32 rules and compare case-insensitively.
bool hostUsesWindowsNames();

// The name an entry gets on this host: hostFileName() with this platform's
// rules; a name isSafeEntryName() refuses is mapped with the Windows rules on
// every platform. The result always passes isSafeEntryName().
QString hostEntryName(const QString& name);

// The key under which this host's file system compares names (NTFS upper-cases
// them, so σ and ς are the same name there; elsewhere names are compared as
// they are).
QString hostNameKey(const QString& name);

} // namespace fw
