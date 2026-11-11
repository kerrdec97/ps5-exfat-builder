#include "app/services/ImageTree.h"

#include "engines/exfat/ExfatReader.h"
#include "engines/ufs2/Ufs2Reader.h"
#include "core/Tr.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

namespace fw::ui {

static ImageTreeResult readExfat(const QString& image, const CancellationToken& token)
{
    ImageTreeResult out;
    out.format = QStringLiteral("exfat");
    auto reader = exfat::ExfatReader::open(image);
    if (!reader.ok()) {
        out.error = reader.error().message;
        return out;
    }
    out.label = reader.value()->volume().label;
    auto tree = reader.value()->readTree(token, false);
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

static ImageTreeResult readFfpkg(const QString& image, const CancellationToken& token)
{
    ImageTreeResult out;
    out.format = QStringLiteral("ffpkg");
    auto reader = ufs2::Ufs2Reader::open(image);
    if (!reader.ok()) {
        out.error = reader.error().message;
        return out;
    }
    QStringList skipped;
    auto tree = reader.value()->loadTree(token, &skipped);
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
        out.warnings << FW_TR("Skipped (link or special file): %1").arg(s);
    return out;
}

ImageTreeResult readImageTree(const QString& image, const CancellationToken& token)
{
    ImageTreeResult out;
    out.image = image;
    const QString ext = QFileInfo(image).suffix().toLower();
    if (!QFileInfo::exists(image)) {
        out.error = QStringLiteral("The image does not exist.");
        return out;
    }
    if (ext == QLatin1String("exfat"))
        out = readExfat(image, token);
    else if (ext == QLatin1String("ffpkg"))
        out = readFfpkg(image, token);
    else
        out.error = QStringLiteral("Only .exfat and .ffpkg images can be opened here.");
    out.image = image;
    return out;
}

ImageTreeReader::ImageTreeReader(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<ImageTreeResult>();
}

ImageTreeReader::~ImageTreeReader()
{
    if (m_cancel)
        m_cancel->cancel();
}

void ImageTreeReader::cancel()
{
    ++m_generation; // whatever the worker still posts is not for anyone
    if (m_cancel)
        m_cancel->cancel();
    m_cancel.reset();
    m_busy = false;
}

void ImageTreeReader::read(const QString& image)
{
    cancel();
    const quint64 gen = m_generation;
    m_busy = true;
    m_cancel = std::make_shared<CancellationSource>();
    const CancellationToken token = m_cancel->token();
    QPointer<ImageTreeReader> self(this);
    (void)QtConcurrent::run([self, image, gen, token] {
        const ImageTreeResult r = readImageTree(image, token);
        if (!self)
            return;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, r, gen] {
            if (!self || gen != self->m_generation)
                return;
            self->m_busy = false;
            self->m_cancel.reset();
            emit self->ready(r);
        }, Qt::QueuedConnection);
    });
}

} // namespace fw::ui
