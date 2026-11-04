#pragma once

// Unity module-name case fix (docs/BACKPORT_PARITY.md matrix #41, Backport
// GUI issue #2). Ported 2026-11-04 from the owner's Backport engine
// (make_baseline.py MODULE_LOAD_STRING / module_case_renames, exporter.py
// _rename_case).
//
// Unity games load "Media/Modules/Il2CppUserAssemblies.prx" by that exact
// name, while 19 of 34 Unity dumps the owner checked ship
// "Il2cppUserAssemblies.prx". ShadowMountPlus on internal storage resolves
// names case-sensitively, so the module never loads and the game calls a
// null il2cpp export (MGS4 on a base PS5 at FW 4.03, RIP 0). The spelling the
// game asks for is found in the decoded eboot.bin as the text
// "%s/Modules/<Name>.prx" (the DT_NEEDED module name is a separate,
// lower-case string and does not decide the file name). When the dump has
// that file under another letter case, the backport ZIP and Apply use the
// game's spelling; the originals ZIP and Restore keep the dump's.
//
// Only the file name takes the game's spelling; the folder spelling
// is the dump's (a differing folder is reported). A hardware confirmation of
// the fix is still pending in the owner's records.

#include "core/Result.h"

#include <QByteArrayView>
#include <QString>
#include <QStringList>

#include <vector>

namespace fw::backport {

struct ScanResult;

// The game-root paths the eboot loads by name: "Media/Modules/<Name>.prx" for
// every "%s/Modules/<Name>.prx" in `ebootElf` (names of [A-Za-z0-9_.-],
// case kept), de-duplicated, in the order found.
QStringList requestedModulePaths(QByteArrayView ebootElf);

struct ModuleCaseRename {
    QString from; // the dump's rel
    QString to;   // the same folder, the file name as the game spells it
    bool folderCaseDiffers = false; // the dump's folder is not spelled "Media/Modules"
};

struct ModuleCasePlan {
    std::vector<ModuleCaseRename> renames;
    QStringList notes; // what was found but not renamed (folder case only, not an executable)
};

// Every requested path that a game executable matches only when letter case
// is ignored.
ModuleCasePlan planModuleCase(const ScanResult& scan, const QStringList& requested);

} // namespace fw::backport
