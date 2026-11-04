#pragma once

// Title / content IDs and the names the app gives to dumps and images.
//
// Everything here reproduces the legacy rules (exfat_builder.py
// sanitize_filename, build_exfat_name, _dumps_proposed_name and
// core/fpkg/param.py CONTENT_ID_RE).

#include <QString>

#include <optional>

namespace fw::gameinfo {

// ---- IDs -----------------------------------------------------------------------
// "UP0000-PPSA00000_00-0000000000000000": 2 letters + 4 digits, '-', 4 letters
// + 5 digits, '_' + 2 digits, '-', 16 upper-case letters or digits (36 chars).
bool isValidContentId(const QString& contentId);
// Characters 8..16 of a valid content ID ("PPSA00000"); empty otherwise.
QString titleIdFromContentId(const QString& contentId);
// 4 upper-case letters + 5 digits.
bool isValidTitleId(const QString& titleId);
// First "(PPSA|CUSA|PPLH)\d{5}" in `text` (case-insensitive), upper-cased.
// Empty when there is none. Used for folder-name fallbacks.
QString findTitleIdIn(const QString& text);
// Upper-case without '-' (the legacy dedupe key).
QString normalizedTitleId(const QString& titleId);

// ---- names -----------------------------------------------------------------------
// Legacy sanitize_filename(): typographic quotes/dashes/ellipsis normalised,
// trademark signs dropped, bullets to spaces, control characters dropped,
// \ / : * ? " < > | and the cmd.exe metacharacters & ^ % ! ` $ removed,
// whitespace collapsed, leading/trailing spaces and dots trimmed, at most 80
// characters. Returns "game" when nothing is left; empty input gives "".
QString sanitizeTitle(const QString& name);

// The three Dump Rename patterns (legacy keys in parentheses).
enum class NamePreset {
    TitleId,             // "ppsa"          PPSA01234
    TitleIdTitle,        // "ppsa_title"    PPSA01234 Spider-Man
    TitleIdTitleVersion, // "ppsa_title_v"  PPSA01234 Spider-Man (01.050.000)
};
QString namePresetKey(NamePreset preset);
std::optional<NamePreset> namePresetFromKey(const QString& key);

struct NameParts {
    QString titleId;
    QString title;
    QString version;
};

struct NameOptions {
    bool uppercaseTitleId = true; // legacy default (no UI to change it)
    bool sanitize = true;         // legacy default (no UI to change it)
};

// Folder name for a dump (no extension). Empty when there is no title ID,
// which the legacy page showed as "No PPSA found".
QString buildDumpName(const NameParts& parts, NamePreset preset, const NameOptions& options = {});

// Legacy build_exfat_name() without the ".exfat" extension:
// "<ID> <Title> (<version>)", sanitised; "game" when there is nothing.
QString buildImageBaseName(const NameParts& parts);

// Why `name` cannot be used as a folder name on Windows (forbidden
// characters, reserved device names, trailing dot/space, too long); empty when
// it is fine.
QString folderNameProblem(const QString& name);

} // namespace fw::gameinfo
