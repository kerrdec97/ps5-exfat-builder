#pragma once

// ampr_emu.index in an exFAT build (exfat.amprIndex).
//
// A game with fakelib/libSceAmpr.sprx needs ampr_emu.index at its top: the
// index of its files. The build adds a missing one and replaces one that no
// longer matches, as the legacy app did before every build, but in the image
// only: the index is written to the job's temporary folder and takes the
// place of (or joins) the root's entry in the scanned tree. The dump folder
// is never written.

#include "core/Result.h"
#include "engines/exfat/SourceScan.h"
#include "engines/pfs/AmprIndex.h"

namespace fw::exfat {

// The index of the files `tree` holds, by the rules of the AMPR index tool
// (pfs::amprToolRows). Empty when there is nothing to list.
QByteArray amprIndexFor(const SourceTree& tree);

// What `mode` makes of the scanned game (pfs::decideAmprIndex).
pfs::AmprIndexDecision decideAmprIndex(const SourceTree& tree, pfs::AmprMode mode);

// Makes `indexFile` (a file on disk) the root's ampr_emu.index in `tree`: in
// place of the dump's own entry, or as a new entry where Windows would list it.
void placeAmprIndex(SourceTree& tree, const QString& indexFile);

struct AmprPlacement {
    pfs::AmprIndexAction action = pfs::AmprIndexAction::Off;
    QString indexFile; // set when the tree got a new index: the file in `tempDir` that holds it
};
// Decides and, when the image gets a new index, writes it into `tempDir` and
// places it in `tree`.
Result<AmprPlacement> applyAmprIndex(SourceTree& tree, pfs::AmprMode mode, const QString& tempDir);

// For a verification against a folder: when `image` holds the index of the
// folder's files (`tree`) and the folder has none or another one, the image's
// index is what the tree expects (it is saved into `tempDir` and placed).
// Returns true when it did that. An image that cannot be read changes nothing:
// the verification itself reports it.
bool acceptBuiltAmprIndex(SourceTree& tree, const QString& image, const QString& tempDir, const CancellationToken& token);

} // namespace fw::exfat
