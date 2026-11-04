#pragma once

// The build stage of a backport (docs/BACKPORT_PARITY.md §6.2, §6.4), run
// entirely inside a private staging folder; the game folder is only read
// (D1, S31). Per executable: snapshot → unwrap (SELF) → SDK patch → fake
// sign → libc NID substitution → build verification (V1 structure, V2 round
// trip, V3 SDK) → stage. Then the fakelib set is staged and checked (V4) and
// coverage is checked (V5). Any per-file failure makes the build fail (D4).
//
// Staging layout: originals/<rel> (snapshot of every file Apply may
// replace), unwrapped/<rel> (ELF of SELF inputs, for diagnosis), stage/<rel>
// (what goes into the backport ZIP and the game).

#include "core/Result.h"
#include "engines/backport/BackportPlanner.h"
#include "engines/backport/NidPatch.h"
#include "engines/backport/SceParam.h"
#include "jobs/Job.h"

#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

namespace fw::backport {

enum class FileStage { Snapshot, Unwrap, Patch, Sign, Nid, Verify, Stage, Done, LeftUnchanged };
QString fileStageName(FileStage s);

struct FileResult {
    QString rel;
    bool ok = false;
    FileStage stage = FileStage::Snapshot; // last stage reached (the failing one when !ok)
    Code code = Code::Ok;
    QString message;
    std::optional<int> sdkBefore, sdkAfter;
    QString shaBefore, shaAfter;
    qint64 sizeBefore = 0, sizeAfter = 0;
    NidResult nid;
    QStringList patchMessages;
};

struct StagedFile {
    QString rel;      // game-relative
    QString path;     // absolute path in staging
    QString sha256;
    qint64 size = 0;
    bool executable = false; // a signed executable (else a fakelib library)
};

struct BuildResult {
    QString stagingDir;
    std::vector<FileResult> files;
    std::vector<StagedFile> staged;     // stage/<rel>
    std::vector<StagedFile> originals;  // originals/<rel>: executables and replaced fakelib files
    QStringList leftUnchanged;
    QStringList warnings;
    bool nothingChanged = false;        // every output equals the game's current bytes

    bool failed() const;
    const FileResult* firstFailure() const;
    int okCount() const;
};

QString stagingOriginalsDir(const QString& staging);
QString stagingUnwrappedDir(const QString& staging);
QString stagingStageDir(const QString& staging);

// Errors: cancellation, and snapshot/staging I/O failures (S13: a copy error
// stops the run). Per-file processing failures are in BuildResult::files.
Result<BuildResult> buildBackport(const BackportPlan& plan, const QString& stagingDir, JobContext& ctx);

// The per-file pipeline on bytes, without I/O (also used by tests).
struct ProcessedExecutable {
    QByteArray elf;     // patched ELF
    QByteArray output;  // signed (and NID-substituted) SELF
    PatchReport patch;
    NidResult nid;
};
Result<ProcessedExecutable> processExecutable(const QByteArray& input, const SdkPair& pair, NidMode nidMode,
                                              bool verify, QStringList* warnings = nullptr);

// V1-V3 on a staged SELF against the patched ELF it was made from.
Status verifySignedExecutable(const QByteArray& output, const QByteArray& patchedElf, const NidResult& nid,
                              const SdkPair& pair);

} // namespace fw::backport
