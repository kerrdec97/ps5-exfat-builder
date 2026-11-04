#pragma once

// Backport and originals ZIPs (docs/BACKPORT_PARITY.md §2.13, S10, S17, S23,
// S28, S30). Archives are written with the shared core ZIP writer
// (core/Zip.h) to "<final>.part", read back completely (CRC, size, member
// count and the SHA-256 of every member against the hash recorded for its
// source) and only then renamed to the final name, which is never an
// existing file: "<stem> (N).zip" is used instead.

#include "core/Cancellation.h"
#include "core/Progress.h"
#include "core/Result.h"

#include <QMap>
#include <QString>

#include <functional>
#include <vector>

namespace fw::backport {

// Name suffix of the file being written and the longest unique suffix: the
// archive is first "<final>.part" (the core writer stages it as
// "<final>.part.part"), and the final name may get " (99)" (S10).
inline constexpr const char* kWorstNameSuffix = " (99).part.part";

QString originalsZipName(const QString& prefix);
// "FW <sdk>xx <prefix> backport files.zip"; with the PS5 Pro fix applied
// (#32) "FW <sdk>xx <prefix> Pro fix backport files.zip", as the Backport GUI.
QString backportZipName(const QString& prefix, int sdk, bool proFix = false);

// `path` if neither it nor its ".part" exists, else "<stem> (N)<ext>" for
// the first free N (an earlier ZIP is never overwritten).
QString uniqueZipPath(const QString& path);

// Empty when a ZIP named `zipName` can be written to `dest`; otherwise the
// reason (the folder is set, exists, is not inside the game, the longest name
// fits in 259 characters, and a probe file can be created and removed).
QString zipDestinationProblem(const QString& dest, const QString& game, const QString& zipName);

struct ZipMember {
    QString arcName;
    QString sourcePath;
    QString sha256; // expected content hash (required)
};

// Drops case-insensitive duplicates (first one wins) and sorts by name.
std::vector<ZipMember> normalizeMembers(std::vector<ZipMember> members);

struct VerifiedZip {
    QString path;                   // final path after commit()
    int count = 0;
    QMap<QString, QString> sha256;  // member → SHA-256, read back from the archive
    QString zipSha256;              // of the whole archive
    qint64 bytes = 0;
};

// Test hook: called before each member is added (point "zip.member") and
// before the archive is renamed ("zip.commit"); an error aborts the write.
using ZipHook = std::function<Status(const QString& point, const QString& item)>;

class StagedZip {
public:
    explicit StagedZip(QString finalPath);
    ~StagedZip(); // removes the .part unless committed
    StagedZip(const StagedZip&) = delete;
    StagedZip& operator=(const StagedZip&) = delete;

    QString finalPath() const { return m_final; }
    QString partPath() const { return m_final + QStringLiteral(".part"); }

    // Writes every member (deflate) to partPath().
    Status write(const std::vector<ZipMember>& members, const CancellationToken& token,
                 ProgressReporter* progress = nullptr, const ZipHook& hook = {});
    // Reads partPath() back: member count, CRC and size of every member, and
    // each member's SHA-256 against ZipMember::sha256.
    Status verify(const CancellationToken& token, ProgressReporter* progress = nullptr);
    // Renames partPath() to finalPath(), never over an existing file.
    Status commit(const ZipHook& hook = {});
    // Deletes partPath() now.
    void discard();

    const VerifiedZip& result() const { return m_result; }
    bool committed() const { return m_committed; }

private:
    QString m_final;
    std::vector<ZipMember> m_members;
    VerifiedZip m_result;
    bool m_written = false;
    bool m_verified = false;
    bool m_committed = false;
};

} // namespace fw::backport
