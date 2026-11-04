#pragma once

// Shared pieces of the backport engine: the outcome codes of
// docs/BACKPORT_PARITY.md §6.3, error construction, OS-metadata rules (S15)
// and small file helpers used by every stage.

#include "core/Cancellation.h"
#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace fw::backport {

// Engine identity recorded in manifests and logs.
inline constexpr const char* kEngineName = "fw-backport";
inline constexpr const char* kEngineVersion = "1.0.0";

// Executables are processed in memory; a larger file is refused (§6.2; the
// limit itself is UNVERIFIED against real titles, which are far smaller).
inline constexpr qint64 kMaxExecBytes = 1024LL * 1024 * 1024;

// Windows MAX_PATH without the terminating NUL (S10, S11, S28).
inline constexpr int kMaxWinPath = 259;

// Outcome / error codes (§6.3). They travel inside fw::Error::detail as a
// "[Code]" prefix so callers and tests can tell them apart.
enum class Code {
    Ok,
    NotAGame,
    TargetSdkInvalid,
    TargetAboveGameSdk,
    GameSdkUnknown,
    AlreadyBackported,
    DiffersFromOriginals,
    FakelibInvalid,
    ZipFolderInvalid,
    PathTooLong,
    InsufficientSpace,
    NothingToDo,
    ElfInvalid,
    ParamMissing,
    ParamSizeInvalid,
    ParamMagicInvalid,
    ParamOutOfRange,
    SelfEncrypted,
    SelfCompressed,
    SelfMalformed,
    SignUnsupported,
    FileTooLarge,
    Unreadable,
    VerifyFailed,
    ZipWriteFailed,
    ZipVerifyFailed,
    GameChanged,
    ApplyFailed,
    InterruptedApply,
    RollbackFailed,
    RestoreFailed,
    ManifestMismatch,
    ModuleNameCase,  // #41 detector: an imported module differs from sce_module/ only in letter case
    ProviderChanged, // #34: a pinned AMPR/PlayGo library changed (or is not pinned yet)
    Cancelled,
    Internal,
};

QString codeName(Code code);
ErrorCode toErrorCode(Code code);

// Builds an fw::Error whose detail starts with "[CodeName]".
Error makeError(Code code, const QString& message, const QString& detail = {});
// The Code carried by an error made with makeError (Internal otherwise;
// Cancelled for fw cancellation errors).
Code codeOf(const Error& error);

// ---- OS metadata (S15) -------------------------------------------------------
bool isOsMetadataFile(const QString& fileName);
bool isOsMetadataDir(const QString& dirName);

// ---- paths -------------------------------------------------------------------
// Joins a '/'-separated relative path onto a root.
QString joinPath(const QString& root, const QString& rel);
// Normalises a relative path: '\' → '/', no leading "./" or '/'.
QString normalizeRel(const QString& rel);
// Lower-cased key for case-insensitive comparisons of relative paths.
inline QString relKey(const QString& rel) { return normalizeRel(rel).toLower(); }

// ---- files -------------------------------------------------------------------
// Reads a whole file (refuses files larger than `maxBytes`).
Result<QByteArray> readFileBytes(const QString& path, qint64 maxBytes = kMaxExecBytes);
// Writes through `<path>.part` and renames (never over an existing file
// unless `replaceExisting`).
Status writeFileBytes(const QString& path, const QByteArray& data, bool replaceExisting = false);
// Renames `src` over `dst`, replacing it atomically when it exists
// (MoveFileEx(REPLACE_EXISTING) on Windows, rename(2) elsewhere). A
// read-only `dst` is made writable first (Windows refuses to replace it).
Status replaceFileAtomic(const QString& src, const QString& dst);
// Clears the read-only attribute of a file (Windows) or adds the owner's
// write permission (elsewhere). Folders and links are left alone.
void clearReadOnly(const QString& path);
// QFile::remove, retried once after clearReadOnly().
bool removeFileForce(const QString& path);
// Reads the first `n` bytes of a file (fewer if the file is shorter).
Result<QByteArray> readHead(const QString& path, qint64 n);

} // namespace fw::backport
