#pragma once

// ps5_autoloader editor (legacy Y2JB › Autoloader, exfat_builder.py
// _al_load/_al_save/_al_insert_*/_al_add_elf/_al_delete_payload), with the
// rules of today's autoloaders (itsPLK/ps5-unified-autoloader 0.1.7, shared by
// the Y2JB, BD-JB, Lua, WebKit and yarpe autoloaders; research §9):
//
// `autoload.txt` in a LOCAL ps5_autoloader folder (the user copies the folder
// to a USB drive or /data; nothing is pushed to the console from here):
//
//   * one payload per line: a file of the folder, or an absolute path when the
//     line starts with "/"; .elf and .bin (there is no extension filter
//     upstream, so a .js line is sent to elfldr and fails: warned),
//   * "!<ms>" sleeps that many milliseconds ("!1000"),
//   * '#' anywhere starts a comment (so names cannot contain '#'),
//   * "@sync" copies the folder from USB to /data after a clean run,
//   * trailing spaces are stripped, leading spaces are not (warned),
//   * elfldr.elf (exactly that name) must come first, followed by !4000,
//     when a custom ELF loader is used (the bundled one is localhost-only).
//
// Search order: /mnt/usbN/ps5_autoloader_<TITLEID>, /data/ps5_autoloader_<TITLEID>,
// /mnt/usbN/ps5_autoloader, /data/ps5_autoloader (savedata is no longer searched).
//
// The file is written with LF line endings, runs of 3+ newlines collapsed to
// one blank line and exactly one trailing newline (the legacy normalisation).
// The first save of a folder without autoload.txt starts from a header that
// documents the rules above.

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QString>
#include <QStringList>

#include <vector>

namespace fw {
class SettingsValues;
}

namespace fw::ps5::autoloader {

inline const QString kFileName = QStringLiteral("autoload.txt");
inline const QString kElfLoader = QStringLiteral("elfldr.elf");

// The folder from the settings (ps5.autoloaderFolder, legacy al_dir).
QString folderFromSettings(const SettingsValues& settings);

// The legacy default header (load order, usage, elfldr.elf note).
QString defaultHeader();

// CRLF/CR -> LF, 3+ consecutive newlines -> 2, exactly one trailing newline.
QString normalize(const QString& text);

// True for the payload types the autoloader runs (.elf, .bin; any case).
bool isPayloadName(const QString& name);

enum class LineKind { Blank, Comment, Sleep, Payload, Directive, Invalid };

struct Line {
    int number = 0;        // 1-based
    LineKind kind = LineKind::Blank;
    QString text;          // trimmed, without its comment
    int sleepMs = 0;       // Sleep
    QString payload;       // Payload
    QString problem;       // Invalid: why
    QString directive;     // Directive: "sync"
    bool absolute = false; // Payload: an absolute path ("/data/x.elf")
    QString warning;       // allowed but probably wrong (leading spaces, .js, unknown @word)
};

std::vector<Line> parse(const QString& text);

struct Validation {
    QStringList errors;   // the file would not do what it says
    QStringList warnings; // probably a mistake
    int payloads = 0;
    int sleeps = 0;
    bool ok() const { return errors.isEmpty(); }
};

// Checks every line; with `folderFiles` (the folder's payload files, as
// payloadFiles() returns them) a payload that is not in the folder is a
// warning. `checkFiles` = false skips that check.
Validation validate(const QString& text, const QStringList& folderFiles = {}, bool checkFiles = false);

// The .elf/.bin files directly inside `folder`, sorted case-insensitively.
QStringList payloadFiles(const QString& folder);

// "!<ms>"
QString sleepLine(int ms);

struct Insertion {
    QString text;
    int cursor = 0; // position right after the inserted line
};
// Inserts "\n<line>" at the end of the line holding `cursor` (legacy: insert
// at the end of the current line, so a click never splits a line).
Insertion insertLineAfter(const QString& text, int cursor, const QString& line);

// Removes every line that is exactly `fileName` (after trimming) and the
// "!<ms>" lines right after it (legacy _al_delete_payload).
QString removePayloadLines(const QString& text, const QString& fileName);

struct Loaded {
    QString text;         // normalised file contents, or defaultHeader()
    bool existed = false; // false: the folder has no autoload.txt yet
};
Result<Loaded> load(const QString& folder);

struct SaveResult {
    QString path;       // <folder>/autoload.txt
    QString backupPath; // copy of the previous file; empty when there was none or nothing changed
    bool changed = false;
};
// Normalises `text` and writes it atomically (UTF-8, LF). An existing file is
// first copied to autoload.txt.bak (.bak.2, ... when a backup exists: a backup
// is never overwritten). Unchanged contents are not written again.
Result<SaveResult> save(const QString& folder, const QString& text);

// Copies a payload into the folder through a .part file. Only .elf/.bin;
// an existing file of that name is replaced only when `overwrite` is true.
// Returns the file name.
Result<QString> addPayload(const QString& folder, const QString& sourceFile, bool overwrite,
                           const CancellationToken& token = {});

// Removes a payload file from the folder. With `toRecycleBin`, to the recycle
// bin when the system has one (`*trashed` = true), otherwise deleted. Only
// .elf/.bin (and old .js) names without path separators.
Status deletePayload(const QString& folder, const QString& fileName, bool toRecycleBin, bool* trashed = nullptr);

} // namespace fw::ps5::autoloader
