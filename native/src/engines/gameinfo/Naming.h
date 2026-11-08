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

// ---- image file names (Build page) ------------------------------------------------
// The legacy Build tab presets (ui/tab_unibuild.py _gen_name; keys as in the
// legacy name_preset setting) plus "folder" (the source folder's own name).
enum class ImageNamePreset {
    TitleId,                     // "ppsa"              PPSA01234
    TitleIdTitle,                // "ppsa_title"        PPSA01234 Spider-Man
    TitleIdTitleVersion,         // "ppsa_title_ver"    PPSA01234 Spider-Man (01.050.000)   (default)
    TitleIdTitleVersionFirmware, // "ppsa_title_ver_fw" PPSA01234 Spider-Man (01.050.000) (FW4)
    Folder,                      // "folder"            the source folder name
    Custom,                      // "custom"            the template below
};
QString imageNamePresetKey(ImageNamePreset preset);
std::optional<ImageNamePreset> imageNamePresetFromKey(const QString& key);

// The legacy default template (name_custom_template).
inline const QString kDefaultImageNameTemplate = QStringLiteral("%TITLE% (%VERSION%) (%FIRMWARE%) (%PPSA%)");

struct ImageNameFields {
    QString titleId;    // %PPSA%
    QString title;      // %TITLE%
    QString version;    // %VERSION%
    QString firmware;   // major of the required system software ("4", "10"); %FIRMWARE% = "FW4"
    QString sdk;        // %SDK% ("9.00")
    QString type;       // %TYPE% (legacy labels: exFAT, ffpkg, PFS, FPKG)
    QString folderName; // %FOLDER%, and the fallback when nothing else is known
    QString date;       // %DATE% (yyyy-MM-dd); empty = today
};

// Legacy _gen_name(): the preset (or, for Custom, the template with the tokens
// %TITLE% %PPSA% %VERSION% %FIRMWARE% %TYPE% %DATE% and the new %FOLDER% %SDK%,
// case-insensitive) → empty (), [] and {} removed, whitespace collapsed,
// spaces inside brackets trimmed, " -_–—" stripped at both ends → the folder
// name when nothing is left → sanitizeImageName(). Never empty ("game").
QString renderImageName(ImageNamePreset preset, const QString& customTemplate, const ImageNameFields& fields);

// File-name cleanup for image names: keeps Unicode letters, apostrophes and
// brackets ("Ghost of Yōtei", "Demon's Souls"), and removes only what Windows
// forbids in a file name (< > : " / \ | ? * and control characters).
// Whitespace is collapsed, spaces and dots are trimmed at both ends, reserved
// device names get a trailing "_", and the name is capped at 150 characters.
QString sanitizeImageName(const QString& name);

// "4.50" → "4", "10.00" → "10"; empty for anything else.
QString firmwareMajor(const QString& systemVersion);

} // namespace fw::gameinfo
