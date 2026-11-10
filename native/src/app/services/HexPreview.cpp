#include "app/services/HexPreview.h"

#include "engines/integration/ImageFiles.h"

#include <QCoreApplication>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

namespace fw::ui {

int hexOffsetDigits(qint64 fileSize)
{
    int digits = 8;
    while (digits < 16 && fileSize > 0 && (fileSize - 1) >> (digits * 4) != 0)
        ++digits;
    return digits;
}

QString hexDumpLines(const QByteArray& data, qint64 baseOffset, int offsetDigits)
{
    static const char kHex[] = "0123456789abcdef";
    QString out;
    const qsizetype n = data.size();
    const qsizetype lines = (n + kHexBytesPerLine - 1) / kHexBytesPerLine;
    out.reserve(lines * (offsetDigits + 2 + 3 * kHexBytesPerLine + 1 + 2 + kHexBytesPerLine + 2 + 1));
    for (qsizetype line = 0; line < lines; ++line) {
        const qsizetype start = line * kHexBytesPerLine;
        if (line > 0)
            out += u'\n';
        out += QStringLiteral("%1").arg(baseOffset + start, offsetDigits, 16, QLatin1Char('0'));
        out += QLatin1String("  ");
        QString ascii;
        for (int i = 0; i < kHexBytesPerLine; ++i) {
            if (i == kHexBytesPerLine / 2)
                out += u' ';
            if (start + i < n) {
                const auto b = quint8(data.at(start + i));
                out += QLatin1Char(kHex[b >> 4]);
                out += QLatin1Char(kHex[b & 0xF]);
                ascii += (b >= 0x20 && b < 0x7F) ? QLatin1Char(char(b)) : QLatin1Char('.');
            } else {
                out += QLatin1String("  ");
                ascii += u' ';
            }
            out += u' ';
        }
        out += u' ';
        out += u'|';
        out += ascii;
        out += u'|';
    }
    return out;
}

HexSlice readHexSlice(const QString& image, const QString& path, qint64 offset, qint64 length)
{
    HexSlice out;
    out.image = image;
    out.path = path;
    out.offset = offset;
    auto r = integration::readFileSlice(image, path, offset, length);
    if (!r.ok()) {
        out.error = r.error().message;
        return out;
    }
    out.offset = r.value().offset;
    out.fileSize = r.value().fileSize;
    out.data = r.value().data;
    return out;
}

HexSliceReader::HexSliceReader(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<HexSlice>();
}

void HexSliceReader::read(const QString& image, const QString& path, qint64 offset, qint64 length)
{
    const quint64 gen = ++m_generation;
    m_busy = true;
    QPointer<HexSliceReader> self(this);
    (void)QtConcurrent::run([self, image, path, offset, length, gen] {
        const HexSlice s = readHexSlice(image, path, offset, length);
        if (!self)
            return;
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, s, gen] {
            if (!self || gen != self->m_generation)
                return;
            self->m_busy = false;
            emit self->ready(s);
        }, Qt::QueuedConnection);
    });
}

} // namespace fw::ui
