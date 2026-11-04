#pragma once

// Application shell. (Foundation placeholder: the design-system shell with the
// sidebar, page header and every page replaces this in the UI milestone.)

#include <QMainWindow>

namespace fw {

class AppContext;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(AppContext& ctx, QWidget* parent = nullptr);

    // Renders every page into `<dir>/<index>-<page>.png`. Returns the count.
    int screenshotAllPages(const QString& dir);

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    AppContext& m_ctx;
};

} // namespace fw
