#pragma once

// Fakelib provider set chosen by the user (docs/BACKPORT_PARITY.md §2.10,
// safety rule S8) and its relation to a game's own fakelib/ folder (S6).
// Only the .sprx/.prx libraries directly in the chosen folder are part of
// the set; other files (notes, OS metadata) are ignored and listed.

#include "core/Cancellation.h"

#include <QString>
#include <QStringList>

#include <vector>

namespace fw::backport {

inline constexpr qint64 kMinFakelibBytes = 1024;

struct FakelibLib {
    QString name;   // file name (libraries sit directly in the folder)
    QString path;   // absolute source path
    qint64 size = 0;
    QString sha256;
};

struct FakelibSource {
    QString dir;
    std::vector<FakelibLib> libs; // sorted by name
    QStringList ignored;          // non-library files, metadata
    qint64 totalBytes() const;
    const FakelibLib* find(const QString& name) const; // case-insensitive
};

// Problems with a fakelib folder (empty list = usable): it must exist and be
// a folder outside the game (and not contain it); libraries must sit directly
// in it (no sub-folders; __MACOSX, AppleDouble and other OS metadata ignored);
// each must be at least 1 KiB and have ELF/SELF magic; at least one library.
// On success `out` receives the set with SHA-256 hashes.
QStringList fakelibProblems(const QString& dir, const QString& game, FakelibSource* out = nullptr,
                            const CancellationToken& token = {});

// The top-level libraries of a folder with hashes, without validation.
FakelibSource listFakelibSource(const QString& dir, const CancellationToken& token = {});

enum class FakelibState {
    None,  // no game/fakelib folder, or it holds no files
    Same,  // every library of the set is there with the same size and SHA-256
    Other, // another tool's set, an older set, or no set to compare with
};

QString fakelibStateName(FakelibState s);

FakelibState fakelibState(const QString& game, const FakelibSource* source, const CancellationToken& token = {});

// Files in game/fakelib/ that the set does not provide (carried over
// unchanged with a warning, D3).
QStringList fakelibExtras(const QString& game, const FakelibSource& source);

} // namespace fw::backport
