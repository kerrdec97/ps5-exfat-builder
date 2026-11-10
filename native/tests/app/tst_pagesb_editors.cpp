// Library-side pieces that need no main window: the dlc_emu.ini editor (the
// keys without a table column stay with their entry; unsaved edits are not
// dropped without a question) and the rule that merges a library scan into the
// known list.

#include "app/pages/DlcEmuEditor.h"
#include "app/pages/LibraryPage.h"
#include "app/ui/Sheet.h"
#include "app/ui/Theme.h"
#include "support/TestSupport.h"

#include <QAbstractButton>
#include <QDir>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace fw;
namespace dlcemu = fw::tools::dlcemu;

namespace {

QString iniWith(const QStringList& labels)
{
    QString text;
    int n = 0;
    for (const QString& l : labels) {
        ++n;
        text += QStringLiteral("[PSAC]\r\ncontent_id=UP9000-PPSA00001_00-ADDON0000000000%1\r\nlabel=DLC%1\r\nentitlement_label=%2\r\nextra_key=x%1\r\n\r\n")
                    .arg(n)
                    .arg(l);
    }
    return text;
}

QAbstractButton* buttonByTip(QWidget* root, const QString& tip)
{
    for (auto* b : root->findChildren<QAbstractButton*>())
        if (b->toolTip() == tip)
            return b;
    return nullptr;
}

QPushButton* buttonByText(QWidget* root, const QString& text)
{
    for (auto* b : root->findChildren<QPushButton*>())
        if (b->text() == text)
            return b;
    return nullptr;
}

QStringList labelsOf(const std::vector<dlcemu::Entry>& entries)
{
    QStringList out;
    for (const auto& e : entries)
        out << e.get(QStringLiteral("entitlement_label"));
    return out;
}

ui::LibraryGame game(const QString& folder, const QString& status = {}, const QDateTime& seen = {})
{
    ui::LibraryGame g;
    g.info.folder = folder;
    g.info.isGame = true;
    g.status = status;
    g.lastSeen = seen;
    return g;
}

QStringList foldersOf(const std::vector<ui::LibraryGame>& games)
{
    QStringList out;
    for (const auto& g : games)
        out << g.info.folder;
    out.sort();
    return out;
}

} // namespace

class TstPagesbEditors : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;

    // Answers the next sheet of `host` that has a button `text` (a sheet runs
    // its own event loop, so the click comes from a timer inside it).
    static void answerNextSheet(QWidget* host, const QString& text, bool* answered)
    {
        auto* watch = new QTimer(host);
        watch->setInterval(50);
        QObject::connect(watch, &QTimer::timeout, host, [host, text, answered, watch] {
            for (auto* s : host->findChildren<ui::Sheet*>()) {
                if (!s->isVisible())
                    continue;
                for (auto* b : s->findChildren<QPushButton*>())
                    if (b->text() == text) {
                        watch->stop();
                        watch->deleteLater();
                        *answered = true;
                        b->click();
                        return;
                    }
            }
        });
        watch->start();
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_root.isValid());
        fwtest::isolateAppData(m_root);
        // Nothing here builds the Backport page, but no test of this app may
        // ever reach the real PS5 Backport files.
        qputenv("D97BETA_CONFIG_DIR", QDir::toNativeSeparators(m_root.filePath(QStringLiteral("bp-config"))).toUtf8());
        qputenv("D97BETA_DATA_DIR", QDir::toNativeSeparators(m_root.filePath(QStringLiteral("bp-data"))).toUtf8());
        theme::apply(qApp);
    }

    // entitlement_label and unknown keys have no column: they must stay with
    // their own entry when rows are removed, added or duplicated around it.
    void keysWithoutAColumnStayWithTheirEntry()
    {
        pages::DlcEmuEditor editor;
        editor.resize(1100, 700);
        editor.show();
        editor.setEntries(dlcemu::parse(iniWith({QStringLiteral("L1"), QStringLiteral("L2"), QStringLiteral("L3")})));
        QCOMPARE(editor.table()->rowCount(), 3);
        QCOMPARE(labelsOf(editor.entries()), (QStringList{QStringLiteral("L1"), QStringLiteral("L2"), QStringLiteral("L3")}));

        editor.table()->selectRow(0);
        QAbstractButton* remove = buttonByTip(&editor, QStringLiteral("Remove the selected entry"));
        QVERIFY(remove);
        remove->click();
        QCOMPARE(labelsOf(editor.entries()), (QStringList{QStringLiteral("L2"), QStringLiteral("L3")}));

        // A new entry starts empty: it used to pick up the keys of another row.
        QPushButton* add = buttonByText(&editor, QStringLiteral("Add entry"));
        QVERIFY(add);
        add->click();
        auto all = editor.entries();
        QCOMPARE(int(all.size()), 3);
        QCOMPARE(all[2].get(QStringLiteral("entitlement_label")), QString());
        QCOMPARE(all[2].get(QStringLiteral("extra_key")), QString());
        QCOMPARE(all[0].get(QStringLiteral("extra_key")), QStringLiteral("x2"));
        QCOMPARE(all[1].get(QStringLiteral("extra_key")), QStringLiteral("x3"));

        // A duplicate carries the keys of the row it copies.
        editor.table()->setCurrentCell(1, 1);
        QAbstractButton* dup = buttonByTip(&editor, QStringLiteral("Duplicate the selected entry"));
        QVERIFY(dup);
        dup->click();
        all = editor.entries();
        QCOMPARE(int(all.size()), 4);
        QCOMPARE(all[3].get(QStringLiteral("entitlement_label")), QStringLiteral("L3"));

        // Removing in the middle leaves the others as they are.
        editor.table()->selectRow(1);
        remove->click();
        QCOMPARE(labelsOf(editor.entries()), (QStringList{QStringLiteral("L2"), QString(), QStringLiteral("L3")}));
    }

    void unsavedEditsAreNotDroppedWithoutAQuestion()
    {
        const QString a = m_root.filePath(QStringLiteral("gameA"));
        const QString b = m_root.filePath(QStringLiteral("gameB"));
        QVERIFY(fwtest::writeTextFile(a + QStringLiteral("/dlc_emu.ini"), iniWith({QStringLiteral("A1"), QStringLiteral("A2")}).toUtf8()));
        QVERIFY(fwtest::writeTextFile(b + QStringLiteral("/dlc_emu.ini"), iniWith({QStringLiteral("B1")}).toUtf8()));
        pages::DlcEmuEditor editor;
        editor.resize(1100, 700);
        editor.show();
        editor.openFolder(a);
        QTRY_COMPARE_WITH_TIMEOUT(editor.table()->rowCount(), 2, 10000);
        QVERIFY(!editor.hasUnsavedChanges());

        editor.table()->item(0, 2)->setText(QStringLiteral("EDITED"));
        QVERIFY(editor.hasUnsavedChanges());

        // Another game is opened: asked first, and "Keep editing" keeps everything.
        bool answered = false;
        answerNextSheet(&editor, QStringLiteral("Keep editing"), &answered);
        editor.openFolder(b);
        QTRY_VERIFY_WITH_TIMEOUT(answered, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(QDir::cleanPath(editor.folder()), QDir::cleanPath(a), 10000);
        QTest::qWait(300);
        QCOMPARE(editor.table()->rowCount(), 2);
        QCOMPARE(editor.entries()[0].get(QStringLiteral("label")), QStringLiteral("EDITED"));
        QVERIFY(editor.hasUnsavedChanges());

        // Asked again; "Discard changes" loads the other game.
        answered = false;
        answerNextSheet(&editor, QStringLiteral("Discard changes"), &answered);
        editor.openFolder(b);
        QTRY_VERIFY_WITH_TIMEOUT(answered, 10000);
        QTRY_COMPARE_WITH_TIMEOUT(editor.table()->rowCount(), 1, 10000);
        QCOMPARE(labelsOf(editor.entries()), QStringList{QStringLiteral("B1")});
        QVERIFY(!editor.hasUnsavedChanges());

        // The same folder again (Return in the field): the table is not reloaded.
        editor.table()->item(0, 2)->setText(QStringLiteral("EDITED B"));
        editor.openFolder(b);
        QTest::qWait(400);
        QCOMPARE(editor.entries()[0].get(QStringLiteral("label")), QStringLiteral("EDITED B"));
    }

    void scanResultIsMergedIntoTheKnownList()
    {
        const QString lib = m_root.filePath(QStringLiteral("lib"));
        const QString gone = m_root.filePath(QStringLiteral("lib-not-connected"));
        QDir().mkpath(lib);
        const QDateTime old = QDateTime::currentDateTime().addDays(-30);
        const QDateTime now = QDateTime::currentDateTime();
        const std::vector<ui::LibraryGame> known = {
            game(lib + QStringLiteral("/GameA"), QStringLiteral("Backported"), old),
            game(lib + QStringLiteral("/Deleted"), {}, old),
            game(m_root.filePath(QStringLiteral("elsewhere/ByHand")), {}, old),      // added by hand, still there
            game(m_root.filePath(QStringLiteral("elsewhere/ByHandGone")), {}, old),  // added by hand, deleted since
            game(gone + QStringLiteral("/OnTheOtherDrive"), {}, old),               // its library folder cannot be reached
        };
        std::vector<ui::LibraryGame> scanned = {game(QDir::toNativeSeparators(lib + QStringLiteral("/GAMEA"))), game(lib + QStringLiteral("/NewGame"))};
        const auto merged = pages::mergeScanResult(known, scanned, {lib, gone}, now,
                                                   [](const QString& folder) { return !folder.endsWith(QLatin1String("Gone")); });
        QStringList expected{QDir::toNativeSeparators(lib + QStringLiteral("/GAMEA")), lib + QStringLiteral("/NewGame"),
                             m_root.filePath(QStringLiteral("elsewhere/ByHand")), gone + QStringLiteral("/OnTheOtherDrive")};
        expected.sort();
        QCOMPARE(foldersOf(merged), expected);
        for (const auto& g : merged) {
            if (g.info.folder.endsWith(QLatin1String("GAMEA"))) {
                QCOMPARE(g.lastSeen, old); // not "recently added" again
                QCOMPARE(g.status, QStringLiteral("Backported"));
            }
            if (g.info.folder.endsWith(QLatin1String("NewGame")))
                QCOMPARE(g.lastSeen, now);
        }
        // A scan that finds nothing empties the reachable library folders, and only those.
        const auto none = pages::mergeScanResult(known, {}, {lib, gone}, now, [](const QString&) { return true; });
        QStringList left{m_root.filePath(QStringLiteral("elsewhere/ByHand")), m_root.filePath(QStringLiteral("elsewhere/ByHandGone")),
                         gone + QStringLiteral("/OnTheOtherDrive")};
        left.sort();
        QCOMPARE(foldersOf(none), left);
    }
};

QTEST_MAIN(TstPagesbEditors)
#include "tst_pagesb_editors.moc"
