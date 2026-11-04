#pragma once

// User-supplied AMPR / PlayGo providers with hash pinning (docs/BACKPORT_PARITY.md
// matrix #34, "Switch AMPR / PlayGo"). The user points backport.amprPlaygoFolder
// at a folder with their own libSceAmpr.sprx and/or libScePlayGo.sprx (for
// example a build of drakmor's ampr_emu). Nothing is bundled or downloaded:
// the engine's own providers stay blocked (§7).
//
//  * The folder follows the fakelib rules (S8): outside the game, libraries
//    directly in it, at least 1 KiB, ELF/SELF magic. Only the two provider
//    names are used; any other library in it is ignored and listed.
//  * Pins: the SHA-256 and size of every provider file are recorded in
//    <dataDir>/backport/provider-pins.json, per folder. The first backport
//    with an unpinned folder records them (trust on first use, logged and
//    in the run report). After that a changed, added or missing file is
//    refused (Code::ProviderChanged) until the folder is pinned again
//    (job backport.pinProviders, "Re-pin" on the Backport page).
//  * The providers are used exactly where the fakelib folder is: they take
//    the place of the same-named libraries of the fakelib set (or are added
//    to it) when fakelib/ is staged, so they also replace a dump's own
//    libSceAmpr/libScePlayGo (kept in the originals ZIP, S14).

#include "core/Cancellation.h"
#include "core/Result.h"
#include "engines/backport/FakelibSet.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <optional>

namespace fw::backport {

// libSceAmpr / libScePlayGo with .sprx or .prx, any letter case.
bool isProviderLibraryName(const QString& fileName);
// <dataDir>/backport/provider-pins.json
QString defaultProviderPinsPath();

// Problems with a provider folder (empty list = usable); on success `out`
// receives the provider libraries with hashes and `ignored` the other
// libraries found in it.
QStringList providerProblems(const QString& dir, const QString& game, FakelibSource* out = nullptr,
                             QStringList* ignored = nullptr, const CancellationToken& token = {});

struct ProviderPin {
    QString sha256;
    qint64 size = 0;
};

struct ProviderPins {
    QString folder;
    QString pinnedAt;
    QMap<QString, ProviderPin> files; // file name → pin
};

// The pins recorded for `folder` (nullopt: never pinned). A damaged pins
// file reads as "never pinned" for every folder and is replaced on the next pin.
std::optional<ProviderPins> loadProviderPins(const QString& pinsPath, const QString& folder);

struct ProviderPinCheck {
    bool pinned = false;   // the folder has pins
    QString pinnedAt;
    QStringList changed;   // "libSceAmpr.sprx (pinned 82484486…, now 0aadd31d…)"
    QStringList added;     // present, not pinned
    QStringList missing;   // pinned, gone
    bool ok() const { return pinned && changed.isEmpty() && added.isEmpty() && missing.isEmpty(); }
    QString describe() const; // one sentence listing what differs
};

ProviderPinCheck checkProviderPins(const FakelibSource& providers, const QString& pinsPath);

// Records (or replaces) the pins of `providers.dir`. Other folders' pins are kept.
Status pinProviders(const FakelibSource& providers, const QString& pinsPath);

// The fakelib set with the providers in place of the same-named libraries
// (the set's file name is kept) or added to it. `base` may be empty.
FakelibSource withProviders(const std::optional<FakelibSource>& base, const FakelibSource& providers);

} // namespace fw::backport
