#pragma once

// DLC INI files (the ppr-pkg Companion's DlcIniDocument, ported).
//
// Any INI whose lines carry content_id / contentId keys (dlc_emu.ini and
// other DLC lists). Every such line is one entry, in any section. Comments,
// blank lines, unknown keys, quoting, inline comments, line endings, the
// encoding and the BOM are kept byte for byte when identifiers or titles are
// rewritten; only the edited fields change.
//
// Encodings: BOM -> UTF-32 LE/BE, UTF-8, UTF-16 LE/BE (strict decoding);
// no BOM -> UTF-8, or Latin-1 (lossless single-byte) when that is not valid
// UTF-8. Limit 16 MiB.
//
// Keys (case-insensitive): content_id/contentId, title_id/titleId,
// title/title_name/titleName/name, and primary_id/primaryId (rewritten only
// when its value equals the entry's original Content ID).

#include "core/Result.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QStringList>

#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace fw::dlc {

inline constexpr qint64 kMaxIniBytes = 16LL * 1024 * 1024;

struct DlcIniEntry {
    int index = 0;            // 0-based, file order
    QString contentId;        // as written
    QString titleId;          // from the Content ID, or the INI's title_id value
    QString title;            // name key, else the preceding comment, else the Content ID suffix
    QString section;          // "" before any section
    QString region;           // regionText(contentId)
    int contentLine = 0;      // line index of the content_id line
    int startLine = 0;        // line index of the section header (0 before any section)
    int endLine = 0;          // exclusive end of this entry's block
    int titleCommentLine = -1;
    std::vector<std::pair<QString, QString>> metadata; // key = value in the block (last wins, file order)
    QStringList warnings;
};

struct DlcTarget {
    QString titleId;
    QString contentId;
    std::optional<QString> title; // nullopt = keep
};

class DlcIniDocument {
public:
    // "DLC INI does not exist." / "DLC INI exceeds the 16 MiB safety limit." / encoding errors.
    static Result<DlcIniDocument> load(const QString& path);
    static Result<DlcIniDocument> parse(const QByteArray& bytes, const QString& path = {});

    const QString& path() const { return m_path; }
    QString encodingName() const;  // "utf-8 (no BOM)", "utf-16LE (BOM)", "iso-8859-1 (no BOM)" ...
    const std::vector<DlcIniEntry>& entries() const { return m_entries; }
    const QStringList& warnings() const { return m_warnings; } // document-level
    const QByteArray& originalBytes() const { return m_original; }
    qint64 sourceSize() const { return m_size; }
    QDateTime sourceModifiedUtc() const { return m_modified; }
    // The file on disk still has the size and modification time seen at load.
    bool sourceUnchanged() const;

    // The whole file with `changes` (entry index -> target) applied, validated
    // by re-parsing (identity, then titles). Errors use the Companion's texts.
    Result<QByteArray> updatedBytes(const std::map<int, DlcTarget>& changes) const;
    // Writes a NEW file (never overwrites; never the source itself).
    Status saveUpdated(const QString& outPath, const std::map<int, DlcTarget>& changes) const;

    // A new INI for a PKG-only entry: UTF-8 without BOM,
    // "#<title>\r\ncontent_id=<cid>\r\n" (validated by parsing).
    static Result<QByteArray> newIniForPackage(const QString& title, const QString& contentId);

private:
    struct Line {
        QString text;
        QString end; // exact terminator ("" for the last line)
    };
    QString m_path;
    QByteArray m_original;
    QByteArray m_bom;
    QString m_codec;     // "utf-8", "utf-16le", "utf-16be", "utf-32le", "utf-32be", "latin1"
    bool m_hasBom = false;
    std::vector<Line> m_lines;
    std::vector<DlcIniEntry> m_entries;
    QStringList m_warnings;
    qint64 m_size = 0;
    QDateTime m_modified;
    friend struct DlcIniTestAccess;
};

// Writes `data` to a new file `path` (fails when it exists): written as a
// .part file (PartFile), flushed to disk and only then given its name, so a
// crash or a full drive never leaves a short file under that name.
Status writeNewFile(const QString& path, const QByteArray& data);

} // namespace fw::dlc
