#include "app/MainWindow.h"

#include "app/AppContext.h"
#include "core/BuildInfo.h"

#include <QCloseEvent>
#include <QDir>
#include <QLabel>
#include <QMessageBox>
#include <QVBoxLayout>

namespace fw {

MainWindow::MainWindow(AppContext& ctx, QWidget* parent) : QMainWindow(parent), m_ctx(ctx)
{
    setWindowTitle(QStringLiteral("%1 — %2").arg(QLatin1String(build::kProductName), QLatin1String(build::kEditionName)));
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    auto* label = new QLabel(QStringLiteral("%1 %2").arg(QLatin1String(build::kProductName), QLatin1String(build::kVersion)), central);
    layout->addWidget(label);
    setCentralWidget(central);
    resize(1280, 800);
}

int MainWindow::screenshotAllPages(const QString& dir)
{
    QDir().mkpath(dir);
    const QPixmap pm = grab();
    return pm.save(QDir(dir).filePath(QStringLiteral("00-shell.png"))) ? 1 : 0;
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    if (m_ctx.queue().isBusy()) {
        const auto answer = QMessageBox::question(this, QStringLiteral("Stop running jobs?"),
                                                  QStringLiteral("Jobs are still running. Closing stops them; their partial output is removed."),
                                                  QMessageBox::Cancel | QMessageBox::Close, QMessageBox::Cancel);
        if (answer != QMessageBox::Close) {
            event->ignore();
            return;
        }
    }
    event->accept();
}

} // namespace fw
