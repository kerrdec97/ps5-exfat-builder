#pragma once

// What an image file is, for the Image tools strip and the Library's Images
// view: format, size, and the title / title ID / version read from the
// sce_sys/param.json (or param.sfo) and the icon0.png inside the image through
// the native readers (integration::openFileTree). When the image cannot be
// read that way (a PFS container holding an .exfat file, a damaged image, an
// FPKG package) the title ID and version come from the file name instead.
//
// Reads run on worker threads (QtConcurrent) and post their result back; an
// in-memory cache keyed by path, size and modification time makes rescans cheap.

#include "core/Cancellation.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

namespace fw::ui {

struct ImageSummary {
    QString path;
    QString format;          // "exfat" | "ffpkg" | "ffpfs" | "ffpfsc" | "pkg"
    qint64 bytes = 0;        // the file's size
    QDateTime modified;
    bool exists = false;
    QString title;
    QString titleId;         // PPSA01325
    QString version;         // 01.002.000
    QString contentId;
    QString coverPath;       // icon0.png copied out of the image (cache), "" when none
    qint64 files = -1;       // from the reader; -1 when not read
    qint64 dirs = -1;
    qint64 contentBytes = -1;
    QString inner;           // a PFS container holding one image file: that file's name
    QString packs;           // "zstd" / "LZ4" when the game's files are in asset packs
    bool fromImage = false;  // title / ID read from inside the image (not the file name)
    QString error;           // why the image could not be read (meta from the file name)

    QString formatName() const;   // "exFAT", "FFPKG", "FFPFS", "FFPFSC", "FPKG"
    QString displayTitle() const; // the title, else the file name without extension
    QString metaLine() const;     // "exFAT · 48.5 GB · PPSA01325 · v01.002.000"
};

// "exfat" / "ffpkg" / "ffpfs" / "ffpfsc" ("pkg" too when `withPackages`), "" otherwise.
QString imageFormatOf(const QString& path, bool withPackages = false);
QString imageFormatName(const QString& format);
QString imageFormatIcon(const QString& format); // icon name for strips and chips

// Synchronous (worker threads, tests). `readInside` false: file name only.
ImageSummary readImageSummary(const QString& path, bool readInside = true, const CancellationToken& token = {});
// The images in `folders` and in their direct sub-folders (depth 1), sorted by name.
QStringList findImageFiles(const QStringList& folders, const CancellationToken& token = {});
// Title ID / version / title guessed from a file name ("PPSA01325 Astro Bot (01.002).exfat").
void guessFromFileName(ImageSummary& s);

// One image at a time; read() supersedes an earlier request.
class ImageSummaryReader : public QObject {
    Q_OBJECT
public:
    explicit ImageSummaryReader(QObject* parent = nullptr);
    void read(const QString& path);
    bool isBusy() const { return m_busy; }
signals:
    void ready(const fw::ui::ImageSummary& summary);

private:
    quint64 m_generation = 0;
    bool m_busy = false;
};

// Folders of images; scan() supersedes an earlier scan.
class ImageScanner : public QObject {
    Q_OBJECT
public:
    explicit ImageScanner(QObject* parent = nullptr);
    ~ImageScanner() override;
    void scan(const QStringList& folders);
    bool isBusy() const { return m_busy; }
signals:
    void finished(const QList<fw::ui::ImageSummary>& images);

private:
    quint64 m_generation = 0;
    bool m_busy = false;
    std::unique_ptr<CancellationSource> m_cancel;
};

} // namespace fw::ui

Q_DECLARE_METATYPE(fw::ui::ImageSummary)
