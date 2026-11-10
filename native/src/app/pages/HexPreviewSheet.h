#pragma once

// Read-only hex preview of one file inside an image (Image tools › Edit › Preview…):
// the first 64 KiB as offset / hex / ASCII, "Load more" appends the next
// 64 KiB. Bytes are read by services/HexPreview on a worker thread; the sheet
// only shows them.

#include "app/services/HexPreview.h"
#include "app/ui/Sheet.h"

#include <QPlainTextEdit>

namespace fw::pages {

class HexPreviewSheet : public ui::Sheet {
    Q_OBJECT
public:
    // `knownSize` < 0 when not known yet (shown once the first slice arrives).
    HexPreviewSheet(QWidget* host, const QString& image, const QString& path, qint64 knownSize = -1);

    void start();    // reads the first slice
    void loadMore(); // reads the next slice
    // Appends a slice (from the reader; the screenshot demo calls it directly).
    void showSlice(const ui::HexSlice& slice);

    QString image() const { return m_image; }
    QString path() const { return m_path; }
    qint64 loadedBytes() const { return m_loaded; }
    qint64 fileSize() const { return m_size; }
    bool isLoading() const { return m_reader->isBusy(); }
    QString text() const { return m_view->toPlainText(); }
    QPushButton* loadMoreButton() { return m_more; }
    QString errorText() const { return m_error; }

private:
    void updateStatus();

    QString m_image, m_path;
    qint64 m_size = -1;
    qint64 m_loaded = 0;
    bool m_ended = false; // a slice came back empty before the end of the file
    QString m_error;
    ui::HexSliceReader* m_reader;
    QPlainTextEdit* m_view;
    QLabel* m_meta;
    QLabel* m_status;
    ui::InfoBar* m_errorBar;
    QPushButton* m_more;
};

} // namespace fw::pages
