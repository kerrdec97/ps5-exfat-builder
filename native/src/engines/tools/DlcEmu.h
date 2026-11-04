#pragma once

// dlc_emu.ini editor model (legacy Advanced › DLC-emu, ui/dlc_emu_ini.py —
// original code of this project, ported).
//
// The file declares one DLC per [SECTION] and the same section name repeats,
// so it is modelled as an ordered list of entries; parse → emit is lossless
// (unknown keys kept). Limits from the dlc_emu README: 32 KiB, 1024 entries,
// content_id exactly 36 characters, labels 1-16 alphanumeric, service label
// 1-6, mount_point at most 15 characters, entitlement key 32 hex digits,
// download_status from a fixed list.

#include "core/Result.h"

#include <QString>
#include <QStringList>

#include <utility>
#include <vector>

namespace fw::tools::dlcemu {

inline constexpr int kMaxIniBytes = 32 * 1024;
inline constexpr int kMaxEntries = 1024;

const QStringList& sectionTypes();     // NONE PSGD PSAC PSAL PSCONS PSVC PSSUBS
const QStringList& downloadStatuses(); // NO_EXTRA_DATA ... INSTALLED
const QStringList& paramKeys();        // known keys, in emit order

struct Entry {
    QString section = QStringLiteral("PSAC");
    std::vector<std::pair<QString, QString>> params; // ordered
    // Not written to the file: where a suggestion came from.
    QString confidence; // "high" | "medium" | "low" | ""
    QString source;
    bool placeholder = false;

    QString get(const QString& key) const;
    void set(const QString& key, const QString& value);
};

std::vector<Entry> parse(const QString& text);
QString render(const std::vector<Entry>& entries, const QString& newline = QStringLiteral("\r\n"));
int byteSize(const std::vector<Entry>& entries);

struct Validation {
    QStringList errors;   // "Entry 3 (PSAC): content_id is required."
    QStringList warnings;
    bool ok() const { return errors.isEmpty(); }
};
// `app0` (optional): the game folder, to warn when a mount_point folder is
// missing locally (it may still exist on the console).
Validation validate(const std::vector<Entry>& entries, const QString& app0 = {});

// Reads <app0>/dlc_emu.ini (empty list if absent).
Result<std::vector<Entry>> load(const QString& app0);
// Validates, then writes <app0>/dlc_emu.ini atomically; an existing file is
// first copied to dlc_emu.ini.bak.
Status save(const QString& app0, const std::vector<Entry>& entries);

// ---- offline auto-find ---------------------------------------------------------------
bool looksLikeBaseGameId(const QString& contentId);
QStringList detectAddcontFolders(const QString& app0);           // "/app0/addcont0", ...
QStringList paramJsonContentIds(const QString& app0);            // dump's own content IDs
std::pair<QString, bool> detectContentPrefix(const QString& app0); // ("UP9000-PPSA01234_00-", placeholder?)
std::vector<Entry> scanBinaryForCodes(const QString& path, const QString& contentPrefix);

struct Suggestions {
    std::vector<Entry> entries; // in confidence order, de-duplicated
    QString prefix;
    bool prefixIsPlaceholder = false;
    QStringList addcont;
    QStringList baseIds;
    QStringList notes;
};
Suggestions autoFind(const QString& app0, bool scanEboot = true, const std::vector<Entry>& existing = {});

} // namespace fw::tools::dlcemu
