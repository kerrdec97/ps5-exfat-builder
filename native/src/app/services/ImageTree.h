#pragma once

// Reads the file tree of an .exfat or .ffpkg image on a worker thread
// (ExfatReader / Ufs2Reader) for the image editor. The result is a flat,
// format-neutral node list; nodes[0] is the root.

#include <QDateTime>
#include <QObject>
#include <QString>

#include <vector>

namespace fw::ui {

struct ImageNode {
    QString name;
    QString path;   // '/'-separated, no leading slash; empty for the root
    bool isDir = false;
    qint64 size = 0;
    QDateTime modified;
    int parent = -1;
    std::vector<int> children;
};

struct ImageTreeResult {
    QString image;
    QString format;      // "exfat" | "ffpkg"
    std::vector<ImageNode> nodes;
    qint64 totalBytes = 0;
    qint64 fileCount = 0;
    qint64 dirCount = 0;
    QString label;       // exFAT volume label
    QStringList problems;
    QStringList warnings;
    QString error;       // non-empty: the image could not be read
};

class ImageTreeReader : public QObject {
    Q_OBJECT
public:
    explicit ImageTreeReader(QObject* parent = nullptr);
    void read(const QString& image); // cancels any earlier read's result
    bool isBusy() const { return m_busy; }
signals:
    void ready(const fw::ui::ImageTreeResult& result);

private:
    quint64 m_generation = 0;
    bool m_busy = false;
};

// Synchronous (worker threads, tests).
ImageTreeResult readImageTree(const QString& image);

} // namespace fw::ui

Q_DECLARE_METATYPE(fw::ui::ImageTreeResult)
