#pragma once

// GP5 projects for prospero-pub-cmd.
//
// Output is byte-compatible with what Python's xml.etree.ElementTree writes
// after ET.indent(space="  "): single-quoted XML declaration, two-space
// indent, " />" for empty elements, attribute order as inserted, attribute
// escaping of & < > " CR LF TAB. UTF-8 without BOM, every LF written as CRLF,
// no line break after the last line.

#include "core/Result.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>
#include <vector>

namespace fw::fpkg {

using XmlAttrs = std::vector<std::pair<QString, QString>>;

struct XmlElement {
    QString tag;
    XmlAttrs attrs;
    QString text;
    std::vector<XmlElement> children;
};

QString escapeXmlAttribute(const QString& s);
QString escapeXmlText(const QString& s);
// ElementTree-style document (declaration + indented root), LF line ends.
QByteArray writeEtreeDocument(const XmlElement& root);

struct Gp5Chunk {
    XmlAttrs attrs; // id, label, layer_no, languages ... in output order
};

struct Gp5Scenario {
    XmlAttrs attrs; // id, type, initial_chunk_count, label
    QString text;   // chunk list, e.g. "0-99" or "0 1 2"
};

struct Gp5ChunkInfo {
    XmlAttrs attrs;          // chunk_count, scenario_count
    XmlAttrs chunksAttrs;    // supported_languages, default_language (may be empty)
    std::vector<Gp5Chunk> chunks;
    XmlAttrs scenariosAttrs; // default_id
    std::vector<Gp5Scenario> scenarios;
};

struct Gp5FileEntry {
    QString dst;
    QString src;
    std::optional<int> chunk;
    bool noCompression = false; // pfs_compression="disable"
};

struct Gp5Project {
    QString volumeType = QStringLiteral("prospero_app");
    QString passcode;
    QString contentId;
    QString entitlementKey; // add-on volumes only
    std::optional<Gp5ChunkInfo> chunkInfo;
    std::vector<Gp5FileEntry> files;
    QStringList emptyDirs;
};

XmlElement gp5ToXml(const Gp5Project& project);
// The exact bytes written to <stem>.gp5 (CRLF, no BOM, no final line break).
QByteArray writeGp5(const Gp5Project& project);

// ---- a user's .gp5 used as the build source -----------------------------------
struct Gp5Source {
    QString path;
    QString volumeType;
    QString passcode;
    QString contentId;
    QString entitlementKey;
    bool hasChunkInfo = false;
    bool hasRootDir = false;   // "rootdir" style projects (not supported)
    std::vector<Gp5FileEntry> files; // src as written in the project
    QStringList emptyDirs;
};

Result<Gp5Source> readGp5(const QString& path);

// Resolves a src_path of a project: absolute as written, otherwise relative to
// the project's folder.
QString resolveGp5SourcePath(const Gp5Source& project, const QString& src);

} // namespace fw::fpkg
