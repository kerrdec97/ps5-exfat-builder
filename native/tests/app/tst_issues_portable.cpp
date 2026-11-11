// "How can I change the paths?": Settings › Storage says where the app keeps
// its files and makes a copy portable.
//
//  * portable::makePortable() against a temporary folder standing in for the
//    exe's: portable.txt comes last, data files are copied and never
//    overwritten or moved, a folder that cannot be written to is refused.
//  * The Storage card lists the folders of the (isolated) profile, offers the
//    button only for a copy started from the single-file exe, and does
//    nothing when the sheet is cancelled.
//  * Settings › Tools points at Storage.

#include "app/AppContext.h"
#include "app/pages/SettingsPage.h"
#include "app/services/PortableMode.h"
#include "app/ui/Sheet.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "core/Paths.h"
#include "engines/general/Module.h"
#include "support/TestSupport.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <functional>
#include <memory>

using namespace fw;

namespace {

QByteArray readFile(const QString& p)
{
    QFile f(p);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QPushButton* buttonByText(QWidget* root, const QString& text)
{
    for (auto* b : root->findChildren<QPushButton*>())
        if (b->text() == text)
            return b;
    return nullptr;
}

// Takes away (or gives back) the right to create files in `dir`. False when
// this machine cannot do that.
bool denyWriting(const QString& dir, bool deny)
{
#ifdef Q_OS_WIN
    QProcess p;
    p.start(QStringLiteral("icacls"),
            {QDir::toNativeSeparators(dir), deny ? QStringLiteral("/deny") : QStringLiteral("/remove:d"),
             deny ? QStringLiteral("*S-1-1-0:(WD,AD)") : QStringLiteral("*S-1-1-0")});
    return p.waitForFinished(20000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
#else
    return QFile::setPermissions(dir, deny ? QFileDevice::ReadOwner | QFileDevice::ExeOwner
                                           : QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
}

// Runs `act` on the next sheet that opens in `host` (a sheet runs its own
// event loop, so the visit comes from a timer inside it).
[[nodiscard]] std::shared_ptr<bool> onNextSheet(QWidget* host, std::function<void(ui::Sheet*)> act)
{
    auto met = std::make_shared<bool>(false);
    auto* watch = new QTimer(host);
    watch->setInterval(30);
    QObject::connect(watch, &QTimer::timeout, host, [watch, host, met, act = std::move(act)] {
        for (auto* s : host->findChildren<ui::Sheet*>()) {
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

QString sheetText(ui::Sheet* sheet)
{
    QStringList parts;
    for (auto* l : sheet->findChildren<QLabel*>())
        parts << l->text();
    return parts.join(u'\n');
}

} // namespace

class TstIssuesPortable : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    int m_n = 0;

    // A folder with a stand-in for the single-file exe, and a data folder as the app leaves one.
    struct Stage {
        QString exe, exeDir, data;
    };
    Stage stage()
    {
        Stage s;
        const QString base = m_root.filePath(QStringLiteral("stage%1").arg(++m_n));
        s.exeDir = base + QStringLiteral("/USB drive");
        s.exe = s.exeDir + QStringLiteral("/exFAT Image Builder.exe");
        s.data = base + QStringLiteral("/roaming");
        fwtest::writeTextFile(s.exe, "MZ");
        fwtest::writeTextFile(s.data + QStringLiteral("/settings.json"), "{\"settings\":1}");
        fwtest::writeTextFile(s.data + QStringLiteral("/queue.json"), "{\"queue\":1}");
        fwtest::writeTextFile(s.data + QStringLiteral("/history.jsonl"), "{\"history\":1}\n");
        fwtest::writeTextFile(s.data + QStringLiteral("/library.json"), "{\"library\":1}");
        fwtest::writeTextFile(s.data + QStringLiteral("/app.lock"), "1234");
        fwtest::writeTextFile(s.data + QStringLiteral("/rename-undo/undo.json"), "{}");
        return s;
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        theme::apply(qApp);
    }

    void thePlanNamesTheFoldersNextToTheExe()
    {
        const Stage s = stage();
        const portable::Plan plan = portable::planFor(QDir::toNativeSeparators(s.exe), s.data);
        QCOMPARE(plan.exePath, s.exe);
        QCOMPARE(plan.exeDir, s.exeDir);
        QCOMPARE(plan.markerFile, s.exeDir + QStringLiteral("/portable.txt"));
        QCOMPARE(plan.programDir, s.exeDir + QStringLiteral("/exFAT Image Builder.data")); // where the launcher unpacks in portable mode
        QCOMPARE(plan.dataDir, s.exeDir + QStringLiteral("/data"));                        // where Paths keeps everything in portable mode
        QCOMPARE(plan.currentDataDir, s.data);
        // The files directly in the data folder; not the lock of the running app, not sub-folders.
        QCOMPARE(portable::filesToCopy(plan), (QStringList{QStringLiteral("history.jsonl"), QStringLiteral("library.json"),
                                                           QStringLiteral("queue.json"), QStringLiteral("settings.json")}));
        QVERIFY(!QFileInfo::exists(plan.markerFile)); // planning touches nothing
        QVERIFY(!QFileInfo::exists(plan.dataDir));
    }

    void copiesWithoutOverwritingAndCreatesTheMarkerLast()
    {
        const Stage s = stage();
        const portable::Plan plan = portable::planFor(s.exe, s.data);
        // A file from an earlier portable use of this folder stays as it is.
        QVERIFY(fwtest::writeTextFile(plan.dataDir + QStringLiteral("/settings.json"), "earlier"));
        const auto done = portable::makePortable(plan, true);
        QVERIFY2(done.ok(), done.ok() ? "" : qPrintable(done.error().toLogString()));
        QCOMPARE(done->copied, (QStringList{QStringLiteral("history.jsonl"), QStringLiteral("library.json"), QStringLiteral("queue.json")}));
        QCOMPARE(done->kept, (QStringList{QStringLiteral("settings.json")}));
        QCOMPARE(readFile(plan.dataDir + QStringLiteral("/settings.json")), QByteArray("earlier"));
        QCOMPARE(readFile(plan.dataDir + QStringLiteral("/queue.json")), QByteArray("{\"queue\":1}"));
        QCOMPARE(readFile(plan.dataDir + QStringLiteral("/history.jsonl")), QByteArray("{\"history\":1}\n"));
        QVERIFY(!QFileInfo::exists(plan.dataDir + QStringLiteral("/app.lock")));
        QVERIFY(!QFileInfo::exists(plan.dataDir + QStringLiteral("/rename-undo")));
        // Copied, never moved.
        for (const QString& name : {QStringLiteral("settings.json"), QStringLiteral("queue.json"), QStringLiteral("history.jsonl"),
                                    QStringLiteral("library.json"), QStringLiteral("app.lock"), QStringLiteral("rename-undo/undo.json")})
            QVERIFY2(QFileInfo::exists(s.data + u'/' + name), qPrintable(name));
        QCOMPARE(readFile(s.data + QStringLiteral("/settings.json")), QByteArray("{\"settings\":1}"));
        QVERIFY(QFileInfo(plan.markerFile).isFile());
        QVERIFY(readFile(plan.markerFile).contains("Delete this file to turn portable mode off."));
        // The temporary file of the write check is gone, and nothing else appeared.
        QStringList left = QDir(s.exeDir).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
        QCOMPARE(left, (QStringList{QStringLiteral("data"), QStringLiteral("exFAT Image Builder.exe"), QStringLiteral("portable.txt")}));

        // A second run changes nothing.
        const auto again = portable::makePortable(plan, true);
        QVERIFY(!again.ok());
        QCOMPARE(again.error().code, ErrorCode::AlreadyExists);
    }

    // The marker is what switches the next start: when anything before it
    // fails there is none, and the next start is as it was.
    void aFailedCopyLeavesNoMarker()
    {
        const Stage s = stage();
        const portable::Plan plan = portable::planFor(s.exe, s.data);
        QVERIFY(fwtest::writeTextFile(plan.dataDir, "a file where the data folder should be"));
        const auto done = portable::makePortable(plan, true);
        QVERIFY(!done.ok());
        QCOMPARE(done.error().code, ErrorCode::IoError);
        QVERIFY(!QFileInfo::exists(plan.markerFile));
        QCOMPARE(readFile(plan.dataDir), QByteArray("a file where the data folder should be"));
        QVERIFY(QFileInfo::exists(s.data + QStringLiteral("/settings.json")));
    }

    void withoutTheCopyOnlyTheMarkerIsCreated()
    {
        const Stage s = stage();
        const portable::Plan plan = portable::planFor(s.exe, s.data);
        const auto done = portable::makePortable(plan, false);
        QVERIFY2(done.ok(), done.ok() ? "" : qPrintable(done.error().toLogString()));
        QVERIFY(done->copied.isEmpty() && done->kept.isEmpty());
        QVERIFY(QFileInfo(plan.markerFile).isFile());
        QVERIFY(!QFileInfo::exists(plan.dataDir));
    }

    void aFolderThatCannotBeWrittenToIsRefused()
    {
        // A folder that is not there.
        {
            const portable::Plan plan = portable::planFor(m_root.filePath(QStringLiteral("nowhere/exFAT Image Builder.exe")), m_root.path());
            const auto done = portable::makePortable(plan, true);
            QVERIFY(!done.ok());
            QCOMPARE(done.error().code, ErrorCode::NotFound);
            QVERIFY(!QFileInfo::exists(plan.exeDir));
        }
        const Stage s = stage();
        const portable::Plan plan = portable::planFor(s.exe, s.data);
        if (!denyWriting(s.exeDir, true))
            QSKIP("This machine cannot take the write permission off a folder.");
        const auto restore = qScopeGuard([&] { (void)denyWriting(s.exeDir, false); });
        {
            QFile probe(s.exeDir + QStringLiteral("/probe.txt"));
            if (probe.open(QIODevice::WriteOnly)) {
                probe.close();
                probe.remove();
                QSKIP("This account writes to the folder all the same (administrator, root).");
            }
        }
        const auto done = portable::makePortable(plan, true);
        QVERIFY(!done.ok());
        QCOMPARE(done.error().code, ErrorCode::PermissionDenied);
        QVERIFY2(done.error().message.contains(QDir::toNativeSeparators(s.exeDir)), qPrintable(done.error().message));
        QVERIFY(!QFileInfo::exists(plan.markerFile));
        QVERIFY(!QFileInfo::exists(plan.dataDir));
    }

    // Settings › Storage lists where this (isolated) profile keeps its files.
    void theStorageCardListsTheProfilesFolders()
    {
        qunsetenv("FW_LAUNCHER_EXE");
        qunsetenv("FW_LAUNCHER_DIR");
        AppContext ctx;
        QVERIFY(ctx.initialize().ok());
        pages::SettingsPage page(ctx);
        page.resize(1200, 900);
        page.show();
        page.showSection(QStringLiteral("Storage"));
        auto* card = page.findChild<QWidget*>(QStringLiteral("locations-card"));
        QVERIFY(card);
        const auto pathOf = [&card](const QString& key) -> ui::ElideLabel* {
            auto* row = card->findChild<QWidget*>(QStringLiteral("location-") + key);
            return row ? row->findChild<ui::ElideLabel*>(QStringLiteral("locationPath")) : nullptr;
        };
        const auto native = [](const QString& p) { return QDir::toNativeSeparators(p); };
        QVERIFY(pathOf(QStringLiteral("program")) && pathOf(QStringLiteral("data")) && pathOf(QStringLiteral("logs")) && pathOf(QStringLiteral("temp")));
        QCOMPARE(pathOf(QStringLiteral("program"))->fullText(), native(QCoreApplication::applicationDirPath()));
        QCOMPARE(pathOf(QStringLiteral("data"))->fullText(), native(Paths::dataDir()));
        QCOMPARE(pathOf(QStringLiteral("logs"))->fullText(), native(Paths::logsDir()));
        QCOMPARE(pathOf(QStringLiteral("temp"))->fullText(), native(Paths::tempRoot()));
        QCOMPARE(pathOf(QStringLiteral("cache"))->fullText(), native(Paths::cacheDir()));
        // Every one of them is inside the test's own folder, and the whole path is the tooltip.
        for (const QString& key : {QStringLiteral("data"), QStringLiteral("logs"), QStringLiteral("temp"), QStringLiteral("cache")}) {
            QVERIFY2(pathOf(key)->fullText().startsWith(native(m_root.path())), qPrintable(pathOf(key)->fullText()));
            QVERIFY(pathOf(key)->toolTip().contains(QFileInfo(m_root.path()).fileName()));
            QVERIFY(buttonByText(card->findChild<QWidget*>(QStringLiteral("location-") + key), QStringLiteral("Show in folder")));
        }
        // The temporary folder chosen in this section is the one that is listed.
        const QString ownTemp = m_root.filePath(QStringLiteral("my temp"));
        QVERIFY(QDir().mkpath(ownTemp));
        QVERIFY(ctx.settings().setValue(general::keys::kTempFolder, ownTemp).ok());
        QCOMPARE(pathOf(QStringLiteral("temp"))->fullText(), native(ownTemp));
        QVERIFY(ctx.settings().setValue(general::keys::kTempFolder, QString()).ok());
        card = page.findChild<QWidget*>(QStringLiteral("locations-card")); // the section may have been built anew
        QVERIFY(card);
        QCOMPARE(pathOf(QStringLiteral("temp"))->fullText(), native(Paths::tempRoot()));

        // Not started from the single-file exe: no "exe" row, portable mode is off and the button says why it is not offered.
        QVERIFY(!card->findChild<QWidget*>(QStringLiteral("location-exe")));
        auto* badge = card->findChild<ui::Badge*>(QStringLiteral("portable-badge"));
        QVERIFY(badge);
        QCOMPARE(badge->text(), QStringLiteral("Off"));
        auto* make = card->findChild<QPushButton*>(QStringLiteral("make-portable"));
        QVERIFY(make);
        QCOMPARE(make->text(), QStringLiteral("Make this copy portable…"));
        QVERIFY(!make->isEnabled());
        auto* why = card->findChild<QLabel*>(QStringLiteral("portable-note"));
        QVERIFY(why);
        QTRY_VERIFY(why->isVisible()); // a new card is shown once the event loop has run
        QVERIFY2(why->text().contains(QStringLiteral("developer run")), qPrintable(why->text()));
    }

    // Started from the single-file exe: the sheet says what will happen;
    // "Cancel" does nothing, "Make portable" copies, creates portable.txt and asks for a restart.
    void makingThisCopyPortableFromThePage()
    {
        const Stage s = stage();
        qputenv("FW_LAUNCHER_EXE", QDir::toNativeSeparators(s.exe).toUtf8());
        qputenv("FW_LAUNCHER_DIR", QDir::toNativeSeparators(s.exeDir).toUtf8());
        const auto envBack = qScopeGuard([] {
            qunsetenv("FW_LAUNCHER_EXE");
            qunsetenv("FW_LAUNCHER_DIR");
        });
        AppContext ctx;
        QVERIFY(ctx.initialize().ok());
        QVERIFY(ctx.settings().save().ok());
        QVERIFY(QFileInfo::exists(Paths::settingsFile()));
        pages::SettingsPage page(ctx);
        page.resize(1200, 900);
        page.show();
        page.showSection(QStringLiteral("Storage"));
        auto* card = page.findChild<QWidget*>(QStringLiteral("locations-card"));
        QVERIFY(card);
        auto* exeRow = card->findChild<QWidget*>(QStringLiteral("location-exe"));
        QVERIFY(exeRow);
        QCOMPARE(exeRow->findChild<ui::ElideLabel*>(QStringLiteral("locationPath"))->fullText(), QDir::toNativeSeparators(s.exe));
        auto* make = card->findChild<QPushButton*>(QStringLiteral("make-portable"));
        QVERIFY(make && make->isEnabled());
        const QString marker = s.exeDir + QStringLiteral("/portable.txt");

        // Cancel: nothing happens.
        {
            const auto met = onNextSheet(&page, [&](ui::Sheet* sheet) {
                const QString text = sheetText(sheet);
                QVERIFY2(text.contains(QStringLiteral("portable.txt")), qPrintable(text));
                QVERIFY(text.contains(QDir::toNativeSeparators(s.exe).toHtmlEscaped()));
                QVERIFY(text.contains(QDir::toNativeSeparators(s.exeDir + QStringLiteral("/exFAT Image Builder.data")).toHtmlEscaped()));
                QVERIFY(text.contains(QDir::toNativeSeparators(s.exeDir + QStringLiteral("/data")).toHtmlEscaped()));
                QVERIFY(text.contains(QStringLiteral("left in place")) && text.contains(QStringLiteral("restarted")));
                auto* copy = sheet->findChild<QCheckBox*>();
                QVERIFY(copy);
                QCOMPARE(copy->text(), QStringLiteral("Copy my settings and queue"));
                QVERIFY(copy->isChecked());
                QVERIFY(buttonByText(sheet, QStringLiteral("Make portable")));
                buttonByText(sheet, QStringLiteral("Cancel"))->click();
            });
            make->click(); // blocks in the sheet
            QVERIFY(*met);
            QVERIFY(!QFileInfo::exists(marker));
            QCOMPARE(QDir(s.exeDir).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot), (QStringList{QStringLiteral("exFAT Image Builder.exe")}));
        }

        // Make portable: the settings are copied, the marker is there, and a sheet asks for the restart.
        {
            bool told = false;
            std::shared_ptr<bool> second;
            const auto met = onNextSheet(&page, [&](ui::Sheet* sheet) {
                buttonByText(sheet, QStringLiteral("Make portable"))->click();
                second = onNextSheet(&page, [&](ui::Sheet* after) {
                    const QString text = sheetText(after);
                    told = text.contains(QStringLiteral("Portable from the next start")) && text.contains(QStringLiteral("start"))
                        && text.contains(QStringLiteral("settings.json"));
                    buttonByText(after, QStringLiteral("OK"))->click();
                });
            });
            make = card->findChild<QPushButton*>(QStringLiteral("make-portable"));
            make->click();
            QVERIFY(*met);
            QVERIFY(second && *second);
            QVERIFY(told);
        }
        QVERIFY(QFileInfo(marker).isFile());
        QCOMPARE(readFile(s.exeDir + QStringLiteral("/data/settings.json")), readFile(Paths::settingsFile()));
        QVERIFY(QFileInfo::exists(Paths::settingsFile())); // copied, not moved
        // The row now says the switch comes with the restart, shows the file, and offers the button no more.
        card = page.findChild<QWidget*>(QStringLiteral("locations-card"));
        QVERIFY(card);
        QCOMPARE(card->findChild<ui::Badge*>(QStringLiteral("portable-badge"))->text(), QStringLiteral("After restart"));
        QVERIFY(!card->findChild<QPushButton*>(QStringLiteral("make-portable")));
        bool markerShown = false;
        for (auto* l : card->findChild<QWidget*>(QStringLiteral("portable-row"))->findChildren<ui::ElideLabel*>())
            markerShown = markerShown || l->fullText() == QDir::toNativeSeparators(marker);
        QVERIFY(markerShown);
    }

    // Settings › Tools: one line under the intro says where the components are, and opens Storage.
    void theToolsSectionPointsAtStorage()
    {
        AppContext ctx;
        QVERIFY(ctx.initialize().ok());
        pages::SettingsPage page(ctx);
        page.resize(1200, 900);
        page.show();
        const QString text = QStringLiteral("Bundled components live in the unpacked program folder (Settings › Storage shows where).");
        ui::Link* link = nullptr;
        for (auto* l : page.findChildren<ui::Link*>())
            if (l->text() == text)
                link = l;
        QVERIFY(link);
        page.showSection(QStringLiteral("General"));
        QVERIFY(!link->isVisible());
        page.showSection(QStringLiteral("Tools"));
        QVERIFY(link->isVisible());
        emit link->activated();
        QTRY_COMPARE(page.currentSection(), QStringLiteral("Storage"));
        QVERIFY(!link->isVisible());
        QVERIFY(page.findChild<QWidget*>(QStringLiteral("locations-card")));
    }
};

QTEST_MAIN(TstIssuesPortable)
#include "tst_issues_portable.moc"
