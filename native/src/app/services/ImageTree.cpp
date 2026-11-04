#include "app/services/ImageTree.h"

#include "core/Cancellation.h"
#include "engines/exfat/ExfatReader.h"
#include "engines/ufs2/Ufs2Reader.h"

#include <QFileInfo>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

namespace fw::ui {

static ImageTreeResult readExfat(const QString& image)
{
    ImageTreeResult out;
    out.format = QStringLiteral("exfat");
    auto reader = exfat::ExfatReader::open(image);
    if (!reader.ok()) {
        out.error = reader.error().message;
        return out;
    }
    out.label = reader.value()->volume().label;
    auto tree = reader.value()->readTree(CancellationToken(), false);
    if (!tree.ok()) {
        out.error = tree.error().message;
        return out;
    }
    const exfat::ImageTree& t = tree.value();
    out.nodes.reserve(t.entries.size());
    for (const exfat::ImageEntry& e : t.entries) {
        ImageNode n;
        n.name = e.name;
        n.path = e.path;
        n.isDir = e.isDir;
        n.size = e.size;
        n.modified = e.modified;
        n.parent = e.parent;
        n.children = e.children;
        out.nodes.push_back(n);
    }
    out.totalBytes = t.totalBytes;
    out.fileCount = t.fileCount;
    out.dirCount = t.dirCount;
    out.problems = t.problems;
    out.warnings = t.warnings;
    return out;
}

static ImageTreeResult readFfpkg(const QString& image)
{
    ImageTreeResult out;
    out.format = QStringLiteral("ffpkg");
    auto reader = ufs2::Ufs2Reader::open(image);
    if (!reader.ok()) {
        out.error = reader.error().message;
        return out;
    }
    QStringList skipped;
    auto tree = reader.value()->loadTree(CancellationToken(), &skipped);
    if (!tree.ok()) {
        out.error = tree.error().message;
        return out;
    }
    const ufs2::SourceTree& t = tree.value();
    out.nodes.reserve(t.nodes.size());
    for (size_t i = 0; i < t.nodes.size(); ++i) {
        const ufs2::TreeNode& e = t.nodes[i];
        ImageNode n;
        n.name = e.name;
        n.path = t.relativePath(int(i));
        n.isDir = e.isDir();
        n.size = e.size;
        if (e.mtime > 0)
            n.modified = QDateTime::fromSecsSinceEpoch(e.mtime);
        n.parent = e.parent;
        n.children = e.children;
        out.nodes.push_back(n);
    }
    out.totalBytes = t.totalBytes;
    out.fileCount = t.fileCount;
    out.dirCount = t.dirCount;
    for (const QString& s : skipped)
        out.warnings << QStringLiteral("Skipped (link or special file): %1").arg(s);
    return out;
}

ImageTreeResult readImageTree(const QString& image)
{
    ImageTreeResult out;
    out.image = image;
    const QString ext = QFileInfo(image).suffix().toLower();
    if (!QFileInfo::exists(image)) {
        out.error = QStringLiteral("The image does not exist.");
        return out;
    }
    if (ext == QLatin1String("exfat"))
        out = readExfat(image);
    else if (ext == QLatin1String("ffpkg"))
        out = readFfpkg(image);
    else
        out.error = QStringLiteral("Only .exfat and .ffpkg images can be opened here.");
    out.image = image;
    return out;
}

ImageTreeReader::ImageTreeReader(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<ImageTreeResult>();
}

void ImageTreeReader::read(const QString& image)
{
    const quint64 gen = ++m_generation;
    m_busy = true;
    QPointer<ImageTreeReader> self(this);
    (void)QtConcurrent::run([self, image, gen] {
        const ImageTreeResult r = readImageTree(image);
        if (!self)
            return;
        QMetaObject::invokeMethod(self.data(), [self, r, gen] {
            if (!self || gen != self->m_generation)
                return;
            self->m_busy = false;
            emit self->ready(r);
        }, Qt::QueuedConnection);
    });
}

} // namespace fw::ui
