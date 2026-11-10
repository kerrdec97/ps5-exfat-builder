#include "engines/exfat/ExfatAmpr.h"

#include "core/FileUtil.h"
#include "engines/exfat/ExfatFormat.h"
#include "engines/exfat/ExfatReader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace fw::exfat {

namespace {

// The root's ampr_emu.index (exFAT names are case-insensitive), or -1.
// `isDir`: look for a folder of that name instead.
int rootIndexNode(const SourceTree& tree, bool isDir = false)
{
    if (tree.nodes.empty())
        return -1;
    for (int c : tree.nodes.front().children) {
        const SourceNode& n = tree.nodes[size_t(c)];
        if (n.isDir == isDir && n.name.compare(pfs::kAmprIndexName, Qt::CaseInsensitive) == 0)
            return c;
    }
    return -1;
}

std::vector<pfs::AmprFile> amprFiles(const SourceTree& tree)
{
    std::vector<pfs::AmprFile> files;
    files.reserve(size_t(tree.fileCount));
    for (size_t i = 1; i < tree.nodes.size(); ++i) {
        const SourceNode& n = tree.nodes[i];
        if (!n.isDir)
            files.push_back({n.relativePath.toUtf8(), n.size, n.modified.toSecsSinceEpoch()});
    }
    return files;
}

Result<QString> saveIndex(const QString& tempDir, const QByteArray& index)
{
    const QString dir = QDir(tempDir).filePath(QStringLiteral("ampr-index"));
    if (!QDir().mkpath(dir))
        return Error(ErrorCode::IoError, QStringLiteral("Could not create the folder %1.").arg(QDir::toNativeSeparators(dir)));
    const QString file = QDir(dir).filePath(pfs::kAmprIndexName);
    FW_TRY(writeFileAtomic(file, index));
    return file;
}

} // namespace

QByteArray amprIndexFor(const SourceTree& tree)
{
    return pfs::buildAmprIndex(pfs::amprToolRows(amprFiles(tree)).rows);
}

pfs::AmprIndexDecision decideAmprIndex(const SourceTree& tree, pfs::AmprMode mode)
{
    // A folder of that name is in the way of the file: the game stays as it is.
    if (mode == pfs::AmprMode::Off || tree.nodes.empty() || rootIndexNode(tree, true) >= 0)
        return {};
    const std::vector<pfs::AmprFile> files = amprFiles(tree);
    const int own = rootIndexNode(tree);
    pfs::AmprIndexState state;
    state.hasLibrary = pfs::hasAmprLibrary(files);
    state.hasIndex = own >= 0;
    state.indexSize = own >= 0 ? tree.nodes[size_t(own)].size : 0;
    for (int c : tree.nodes.front().children) {
        const SourceNode& n = tree.nodes[size_t(c)];
        if (!n.isDir && pfs::isAmprPackIndexName(n.name))
            state.hasAssetPacks = true;
    }
    const auto fresh = [&files] { return pfs::buildAmprIndex(pfs::amprToolRows(files).rows); };
    const auto dumpIndex = [&]() -> Result<QByteArray> {
        const SourceNode& n = tree.nodes[size_t(own)];
        QFile f(toLongPath(n.absolutePath));
        if (!f.open(QIODevice::ReadOnly))
            return Error(ErrorCode::IoError, QStringLiteral("Could not read %1.").arg(QDir::toNativeSeparators(n.absolutePath)),
                         f.errorString());
        return f.read(n.size + 1);
    };
    return pfs::decideAmprIndex(mode, state, fresh, dumpIndex);
}

void placeAmprIndex(SourceTree& tree, const QString& indexFile)
{
    if (tree.nodes.empty())
        return;
    const QFileInfo fi(indexFile);
    const int own = rootIndexNode(tree);
    if (own >= 0) {
        SourceNode& n = tree.nodes[size_t(own)];
        tree.totalBytes += fi.size() - n.size;
        n.absolutePath = fi.absoluteFilePath();
        n.size = fi.size();
        n.modified = n.accessed = fi.lastModified();
        return;
    }
    SourceNode n;
    n.name = pfs::kAmprIndexName;
    n.relativePath = n.name;
    n.absolutePath = fi.absoluteFilePath();
    n.size = fi.size();
    n.attributes = kAttrArchive;
    n.created = n.modified = n.accessed = fi.lastModified();
    n.parent = 0;
    // Where the scanner would have put it: by up-cased name, then by name.
    const UpcaseTable& up = UpcaseTable::recommended();
    const QString key = up.upcase(n.name);
    std::vector<int>& kids = tree.nodes.front().children;
    const auto at = std::find_if(kids.begin(), kids.end(), [&](int c) {
        const QString& other = tree.nodes[size_t(c)].name;
        const int byKey = up.upcase(other).compare(key, Qt::CaseSensitive);
        return byKey != 0 ? byKey > 0 : other.compare(n.name, Qt::CaseSensitive) > 0;
    });
    const int index = int(tree.nodes.size());
    kids.insert(at, index);
    ++tree.fileCount;
    tree.totalBytes += n.size;
    tree.nodes.push_back(std::move(n));
}

Result<AmprPlacement> applyAmprIndex(SourceTree& tree, pfs::AmprMode mode, const QString& tempDir)
{
    AmprPlacement out;
    const pfs::AmprIndexDecision d = decideAmprIndex(tree, mode);
    out.action = d.action;
    if (!pfs::amprActionChangesImage(d.action))
        return out;
    FW_TRY_ASSIGN(out.indexFile, saveIndex(tempDir, d.index));
    placeAmprIndex(tree, out.indexFile);
    return out;
}

bool acceptBuiltAmprIndex(SourceTree& tree, const QString& image, const QString& tempDir, const CancellationToken& token)
{
    if (tree.nodes.empty() || rootIndexNode(tree, true) >= 0 || !pfs::hasAmprLibrary(amprFiles(tree)))
        return false;
    const QByteArray fresh = amprIndexFor(tree);
    if (fresh.isEmpty())
        return false;
    auto reader = ExfatReader::open(image);
    if (!reader.ok())
        return false;
    auto listed = reader.value()->readTree(token, false);
    if (!listed.ok())
        return false;
    const ImageTree& it = listed.value();
    QByteArray inImage;
    for (int c : it.root().children) {
        const ImageEntry& e = it.entries[size_t(c)];
        if (e.isDir || e.name.compare(pfs::kAmprIndexName, Qt::CaseInsensitive) != 0)
            continue;
        if (e.size != fresh.size())
            return false; // not the index of these files
        const std::unique_ptr<ImageFileStream> stream = reader.value()->openStream();
        if (!stream->open().ok())
            return false;
        inImage.resize(qsizetype(e.size));
        qint64 done = 0;
        while (done < e.size) {
            const Result<qint64> n = stream->read(e, done, inImage.data() + done, e.size - done);
            if (!n.ok() || n.value() <= 0)
                return false;
            done += n.value();
        }
        break;
    }
    if (inImage != fresh && !pfs::amprIndexesDifferOnlyInTimes(inImage, fresh))
        return false;
    // The folder's own index, when it is the same, is compared as usual.
    if (const int own = rootIndexNode(tree); own >= 0 && tree.nodes[size_t(own)].size == inImage.size()) {
        QFile f(toLongPath(tree.nodes[size_t(own)].absolutePath));
        if (f.open(QIODevice::ReadOnly) && f.readAll() == inImage)
            return false;
    }
    auto saved = saveIndex(tempDir, inImage);
    if (!saved.ok())
        return false;
    placeAmprIndex(tree, saved.value());
    return true;
}

} // namespace fw::exfat
