#pragma once

// What of a dump goes into a package: OS junk, files the SDK generates
// itself, option-driven leftovers (AMPR emulator, DLC emulator, dumper and
// Unreal "Saved" folders), the Publishing Tools path rule, and the
// dlc_emu.ini parser used to declare DLC mount folders.

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <vector>

namespace fw::fpkg {

// ---- OS junk (never packaged; a folder holding only junk counts as empty) ------
bool isJunkFileName(const QString& name);
bool isJunkDirName(const QString& name);

// ---- Publishing Tools path rule ------------------------------------------------
// Printable ASCII (0x20-0x7E), no '%' or ';', no empty component, no component
// ending in '.'. Returns the reason, or "" when the path is accepted.
QString pathRuleProblem(const QString& dstPath);

// ---- base exclusions -----------------------------------------------------------
// `parts` are the parent components of the entry (empty for the root), `name`
// its own name. Returns the reason the entry is left out, or "".
QString baseExclusionReason(const QStringList& parts, const QString& name, bool isDir);

// ---- option-driven leftovers ------------------------------------------------------
struct LeftoverOptions {
    bool keepDlcEmulator = true;
    bool removeAmprLeftovers = true;
    bool removeDumpLeftovers = true;
};

// `childNames` lists the (merged) children of the entry when it is a folder,
// `rootHasContentPaks(name)` tells whether root folder `name` has Content/Paks
// (Unreal project). Returns the reason, or "".
struct LeftoverContext {
    QStringList childNames;           // children of this folder (folders only)
    bool rootFolderHasContentPaks = false; // for a folder directly under a root folder
};
QString leftoverReason(const QStringList& parts, const QString& name, bool isDir, const LeftoverOptions& options,
                       const LeftoverContext& context);

// ---- dlc_emu.ini (drakmor/dlc_emu format) -------------------------------------------
struct DlcEmuEntry {
    QString section;        // "PSAC", "PSAL", ...
    QString contentId;
    QString mountPoint;     // as written, e.g. "/app0/addcont0"
    QString downloadStatus;
    QString entitlementKey;
    QString mountFolder() const; // "addcont0" (relative to the app root) or ""
};

struct DlcEmuIni {
    std::vector<DlcEmuEntry> entries; // [PSAC] entries with content_id, duplicates dropped, order kept
    std::vector<DlcEmuEntry> allEntries; // every section with a content_id (for the add-on dialog)
    QStringList warnings;
};

inline constexpr qint64 kDlcEmuIniMaxBytes = 64 * 1024;
DlcEmuIni parseDlcEmuIni(const QByteArray& text);

// Root folders named addcont<N> (case-insensitive).
bool isAddcontFolderName(const QString& name);

// libSceAmpr imported by eboot.bin (first bytes searched for the module name).
bool bytesImportAmpr(const QByteArray& head);

} // namespace fw::fpkg
