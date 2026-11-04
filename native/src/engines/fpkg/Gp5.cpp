#include "engines/fpkg/Gp5.h"

#include "engines/fpkg/ParamJson.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QXmlStreamReader>

namespace fw::fpkg {

QString escapeXmlAttribute(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        switch (c.unicode()) {
        case u'&': out += QStringLiteral("&amp;"); break;
        case u'<': out += QStringLiteral("&lt;"); break;
        case u'>': out += QStringLiteral("&gt;"); break;
        case u'"': out += QStringLiteral("&quot;"); break;
        case u'\r': out += QStringLiteral("&#13;"); break;
        case u'\n': out += QStringLiteral("&#10;"); break;
        case u'\t': out += QStringLiteral("&#09;"); break;
        default: out += c;
        }
    }
    return out;
}

QString escapeXmlText(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s) {
        switch (c.unicode()) {
        case u'&': out += QStringLiteral("&amp;"); break;
        case u'<': out += QStringLiteral("&lt;"); break;
        case u'>': out += QStringLiteral("&gt;"); break;
        default: out += c;
        }
    }
    return out;
}

namespace {

// ET.indent(space="  ") followed by ElementTree serialisation.
void writeElement(QString& out, const XmlElement& e, int level)
{
    out += u'<';
    out += e.tag;
    for (const auto& [k, v] : e.attrs) {
        out += u' ';
        out += k;
        out += QStringLiteral("=\"");
        out += escapeXmlAttribute(v);
        out += u'"';
    }
    if (e.children.empty()) {
        if (e.text.isEmpty()) {
            out += QStringLiteral(" />");
            return;
        }
        out += u'>';
        out += escapeXmlText(e.text);
        out += QStringLiteral("</");
        out += e.tag;
        out += u'>';
        return;
    }
    out += u'>';
    const QString childIndent = u'\n' + QString(2 * (level + 1), u' ');
    // ET.indent keeps non-whitespace text; our models never mix text and children.
    out += e.text.trimmed().isEmpty() ? childIndent : escapeXmlText(e.text);
    for (size_t i = 0; i < e.children.size(); ++i) {
        writeElement(out, e.children[i], level + 1);
        out += (i + 1 < e.children.size()) ? childIndent : u'\n' + QString(2 * level, u' ');
    }
    out += QStringLiteral("</");
    out += e.tag;
    out += u'>';
}

} // namespace

QByteArray writeEtreeDocument(const XmlElement& root)
{
    QString out = QStringLiteral("<?xml version='1.0' encoding='utf-8'?>\n");
    writeElement(out, root, 0);
    return out.toUtf8();
}

XmlElement gp5ToXml(const Gp5Project& p)
{
    XmlElement root{QStringLiteral("psproject"), {{QStringLiteral("fmt"), QStringLiteral("gp5")}}, {}, {}};

    XmlElement volume{QStringLiteral("volume"), {}, {}, {}};
    volume.children.push_back({QStringLiteral("volume_type"), {}, p.volumeType, {}});
    XmlElement package{QStringLiteral("package"), {}, {}, {}};
    package.attrs.emplace_back(QStringLiteral("passcode"), p.passcode);
    package.attrs.emplace_back(QStringLiteral("content_id"), p.contentId);
    if (!p.entitlementKey.isEmpty())
        package.attrs.emplace_back(QStringLiteral("entitlement_key"), p.entitlementKey);
    volume.children.push_back(std::move(package));

    if (p.chunkInfo) {
        const Gp5ChunkInfo& ci = *p.chunkInfo;
        XmlElement info{QStringLiteral("chunk_info"), ci.attrs, {}, {}};
        XmlElement chunks{QStringLiteral("chunks"), ci.chunksAttrs, {}, {}};
        for (const Gp5Chunk& c : ci.chunks)
            chunks.children.push_back({QStringLiteral("chunk"), c.attrs, {}, {}});
        info.children.push_back(std::move(chunks));
        XmlElement scenarios{QStringLiteral("scenarios"), ci.scenariosAttrs, {}, {}};
        for (const Gp5Scenario& s : ci.scenarios)
            scenarios.children.push_back({QStringLiteral("scenario"), s.attrs, s.text, {}});
        info.children.push_back(std::move(scenarios));
        volume.children.push_back(std::move(info));
    }
    root.children.push_back(std::move(volume));

    XmlElement files{QStringLiteral("files"), {}, {}, {}};
    files.children.reserve(p.files.size() + size_t(p.emptyDirs.size()));
    for (const Gp5FileEntry& f : p.files) {
        XmlElement e{QStringLiteral("file"), {}, {}, {}};
        e.attrs.emplace_back(QStringLiteral("dst_path"), f.dst);
        e.attrs.emplace_back(QStringLiteral("src_path"), f.src);
        if (f.chunk)
            e.attrs.emplace_back(QStringLiteral("chunk"), QString::number(*f.chunk));
        if (f.noCompression)
            e.attrs.emplace_back(QStringLiteral("pfs_compression"), QStringLiteral("disable"));
        files.children.push_back(std::move(e));
    }
    for (const QString& d : p.emptyDirs)
        files.children.push_back({QStringLiteral("empty_dir"), {{QStringLiteral("dst_path"), d}}, {}, {}});
    root.children.push_back(std::move(files));
    return root;
}

QByteArray writeGp5(const Gp5Project& p)
{
    return toCrlf(writeEtreeDocument(gp5ToXml(p)));
}

// ---------------------------------------------------------------------------

Result<Gp5Source> readGp5(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return Error(ErrorCode::NotFound, QStringLiteral("The project %1 could not be opened.").arg(QFileInfo(path).fileName()),
                     f.errorString());
    Gp5Source src;
    src.path = QFileInfo(path).absoluteFilePath();
    QXmlStreamReader xml(&f);
    bool sawRoot = false;
    QStringList stack;
    while (!xml.atEnd()) {
        const auto tok = xml.readNext();
        if (tok == QXmlStreamReader::StartElement) {
            const QString name = xml.name().toString();
            const auto a = xml.attributes();
            if (stack.isEmpty()) {
                if (name != u"psproject" || a.value(QStringLiteral("fmt")) != u"gp5")
                    return Error(ErrorCode::CorruptData,
                                 QStringLiteral("%1 is not a GP5 project (expected <psproject fmt=\"gp5\">).")
                                     .arg(QFileInfo(path).fileName()));
                sawRoot = true;
            } else if (name == u"volume_type") {
                src.volumeType = xml.readElementText().trimmed();
                continue; // readElementText consumed the end element
            } else if (name == u"package") {
                src.passcode = a.value(QStringLiteral("passcode")).toString();
                src.contentId = a.value(QStringLiteral("content_id")).toString();
                src.entitlementKey = a.value(QStringLiteral("entitlement_key")).toString();
            } else if (name == u"chunk_info") {
                src.hasChunkInfo = true;
            } else if (name == u"rootdir") {
                src.hasRootDir = true;
            } else if (name == u"file") {
                Gp5FileEntry e;
                e.dst = a.value(QStringLiteral("dst_path")).toString();
                e.src = a.value(QStringLiteral("src_path")).toString();
                if (a.hasAttribute(QStringLiteral("chunk")))
                    e.chunk = a.value(QStringLiteral("chunk")).toInt();
                e.noCompression = a.value(QStringLiteral("pfs_compression")) == u"disable";
                src.files.push_back(std::move(e));
            } else if (name == u"empty_dir" || name == u"dir") {
                src.emptyDirs << a.value(QStringLiteral("dst_path")).toString();
            }
            stack << name;
        } else if (tok == QXmlStreamReader::EndElement) {
            if (!stack.isEmpty())
                stack.removeLast();
        }
    }
    if (xml.hasError())
        return Error(ErrorCode::CorruptData,
                     QStringLiteral("The project %1 is not valid XML (line %2).").arg(QFileInfo(path).fileName()).arg(xml.lineNumber()),
                     xml.errorString());
    if (!sawRoot)
        return Error(ErrorCode::CorruptData, QStringLiteral("%1 is empty.").arg(QFileInfo(path).fileName()));
    return src;
}

QString resolveGp5SourcePath(const Gp5Source& project, const QString& srcPath)
{
    QString s = srcPath;
#ifndef Q_OS_WIN
    // Projects written on Windows use backslashes; accept them on other systems (tests).
    if (!s.startsWith(u'/') && s.contains(u'\\'))
        s.replace(u'\\', u'/');
#endif
    if (QFileInfo(s).isAbsolute())
        return QDir::cleanPath(s);
    return QDir::cleanPath(QFileInfo(project.path).absoluteDir().filePath(s));
}

} // namespace fw::fpkg
