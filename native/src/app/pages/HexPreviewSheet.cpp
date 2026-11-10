#include "app/pages/HexPreviewSheet.h"

#include "app/pages/Page.h"
#include "core/FileUtil.h"

#include <QFileInfo>
#include <QFontMetrics>
#include <QScrollBar>
#include <QStyle>
#include <QTextDocument>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

// The file's name is stored in the image; one that reads as markup stays out
// of the title (the line under it shows the whole path literally).
QString previewTitle(const QString& path)
{
    const QString title = QStringLiteral("Preview · %1").arg(path.section(u'/', -1));
    return Qt::mightBeRichText(title) ? QStringLiteral("Preview") : title;
}

} // namespace

HexPreviewSheet::HexPreviewSheet(QWidget* host, const QString& image, const QString& path, qint64 knownSize)
    : Sheet(host, previewTitle(path), Icon::None, size::dialogWide),
      m_image(image), m_path(path), m_size(knownSize)
{
    m_reader = new HexSliceReader(this);
    connect(m_reader, &HexSliceReader::ready, this, &HexPreviewSheet::showSlice);

    auto* where = new ElideLabel(QStringLiteral("/%1 in %2").arg(path, QFileInfo(image).fileName()), Qt::ElideMiddle, this);
    where->setFont(font::mono());
    setTextColor(where, color::fg_1);
    where->setToolTip(QStringLiteral("/%1\n%2").arg(path, nativePath(image)));
    plainLabel(where);
    body()->addWidget(where);
    m_meta = caption({}, this);
    body()->addWidget(m_meta);

    m_errorBar = new InfoBar(Tone::Err, this);
    plainLabels(m_errorBar); // the reader's message quotes the path
    m_errorBar->hide();
    body()->addWidget(m_errorBar);

    m_view = new QPlainTextEdit(this);
    m_view->setReadOnly(true);
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_view->setUndoRedoEnabled(false);
    m_view->setAccessibleName(QStringLiteral("Hex preview"));
    // 16 bytes per line must fit the sheet without a horizontal scroll bar.
    // Panel margins, the QSS padding (10 px a side) and border, the document
    // margin and the vertical scroll bar.
    const int inner = size::dialogWide - 2 * space::xl - 2 * 10 - 2 - 2 * 4
                    - m_view->style()->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, m_view) - 2;
    const QString sample = hexDumpLines(QByteArray(kHexBytesPerLine, 'x'), 0);
    m_view->setFont(QFontMetrics(font::mono()).horizontalAdvance(sample) <= inner ? font::mono() : font::monoSmall());
    // The application QSS sets min/max-height on text edits, which would
    // override setFixedHeight() when the widget is polished.
    const int hostH = host ? host->height() : 900;
    m_view->setStyleSheet(QStringLiteral("QPlainTextEdit { min-height: %1px; max-height: %1px; }").arg(std::clamp(hostH - 340, 160, 440)));
    m_view->setPlaceholderText(QStringLiteral("Reading…"));
    body()->addWidget(m_view);

    auto* row = new QWidget(this);
    auto* h = hbox(row, space::sm);
    m_status = caption({}, row);
    h->addWidget(m_status, 1);
    m_more = button(QStringLiteral("Load more"), ButtonVariant::Secondary, QStringLiteral("arrow_down"), row);
    setButtonSmall(m_more, true);
    m_more->setToolTip(QStringLiteral("Show the next %1").arg(formatBytes(kHexPreviewChunk)));
    m_more->setEnabled(false);
    h->addWidget(m_more);
    body()->addWidget(row);
    connect(m_more, &QPushButton::clicked, this, &HexPreviewSheet::loadMore);

    addButton(QStringLiteral("Close"), Role::Safe);
    updateStatus();
}

void HexPreviewSheet::start()
{
    m_loaded = 0;
    m_ended = false;
    m_error.clear();
    m_view->clear();
    m_reader->read(m_image, m_path, 0, kHexPreviewChunk);
    updateStatus();
}

void HexPreviewSheet::loadMore()
{
    if (m_reader->isBusy() || !m_error.isEmpty() || m_ended || (m_size >= 0 && m_loaded >= m_size))
        return;
    m_reader->read(m_image, m_path, m_loaded, kHexPreviewChunk);
    updateStatus();
}

void HexPreviewSheet::showSlice(const HexSlice& s)
{
    if (!s.ok()) {
        m_error = s.error;
        m_errorBar->setTitle(QStringLiteral("Could not read this file"));
        m_errorBar->setMessage(s.error);
        m_errorBar->show();
        updateStatus();
        return;
    }
    m_size = s.fileSize;
    if (s.offset != m_loaded) // a stale or out-of-order slice
        return;
    if (!s.data.isEmpty()) {
        const QString lines = hexDumpLines(s.data, s.offset, hexOffsetDigits(m_size));
        QScrollBar* bar = m_view->verticalScrollBar();
        const int keep = bar->value();
        if (m_loaded == 0)
            m_view->setPlainText(lines);
        else
            m_view->appendPlainText(lines);
        bar->setValue(m_loaded == 0 ? 0 : keep);
    } else if (m_size > 0 && m_loaded < m_size) {
        m_ended = true; // nothing came although the file goes on: asking again would do nothing
    }
    m_loaded = s.end();
    updateStatus();
}

void HexPreviewSheet::updateStatus()
{
    QStringList meta;
    if (m_size >= 0)
        meta << QStringLiteral("%1 bytes").arg(formatCount(m_size)) << formatBytes(m_size);
    meta << QStringLiteral("read-only");
    m_meta->setText(meta.join(QStringLiteral(" · ")));
    const bool busy = m_reader->isBusy();
    QString status;
    if (!m_error.isEmpty())
        status = QStringLiteral("Nothing more can be shown.");
    else if (busy && m_loaded == 0)
        status = QStringLiteral("Reading…");
    else if (m_size == 0)
        status = QStringLiteral("The file is empty.");
    else if (m_ended)
        status = QStringLiteral("Nothing more can be read.");
    else if (m_loaded == 0)
        status = QString(); // nothing read yet: no "bytes 0–0"
    else if (m_size > 0)
        status = QStringLiteral("Showing bytes 0–%1 of %2%3")
                     .arg(formatCount(std::max<qint64>(0, m_loaded - 1)), formatCount(m_size),
                          busy ? QStringLiteral(" · reading…") : m_loaded >= m_size ? QStringLiteral(" · end of file") : QString());
    m_status->setText(status);
    m_more->setEnabled(!busy && m_error.isEmpty() && !m_ended && m_size > 0 && m_loaded < m_size);
}

} // namespace fw::pages
