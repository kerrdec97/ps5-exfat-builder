#pragma once

// The canonical AGC / AGCDriver / PSML fakelib trio of the owner's Backport
// engine (mandatory_fakelibs.py, MANDATORY_FAKELIB_BUNDLE_V1/manifest.json,
// bundle super-bomberman-r2-ppsa07191-01.003.001-fakelib-v1): every package
// that engine produces ships these three exact files in fakelib/. They are
// Sony libraries from another tool's backport, so they are never bundled;
// only their names, sizes and SHA-256 are kept here, as data, so the Backport
// tab can recognise a game folder that already holds backport files (one
// pinned file is enough, as in the Backport GUI's library scan).

#include <QString>

#include <array>

namespace fw::backport {

struct PinnedLibrary {
    const char* name;   // file name in fakelib/ (canonical spelling)
    qint64 size;
    const char* sha256; // lowercase hex
};

inline constexpr std::array<PinnedLibrary, 3> kCanonicalTrio = {{
    {"libSceAgc.sprx", 326986, "eace8ac152404132e632a7e170ddcca47f230e7c0c02cd4b76f29ca9604037ff"},
    {"libSceAgcDriver.sprx", 122613, "43153558e5917b818207cd9fd81a817b2f2fe338b9a740ef35a563dcbec03cfe"},
    {"libScePsml.sprx", 1536002, "3e1df31de4049d958faec7ed3575d9ef7776eeb76e0ac2354d38c8b5eb33ad21"},
}};

} // namespace fw::backport
