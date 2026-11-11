#pragma once

// Dump check: reads a game dump and says what is missing, damaged or left
// over from a tool, before it is built into an image. It never writes: no
// file is created, changed, renamed or deleted, and nothing is repaired
// (restoring originals and backporting again is the Backport tab's work).
//
// One walk of the folder (links are reported, never followed). Of an
// executable only the header is read (the SELF entry table and the ELF
// header copy, a few kilobytes) plus the 24 bytes of its process / module
// param; no file is read whole. The findings that are listed are capped,
// their counts are exact, so a dump made to waste time or memory costs one
// directory walk and stays small in the result.
//
// Finding codes (docs/engines/TOOLS.md has the table):
//   structure   eboot-missing, eboot-empty, param-missing, param-unreadable,
//               param-incomplete, ps4-dump, icon-missing, game-nested,
//               several-games
//   executables exec-encrypted, exec-compressed, exec-truncated,
//               exec-malformed, exec-empty, exec-unreadable, exec-unknown,
//               exec-unsigned, exec-fake-signed
//   backport    backport-state, fakelib-present, fakelib-eboot-not-signed,
//               sdk-differs, backup-files
//   hygiene     empty-files, case-collision, bad-name, junk-files, link,
//               folder-unreadable, large-files

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>

#include <vector>

namespace fw::tools {

enum class FindingSeverity { Error, Warning, Info };
QString findingSeverityName(FindingSeverity severity); // "error" | "warning" | "info"

struct DumpFinding {
    FindingSeverity severity = FindingSeverity::Info;
    QString code;      // stable id, see the list above
    QString path;      // relative to the checked folder, '/' separators; "" = the dump as a whole
    QString message;   // one sentence for the user
    // A finding about many files at once (empty files, junk files...): how
    // many there are and the first of them.
    qint64 count = 0;
    QStringList paths;

    QJsonObject toJson() const; // {severity, code, path?, message, count?, paths?}
};

// What kind of file an executable is, from its header.
enum class ExecutableClass { FakeSigned, Encrypted, Compressed, PlainElf, Truncated, Malformed, Empty, Unknown, Unreadable };
QString executableClassName(ExecutableClass c); // "fake-signed", "encrypted", ...

struct ExecutableInfo {
    ExecutableClass kind = ExecutableClass::Unknown;
    QString problem;      // why it is not a usable executable ("" when it is)
    bool hasSdk = false;  // a process / module param was read
    quint32 ps5Sdk = 0;   // its PS5 SDK value (backport::sdkVersionText() formats it)
};

// Classifies `path` from its header alone. Never reads more than
// kDumpCheckMaxHeaderBytes of the file.
ExecutableInfo inspectExecutable(const QString& path);

inline constexpr qint64 kDumpCheckHeaderBytes = 64 * 1024;          // the first read
inline constexpr qint64 kDumpCheckMaxHeaderBytes = 1024 * 1024;     // a SELF header that claims more is malformed

struct DumpCheckOptions {
    int maxFindings = 500;  // listed findings; counts stay exact
    int maxPerCode = 100;   // listed findings of one code, so a flood of one kind does not hide the others
    int maxSamplePaths = 20; // paths kept by a finding about many files
};

struct DumpCheckResult {
    QString folder;                     // what was checked
    std::vector<DumpFinding> findings;  // errors first, then warnings, then notes; capped
    qint64 errors = 0, warnings = 0, infos = 0; // exact, whatever was listed
    QMap<QString, qint64> byCode;       // exact count per code
    bool truncated = false;             // some findings are counted but not listed
    qint64 files = 0, folders = 0, bytes = 0;
    QMap<QString, qint64> executables;  // executableClassName() -> count
    qint64 executableCount = 0;
    QJsonObject facts;                  // title ID, versions, SDKs, fakelib, backport state

    // "No problems found in 18,442 files" / "2 problems and 3 warnings in 18,442 files".
    QString summary() const;
    // {findings: [...], counts: {...}, facts: {...}} (JobOutcome::details).
    QJsonObject toJson() const;
};

// Walks `folder` once. Fails only when the folder is not there or the walk
// was cancelled; everything else is a finding. `progress` may be null.
Result<DumpCheckResult> checkDump(const QString& folder, const CancellationToken& token, ProgressReporter* progress = nullptr,
                                  const DumpCheckOptions& options = {});

// ---- pieces with no file access (also for tests) -----------------------------------
// Why a console or an exFAT / Windows volume cannot store `name` (trailing dot
// or space, a reserved device name, a forbidden character, more than 255
// UTF-16 units); "" when it is fine.
QString dumpNameProblem(const QString& name);
// The names of `names` that are the same but for letter case, in groups
// ("Data", "data"). exFAT and the console's game mounts keep only one of each.
QList<QStringList> caseCollisions(const QStringList& names);
// Thumbs.db, desktop.ini, .DS_Store, *.part, *.tmp.
bool isJunkFileName(const QString& name);
// eboot.bin, *.elf, *.self, *.prx, *.sprx.
bool isExecutableName(const QString& name);

} // namespace fw::tools
