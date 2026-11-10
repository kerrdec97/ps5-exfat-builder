// Library: what users asked for and could not find.
//
//  * Images: renaming is on the page itself ("Rename images…" in the tool row
//    for the whole list, "Rename…" in the selection bar), and the sheet names
//    files "PPSA01234 - Title" or after a template, with the new names shown
//    as they will be.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/LibraryPage.h"
#include "app/services/ImageInfo.h"
#include "app/ui/LibraryGrid.h"
#include "app/ui/Sheet.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <memory>

using namespace fw;

namespace {

QPushButton* buttonByText(QWidget* root, const QString& text)
{
    for (auto* b : root->findChildren<QPushButton*>())
        if (b->text() == text)
            return b;
    return nullptr;
}

// Runs `act` on the next sheet that opens in `window` (a sheet runs its own
// event loop, so the visit comes from a timer inside it). The flag says
// whether a sheet was met.
[[nodiscard]] std::shared_ptr<bool> onNextSheet(QWidget* window, std::function<void(ui::Sheet*)> act)
{
    auto met = std::make_shared<bool>(false);
    auto* watch = new QTimer(window);
    watch->setInterval(30);
    QObject::connect(watch, &QTimer::timeout, window, [watch, window, met, act = std::move(act)] {
        for (auto* s : window->findChildren<ui::Sheet*>()) {
            if (!s->isOpen())
                continue;
            watch->stop();
            watch->deleteLater();
            *met = true;
            const QPointer<ui::Sheet> open(s);
            act(s);
            if (open && open->isOpen())
                open->close(-1); // a failed check inside `act` must not leave the test waiting in the sheet
            return;
        }
    });
    watch->start();
    return met;
}

QStringList column(QTableWidget* table, int col)
{
    QStringList out;
    for (int r = 0; r < table->rowCount(); ++r)
        out << table->item(r, col)->text();
    return out;
}

} // namespace

class TstIssuesLibrary : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<AppContext> m_ctx;
    std::unique_ptr<MainWindow> m_window;

    pages::LibraryPage* library() { return qobject_cast<pages::LibraryPage*>(m_window->page(QStringLiteral("library"))); }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_root.filePath(QStringLiteral("qsettings")));
        theme::apply(qApp);
        m_ctx = std::make_unique<AppContext>();
        QVERIFY(m_ctx->initialize().ok());
        m_window = std::make_unique<MainWindow>(*m_ctx);
        m_window->resize(1440, 920);
        m_window->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_window.get()));
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_ctx.reset();
    }

    // "PPSAxx - Game Name.exfat", and any other shape through the template.
    void imageNamesFollowThePatternAndTheTemplate()
    {
        ui::ImageSummary s;
        s.path = QStringLiteral("D:/Images/PPSA01325.exfat");
        s.format = QStringLiteral("exfat");
        s.titleId = QStringLiteral("PPSA01325");
        s.title = QStringLiteral("Astro Bot");
        s.version = QStringLiteral("01.002.000");
        using pages::LibraryPage;
        QCOMPARE(LibraryPage::imageFileName(s, QStringLiteral("id-dash-title")), QStringLiteral("PPSA01325 - Astro Bot.exfat"));
        QCOMPARE(LibraryPage::imageFileName(s, QStringLiteral("id-title")), QStringLiteral("PPSA01325 Astro Bot.exfat"));
        QCOMPARE(LibraryPage::imageFileName(s, QStringLiteral("custom"), QStringLiteral("%TITLE% [%PPSA%] (%VERSION%) %TYPE%")),
                 QStringLiteral("Astro Bot [PPSA01325] (01.002.000) exFAT.exfat"));
        // A missing value leaves no empty brackets or stray dash behind.
        ui::ImageSummary bare = s;
        bare.title.clear();
        bare.version.clear();
        QCOMPARE(LibraryPage::imageFileName(bare, QStringLiteral("id-dash-title")), QStringLiteral("PPSA01325.exfat"));
        QCOMPARE(LibraryPage::imageFileName(bare, QStringLiteral("custom"), QStringLiteral("%PPSA% - %TITLE% (%VERSION%)")),
                 QStringLiteral("PPSA01325.exfat"));
        // A template that renders to nothing, and an image without a title ID, keep the file's name.
        QCOMPARE(LibraryPage::imageFileName(s, QStringLiteral("custom"), QString()), QStringLiteral("PPSA01325.exfat"));
        ui::ImageSummary unknown;
        unknown.path = QStringLiteral("D:/Images/my backup.ffpkg");
        unknown.format = QStringLiteral("ffpkg");
        QCOMPARE(LibraryPage::imageFileName(unknown, QStringLiteral("custom"), QStringLiteral("%PPSA% - %TITLE%")), QStringLiteral("my backup.ffpkg"));
    }

    // The Images tab offers renaming in plain sight, for the list and for a selection.
    void imagesTabOffersRename()
    {
        pages::LibraryPage* lib = library();
        QVERIFY(lib);
        m_window->navigate(QStringLiteral("library"));
        const QString dir = m_root.filePath(QStringLiteral("images"));
        // Not real images: the title ID, title and version then come from the file names.
        const QString alpha = dir + QStringLiteral("/PPSA00011 Alpha Game (01.000.000).exfat");
        const QString beta = dir + QStringLiteral("/PPSA00012 Beta Game (01.003.000).ffpkg");
        const QString gamma = dir + QStringLiteral("/PPSA00013 Gamma Game (02.000.000).exfat");
        for (const QString& f : {alpha, beta, gamma})
            QVERIFY(fwtest::writeRandomFile(f, 4096, 5));
        lib->dropUrls({QUrl::fromLocalFile(dir)});
        QTRY_VERIFY_WITH_TIMEOUT(!lib->isScanningImages() && lib->images().size() == 3, 20000);
        lib->setTab(pages::LibraryPage::ImagesTab);
        QTRY_COMPARE(lib->imagesView()->count(), 3);

        // Nothing selected: the tool row has "Rename images…", the selection bar is away.
        QPushButton* renameAll = buttonByText(lib, QStringLiteral("Rename images…"));
        QPushButton* renameSelected = buttonByText(lib, QStringLiteral("Rename…"));
        QVERIFY(renameAll && renameSelected);
        QTRY_VERIFY(renameAll->isVisible());
        QVERIFY(!renameSelected->isVisible());

        // It opens the sheet for every listed image; the names follow the pattern and the template at once.
        {
            const auto met = onNextSheet(m_window.get(), [&](ui::Sheet* sheet) {
                auto* table = sheet->findChild<QTableWidget*>();
                auto* pattern = sheet->findChild<ui::Segmented*>();
                auto* tpl = sheet->findChild<QLineEdit*>();
                QVERIFY(table && pattern && tpl);
                QCOMPARE(column(table, 1), (QStringList{QFileInfo(alpha).fileName(), QFileInfo(beta).fileName(), QFileInfo(gamma).fileName()}));
                QVERIFY(!tpl->isVisible()); // only with "Custom"
                pattern->setCurrentValue(QStringLiteral("id-dash-title"));
                QCOMPARE(column(table, 2), (QStringList{QStringLiteral("PPSA00011 - Alpha Game.exfat"), QStringLiteral("PPSA00012 - Beta Game.ffpkg"),
                                                        QStringLiteral("PPSA00013 - Gamma Game.exfat")}));
                pattern->setCurrentValue(QStringLiteral("custom"));
                QVERIFY(tpl->isVisible());
                QCOMPARE(tpl->text(), QStringLiteral("%PPSA% - %TITLE%"));
                tpl->setText(QStringLiteral("%TITLE% [%PPSA%]"));
                QCOMPARE(table->item(0, 2)->text(), QStringLiteral("Alpha Game [PPSA00011].exfat"));
                buttonByText(sheet, QStringLiteral("Cancel"))->click();
            });
            renameAll->click(); // blocks in the sheet
            QVERIFY(*met);
            QVERIFY(QFileInfo::exists(alpha) && QFileInfo::exists(beta) && QFileInfo::exists(gamma)); // cancelled: nothing renamed
        }

        // A selection brings the bar: its count, "Rename…", "Verify" and "Clear selection".
        lib->imagesView()->setSelectedKeys({alpha, gamma});
        QTRY_VERIFY(renameSelected->isVisible());
        QVERIFY(!renameAll->isVisible());
        QVERIFY(buttonByText(lib, QStringLiteral("Verify"))->isVisible());
        bool countShown = false, clearShown = false;
        for (auto* l : lib->findChildren<QLabel*>()) {
            countShown = countShown || (l->isVisible() && l->text() == QStringLiteral("2 selected"));
            clearShown = clearShown || (l->isVisible() && l->text() == QStringLiteral("Clear selection"));
        }
        QVERIFY(countShown && clearShown);
        {
            const auto met = onNextSheet(m_window.get(), [&](ui::Sheet* sheet) {
                auto* table = sheet->findChild<QTableWidget*>();
                QVERIFY(table);
                // The selected images only; the sheet opens the way it was left.
                QCOMPARE(column(table, 1), (QStringList{QFileInfo(alpha).fileName(), QFileInfo(gamma).fileName()}));
                QCOMPARE(column(table, 2), (QStringList{QStringLiteral("Alpha Game [PPSA00011].exfat"), QStringLiteral("Gamma Game [PPSA00013].exfat")}));
                buttonByText(sheet, QStringLiteral("Rename"))->click();
            });
            renameSelected->click();
            QVERIFY(*met);
        }
        QVERIFY(QFileInfo::exists(dir + QStringLiteral("/Alpha Game [PPSA00011].exfat")));
        QVERIFY(QFileInfo::exists(dir + QStringLiteral("/Gamma Game [PPSA00013].exfat")));
        QVERIFY(!QFileInfo::exists(alpha) && !QFileInfo::exists(gamma));
        QVERIFY(QFileInfo::exists(beta)); // not selected: untouched
        QTRY_VERIFY_WITH_TIMEOUT(!lib->isScanningImages() && lib->images().size() == 3, 20000);
        lib->imagesView()->clearSelection();
        QTRY_VERIFY(renameAll->isVisible());
        m_ctx->queue().clearFinished();
    }
};

QTEST_MAIN(TstIssuesLibrary)
#include "tst_issues_library.moc"
