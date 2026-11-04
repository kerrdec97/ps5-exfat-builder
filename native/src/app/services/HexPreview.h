#pragma once

// Reads slices of a file inside an image for the Edit image page's hex
// preview (integration::readFileSlice: .exfat, .ffpkg, .ffpfs/.ffpfsc or a
// folder) on a worker thread, and formats them as offset / hex / ASCII lines.
// The image is opened and closed for every slice, so the preview never keeps
// a handle on an image that a queued edit may be about to replace.

#include <QByteArray>
#include <QObject>
#include <QString>

namespace fw::ui {

inline constexpr qint64 kHexPreviewChunk = 64 * 1024; // first slice and every "Load more"
inline constexpr int kHexBytesPerLine = 16;

struct HexSlice {
    QString image;
    QString path;          // inside the image, '/'-separated
    qint64 offset = 0;     // of data[0]
    qint64 fileSize = -1;  // -1 = unknown (the read failed)
    QByteArray data;
    QString error;         // non-empty: the read failed (user-facing)

    bool ok() const { return error.isEmpty(); }
    qint64 end() const { return offset + data.size(); }
};

// Hex digits the offset column needs for a file of `fileSize` bytes (at least 8).
int hexOffsetDigits(qint64 fileSize);

// One line per 16 bytes:
// "00000010  45 4c 46 02 01 01 00 00  00 00 00 00 00 00 00 00  |ELF.............|"
// The last line is padded so the ASCII column stays aligned.
QString hexDumpLines(const QByteArray& data, qint64 baseOffset, int offsetDigits = 8);

// Synchronous (worker threads, tests).
HexSlice readHexSlice(const QString& image, const QString& path, qint64 offset, qint64 length = kHexPreviewChunk);

class HexSliceReader : public QObject {
    Q_OBJECT
public:
    explicit HexSliceReader(QObject* parent = nullptr);
    // Reads on a worker thread; a newer read supersedes an older one's result.
    void read(const QString& image, const QString& path, qint64 offset, qint64 length = kHexPreviewChunk);
    bool isBusy() const { return m_busy; }
signals:
    void ready(const fw::ui::HexSlice& slice);

private:
    quint64 m_generation = 0;
    bool m_busy = false;
};

} // namespace fw::ui

Q_DECLARE_METATYPE(fw::ui::HexSlice)
