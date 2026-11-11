// Library: what users asked for and could not find.
//
//  * Images: renaming is on the page itself ("Rename images…" in the tool row
//    for the whole list, "Rename…" in the selection bar), and the sheet names
//    files "PPSA01234 - Title" or after a template, with the new names shown
//    as they will be.
//  * Games: "Check dump…" runs the read-only dump check for the selection;
//    the result comes as a toast with "Show details", the details sheet
//    groups the findings by severity, and the last result stays available.

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/DumpCheckSheet.h"
#include "app/pages/LibraryPage.h"
#include "app/services/ImageInfo.h"
#include "app/services/JobSubmit.h"
#include "app/ui/LibraryGrid.h"
#include "app/ui/Sheet.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSignalSpy>
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

// Every file under `dir` with its size and time stamp.
QMap<QString, QString> listing(const QString& dir)
{
    QMap<QString, QString> out;
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QFileInfo fi = it.nextFileInfo();
        out.insert(QDir(dir).relativeFilePath(fi.filePath()), QStringLiteral("%1|%2").arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch()));
    }
    return out;
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

    // The details sheet: verdict and facts on top, the findings grouped by
    // severity, a finding in full when it is selected, a plain-text report.
    void theDumpCheckSheetGroupsTheFindings()
    {
        const QJsonObject details = pages::DumpCheckSheet::demoDetails();
        const QString summary = QStringLiteral("2 problems and 3 warnings in 18,442 files");
        const QString folder = m_root.filePath(QStringLiteral("a dump"));
        auto* sheet = new pages::DumpCheckSheet(m_window.get(), details, summary, QStringLiteral("Astro Bot"), folder);
        sheet->open();
        QCOMPARE(sheet->groupTitles(), (QStringList{QStringLiteral("Problems · 2"), QStringLiteral("Warnings · 3"), QStringLiteral("Notes · 4")}));
        QTreeWidget* list = sheet->list();
        QCOMPARE(list->topLevelItem(0)->childCount(), 2);
        QCOMPARE(list->topLevelItem(0)->child(0)->text(0), QStringLiteral("sce_module/libSceNpToolkit2.prx"));
        QVERIFY(list->topLevelItem(0)->child(0)->text(1).startsWith(QStringLiteral("Encrypted retail executable")));
        // The verdict in the app's badges, and the facts.
        QStringList badges;
        for (auto* b : sheet->findChildren<ui::Badge*>())
            badges << b->text();
        for (const QString& expected : {QStringLiteral("2 problems"), QStringLiteral("3 warnings"), QStringLiteral("Problems · 2"),
                                        QStringLiteral("Warnings · 3"), QStringLiteral("Notes · 4")})
            QVERIFY2(badges.contains(expected), qPrintable(badges.join(u'|')));
        QStringList labels;
        for (auto* l : sheet->findChildren<QLabel*>())
            labels << l->text();
        const QString text = labels.join(u'\n');
        QVERIFY2(text.contains(QStringLiteral("PPSA01325 · v01.002.000 · PS5")), qPrintable(text));
        QVERIFY(text.contains(QStringLiteral("36 fake-signed · 1 plain ELF · 1 encrypted · 1 cut short")));
        QVERIFY(text.contains(QStringLiteral("Looks backported (fakelib present; executables target SDK 4.50).")));
        QVERIFY(text.contains(QStringLiteral("The check reads the dump and changes nothing.")));

        // Nothing selected: no folder to show. A finding: its file; a finding about the whole dump: the dump.
        QVERIFY(!sheet->showInFolderButton()->isEnabled());
        QVERIFY(sheet->selectedPath().isEmpty());
        list->topLevelItem(0)->child(1)->setSelected(true);
        QVERIFY(sheet->showInFolderButton()->isEnabled());
        QCOMPARE(sheet->selectedPath(), folder + QStringLiteral("/sce_module/libfmod.prx"));
        list->clearSelection();
        QTreeWidgetItem* notes = list->topLevelItem(2);
        QTreeWidgetItem* state = nullptr;
        QTreeWidgetItem* empties = nullptr;
        for (int i = 0; i < notes->childCount(); ++i) {
            if (notes->child(i)->text(1).startsWith(QStringLiteral("Looks backported")))
                state = notes->child(i);
            if (notes->child(i)->text(1).startsWith(QStringLiteral("3 files are empty")))
                empties = notes->child(i);
        }
        QVERIFY(state && empties);
        QCOMPARE(state->text(0), QStringLiteral("The dump"));
        state->setSelected(true);
        QCOMPARE(sheet->selectedPath(), folder);
        // A finding about many files lists the first of them underneath.
        QCOMPARE(empties->childCount(), 3);
        QCOMPARE(empties->child(0)->text(0), QStringLiteral("data/cache/a.tmp.bin"));
        list->clearSelection();
        empties->child(2)->setSelected(true);
        QCOMPARE(sheet->selectedPath(), folder + QStringLiteral("/data/log/empty.txt"));

        const QString report = sheet->report();
        QVERIFY2(report.startsWith(QStringLiteral("DUMP CHECK\nGame:    Astro Bot (PPSA01325 · v01.002.000 · PS5)\n")), qPrintable(report));
        QVERIFY(report.contains(QStringLiteral("Result:  2 problems and 3 warnings in 18,442 files")));
        QVERIFY(report.contains(QStringLiteral("PROBLEMS (2)\n  sce_module/libSceNpToolkit2.prx   Encrypted retail executable")));
        QVERIFY(report.contains(QStringLiteral("WARNINGS (3)\n")));
        QVERIFY(report.contains(QStringLiteral("  3 files are empty (0 bytes).\n      data/cache/a.tmp.bin\n")));
        QVERIFY(report.endsWith(QStringLiteral("The check reads the dump and changes nothing.\n")));

        // "Backport…" asks for the Backport tab and closes the sheet; "Copy report" keeps it open.
        QSignalSpy backport(sheet, &pages::DumpCheckSheet::backportRequested);
        buttonByText(sheet, QStringLiteral("Copy report"))->click();
        QVERIFY(sheet->isOpen());
        QVERIFY(!buttonByText(sheet, QStringLiteral("Copy report"))); // it says "Copied" for a moment
        QVERIFY(buttonByText(sheet, QStringLiteral("Copied")));
        buttonByText(sheet, QStringLiteral("Backport…"))->click();
        QCOMPARE(backport.count(), 1);
        QVERIFY(!sheet->isOpen());

        // Findings that were counted but not listed are said, in the sheet and in the report.
        QJsonObject many = details;
        QJsonObject counts = many.value(QStringLiteral("counts")).toObject();
        counts.insert(QStringLiteral("errors"), 150);
        many.insert(QStringLiteral("counts"), counts);
        auto* capped = new pages::DumpCheckSheet(m_window.get(), many, QStringLiteral("150 problems and 3 warnings in 18,442 files"), QString(), folder);
        QCOMPARE(capped->groupTitles().first(), QStringLiteral("Problems · 150"));
        QCOMPARE(capped->list()->topLevelItem(0)->childCount(), 3); // 2 listed + "… and 148 more"
        QVERIFY(capped->list()->topLevelItem(0)->child(2)->text(0).contains(QStringLiteral("148 more")));
        QVERIFY(capped->report().contains(QStringLiteral("… and 148 more that are counted but not listed")));
        capped->close(0);

        // A clean result says so.
        QJsonObject clean = details;
        clean.insert(QStringLiteral("findings"), QJsonArray());
        clean.insert(QStringLiteral("counts"), QJsonObject{{QStringLiteral("files"), 7}, {QStringLiteral("folders"), 4}, {QStringLiteral("bytes"), 1000}});
        auto* fine = new pages::DumpCheckSheet(m_window.get(), clean, QStringLiteral("No problems found in 7 files"), QStringLiteral("Astro Bot"), folder);
        badges.clear();
        for (auto* b : fine->findChildren<ui::Badge*>())
            badges << b->text();
        QCOMPARE(badges, QStringList{QStringLiteral("No problems found")});
        QVERIFY(fine->groupTitles().isEmpty());
        fine->close(0);
    }

    // Library › Games: "Check dump…" for the selection starts the job now; its
    // result is a toast with "Show details", and stays for "Show last check".
    void checkDumpFromTheGamesTab()
    {
        pages::LibraryPage* lib = library();
        QVERIFY(lib);
        QVERIFY(jobs::isAvailable(*m_ctx, QStringLiteral("tools.dump.check")));
        m_window->navigate(QStringLiteral("library"));
        lib->setTab(pages::LibraryPage::GamesTab);
        // A dump with one problem (eboot.bin is not an executable) and one warning (no cover).
        const QString game = m_root.filePath(QStringLiteral("dumps/PPSA00077 Dump Test"));
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_sys/param.json"),
                                      R"({"titleId":"PPSA00077","contentId":"UP9000-PPSA00077_00-ABCDEFGHIJKLMNOP","contentVersion":"01.000.000",
                                          "localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Dump Test"}}})"));
        QVERIFY(fwtest::writeRandomFile(game + QStringLiteral("/eboot.bin"), 5000, 41));
        QVERIFY(fwtest::writeRandomFile(game + QStringLiteral("/data/level.bin"), 20000, 42));
        const auto before = listing(game);
        lib->dropUrls({QUrl::fromLocalFile(game)});
        QString key;
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (const auto& item : lib->gamesView()->items())
                if (QFileInfo(item.key).fileName() == QStringLiteral("PPSA00077 Dump Test"))
                    key = item.key;
            return !key.isEmpty();
        }(), 20000);
        QVERIFY(!lib->hasDumpCheck(key));
        QCOMPARE(lib->showDumpCheck(key), nullptr);

        // The selection bar offers it.
        lib->gamesView()->setSelectedKeys({key});
        QPushButton* check = buttonByText(lib, QStringLiteral("Check dump…"));
        QVERIFY(check);
        QTRY_VERIFY(check->isVisible());
        QVERIFY(check->isEnabled());
        m_window->toasts()->clear();
        QSet<QString> known;
        for (const JobRecord& r : m_ctx->queue().records())
            known.insert(r.id);
        check->click();
        QString id;
        for (const JobRecord& r : m_ctx->queue().records())
            if (!known.contains(r.id) && r.kind == QLatin1String("tools.dump.check"))
                id = r.id;
        QVERIFY(!id.isEmpty());
        QCOMPARE(m_ctx->queue().record(id)->title, QStringLiteral("Check Dump Test"));
        QTRY_VERIFY_WITH_TIMEOUT(isTerminal(m_ctx->queue().record(id)->state), 60000);
        const auto rec = m_ctx->queue().record(id);
        QCOMPARE(rec->state, JobState::Succeeded); // the check did its job, whatever the dump is like
        QCOMPARE(rec->outcome->verification, Verification::Inconclusive);
        QCOMPARE(rec->outcome->summary, QStringLiteral("1 problem and 1 warning in 3 files"));
        QCOMPARE(listing(game), before); // nothing in the dump changed

        // The toast says what was found and opens the details.
        QTRY_VERIFY(lib->hasDumpCheck(key));
        ui::Link* details = nullptr;
        bool said = false;
        QTRY_VERIFY_WITH_TIMEOUT([&] {
            for (auto* l : m_window->findChildren<ui::Link*>())
                if (l->isVisible() && l->text() == QStringLiteral("Show details"))
                    details = l;
            for (auto* l : m_window->findChildren<QLabel*>())
                said = said || (l->isVisible() && l->text() == QStringLiteral("Checked Dump Test: 1 problem, 1 warning"));
            return details && said;
        }(), 5000);
        emit details->activated();
        pages::DumpCheckSheet* sheet = nullptr;
        for (auto* s : m_window->findChildren<pages::DumpCheckSheet*>())
            if (s->isOpen())
                sheet = s;
        QVERIFY(sheet);
        QCOMPARE(sheet->groupTitles().mid(0, 2), (QStringList{QStringLiteral("Problems · 1"), QStringLiteral("Warnings · 1")}));
        QCOMPARE(sheet->list()->topLevelItem(0)->child(0)->text(0), QStringLiteral("eboot.bin"));
        QCOMPARE(sheet->list()->topLevelItem(1)->child(0)->text(0), QStringLiteral("sce_sys/icon0.png"));
        sheet->list()->topLevelItem(0)->child(0)->setSelected(true);
        QCOMPARE(QDir::cleanPath(sheet->selectedPath()), QDir::cleanPath(key + QStringLiteral("/eboot.bin")));
        QVERIFY(sheet->report().contains(QStringLiteral("Game:    Dump Test (PPSA00077 · v01.000.000 · PS5)")));
        // "Backport…" hands the game to the Backport tab (the request is all this test looks at).
        QSignalSpy nav(lib, &pages::Page::navigateRequested);
        QObject::disconnect(lib, &pages::Page::navigateRequested, m_window.get(), nullptr);
        buttonByText(sheet, QStringLiteral("Backport…"))->click();
        QCOMPARE(nav.count(), 1);
        const auto request = nav.first().first().value<pages::NavRequest>();
        QCOMPARE(request.page, QStringLiteral("backport"));
        QCOMPARE(request.args.value(QStringLiteral("folder")).toString(), key);
        QVERIFY(!sheet->isOpen());

        // The result is kept for the session ("Show last check" in the game's menu).
        pages::DumpCheckSheet* again = lib->showDumpCheck(key);
        QVERIFY(again && again->isOpen());
        QCOMPARE(again->groupTitles().mid(0, 2), (QStringList{QStringLiteral("Problems · 1"), QStringLiteral("Warnings · 1")}));
        again->close(0);
        lib->gamesView()->clearSelection();
        m_window->toasts()->clear();
        m_ctx->queue().clearFinished();
    }
};

QTEST_MAIN(TstIssuesLibrary)
#include "tst_issues_library.moc"
