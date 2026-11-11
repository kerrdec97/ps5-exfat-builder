#include "app/pages/ps5/ConsoleFolderPicker.h"

#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "connect/ps5/FtpClient.h"
#include "connect/ps5/FtpTransfer.h"
#include "connect/ps5/RemotePath.h"
#include "core/Tr.h"

#include <QCoreApplication>
#include <QListWidget>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <memory>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

struct Listed {
    QString path;
    QStringList folders;
    bool looksLikeApp0 = false;
    QString error;
};

Listed listFolders(const ps5::ConsoleEndpoint& ep, const QString& path)
{
    Listed out;
    out.path = path;
    ps5::FtpClient c(ps5::ftpOptionsFor(ep));
    if (auto st = c.open(ep.host, ep.ftpPort); !st.ok()) {
        out.error = st.error().message;
        return out;
    }
    auto r = c.list(path);
    c.quit();
    if (!r.ok()) {
        out.error = r.error().message;
        return out;
    }
    for (const ps5::FtpEntry& e : r.value()) {
        if (e.name.compare(QLatin1String("eboot.bin"), Qt::CaseInsensitive) == 0
            || e.name.compare(QLatin1String("sce_sys"), Qt::CaseInsensitive) == 0)
            out.looksLikeApp0 = true;
        if (e.isDir && !e.isLink && ps5::remoteNameProblem(e.name).isEmpty())
            out.folders << e.name;
    }
    std::sort(out.folders.begin(), out.folders.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    return out;
}

} // namespace

QString pickConsoleFolder(QWidget* host, const ps5::ConsoleEndpoint& ep, const QString& start, const QString& title,
                          const QString& actionText)
{
    auto* sheet = new Sheet(host, title, Sheet::Icon::None, size::dialogWide);
    auto* top = new QWidget(sheet);
    auto* th = hbox(top, space::sm);
    auto* up = new IconButton(QStringLiteral("arrow_up"), FW_TR("Up one level"), IconButton::Size::Small, top);
    auto* pathLabel = new ElideLabel(QString(), Qt::ElideMiddle, top);
    pathLabel->setFont(font::mono());
    keepLeftToRight(pathLabel);
    setTextColor(pathLabel, color::fg_0);
    th->addWidget(up);
    th->addWidget(pathLabel, 1);
    sheet->body()->addWidget(top);
    auto* list = new QListWidget(sheet);
    list->setMinimumHeight(8 * size::row);
    list->setAccessibleName(FW_TR("Folders on the console"));
    sheet->body()->addWidget(list);
    auto* status = new MessageRow(sheet);
    sheet->body()->addWidget(status);
    sheet->addButton(FW_TR("Cancel"), Sheet::Role::Safe);
    QPushButton* choose = sheet->addButton(actionText, Sheet::Role::Primary);

    auto current = std::make_shared<QString>(ps5::normalizeRemotePath(start.isEmpty() ? QStringLiteral("/") : start));
    auto generation = std::make_shared<int>(0);
    QPointer<Sheet> guard(sheet);
    const auto open = [=](const QString& path) {
        const int gen = ++*generation;
        *current = path;
        pathLabel->setFullText(path);
        list->clear();
        list->setEnabled(false);
        choose->setEnabled(false); // nothing to choose until the folder is listed
        status->set(FW_TR("Listing…"), Tone::Info);
        up->setEnabled(path != u"/");
        (void)QtConcurrent::run([=]() {
            const Listed l = listFolders(ep, path);
            QMetaObject::invokeMethod(QCoreApplication::instance(), [=]() {
                if (!guard || gen != *generation)
                    return;
                list->setEnabled(true);
                if (!l.error.isEmpty()) {
                    status->set(l.error, Tone::Err); // a folder that cannot be listed is not offered
                    return;
                }
                choose->setEnabled(true);
                for (const QString& f : l.folders) {
                    auto* it = new QListWidgetItem(icon(QStringLiteral("folder"), color::fg_3), f, list);
                    it->setFont(font::mono());
                }
                if (l.looksLikeApp0)
                    status->set(FW_TR("This folder has eboot.bin or sce_sys: a game's app0 folder."), Tone::Ok);
                else if (l.folders.isEmpty())
                    status->set(FW_TR("No sub-folders here."), Tone::Neutral);
                else
                    status->clear();
            }, Qt::QueuedConnection);
        });
    };
    // A selected (not opened) sub-folder is the choice; otherwise the open folder.
    const auto chosen = [=]() -> QString {
        if (list->currentItem() && list->currentItem()->isSelected())
            if (const auto child = ps5::remoteChild(*current, list->currentItem()->text()))
                return *child;
        return *current;
    };
    QObject::connect(up, &QToolButton::clicked, sheet, [=] { open(ps5::remoteParent(*current)); });
    // Double-click or Enter opens a folder (one signal for both: the list is rebuilt in the slot).
    QObject::connect(list, &QListWidget::itemActivated, sheet, [=](QListWidgetItem* it) {
        if (const auto child = ps5::remoteChild(*current, it->text()))
            open(*child);
    });
    // The path shown is the folder the button returns.
    QObject::connect(list, &QListWidget::itemSelectionChanged, sheet, [=] { pathLabel->setFullText(chosen()); });
    open(*current);
    choose->setDefault(true);
    const int r = sheet->exec();
    if (r != 1 || !guard)
        return {};
    return chosen();
}

} // namespace fw::pages
