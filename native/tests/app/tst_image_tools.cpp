// Offscreen tests of the Image tools page (Extract, Verify / compare, Convert,
// Edit) against images built by the real engines: the old "extract" / "edit"
// navigation keys, the image strip's metadata read from inside each format,
// extract / verify / convert jobs submitted from the page, the hex preview's
// byte service (formatting, synchronous and worker-thread reads of .exfat and
// .ffpkg images), the Preview… sheet with "Load more", and comparing
// (validation, image.compare through the queue, the result sheet and its
// plain-text report).

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/CompareResultSheet.h"
#include "app/pages/HexPreviewSheet.h"
#include "app/pages/ImageToolsPage.h"
#include "app/pages/LibraryPage.h"
#include "app/services/HexPreview.h"
#include "app/services/ImageInfo.h"
#include "app/services/JobSubmit.h"
#include "app/ui/LibraryGrid.h"
#include "app/ui/Sheet.h"
#include "app/ui/Theme.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

using namespace fw;

namespace {

const QByteArray kParam = R"({"titleId":"PPSA00002","contentVersion":"01.020.000","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Image Test"}}})";

// Answers the confirmation sheet that a page action is about to open.
void answerSheetSoon(QObject* ctx, QWidget* window, int button)
{
    QTimer::singleShot(300, ctx, [window, button] {
        for (auto* s : window->findChildren<ui::Sheet*>())
            if (s->isOpen()) {
                s->close(button);
                return;
            }
    });
}

// Runs `fn` and returns the id of the one job it queued ("" when it queued none or several).
QString newJob(JobQueue& q, const std::function<void()>& fn)
{
    QSet<QString> before;
    for (const JobRecord& r : q.records())
        before.insert(r.id);
    fn();
    QStringList added;
    for (const JobRecord& r : q.records())
        if (!before.contains(r.id))
            added << r.id;
    return added.size() == 1 ? added.first() : QString();
}

bool waitTerminal(JobQueue& q, const QString& id, int ms = 120000)
{
    return QTest::qWaitFor([&] {
        auto r = q.record(id);
        return r && isTerminal(r->state);
    }, ms);
}

QByteArray fileBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

} // namespace

class TstImageTools : public QObject {
    Q_OBJECT
    QTemporaryDir m_root;
    std::unique_ptr<AppContext> m_ctx;
    std::unique_ptr<MainWindow> m_window;
    QString m_src, m_exfat, m_ffpkg, m_ffpfsc;

    QString build(const QString& format, const QString& source, const QString& name)
    {
        auto r = jobs::submitImageBuild(*m_ctx, format, source, m_root.path(), name, {});
        if (!r.ok())
            return {};
        m_ctx->queue().holdQueue(false);
        if (!waitTerminal(m_ctx->queue(), r.value()) || m_ctx->queue().record(r.value())->state != JobState::Succeeded)
            return {};
        return m_root.filePath(name + jobs::outputExtension(format));
    }

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
        for (const QString& kind : {QStringLiteral("image.exfat.build"), QStringLiteral("image.ffpkg.build"), QStringLiteral("image.compare")})
            if (!jobs::isAvailable(*m_ctx, kind))
                QSKIP("image engines not registered in this build");

        m_src = m_root.filePath(QStringLiteral("game"));
        QVERIFY(fwtest::writeTextFile(m_src + QStringLiteral("/sce_sys/param.json"), kParam));
        QVERIFY(fwtest::writeRandomFile(m_src + QStringLiteral("/eboot.bin"), 150000, 11));
        QVERIFY(fwtest::writeRandomFile(m_src + QStringLiteral("/data/big.bin"), 3 * 65536 + 100, 12));
        QVERIFY(fwtest::writeTextFile(m_src + QStringLiteral("/data/notes.txt"), "Hello, image!\n"));
        m_exfat = build(jobs::kFormatExfat, m_src, QStringLiteral("game"));
        QVERIFY(!m_exfat.isEmpty());
        m_ffpkg = build(jobs::kFormatFfpkg, m_src, QStringLiteral("game"));
        QVERIFY(!m_ffpkg.isEmpty());
        if (jobs::isAvailable(*m_ctx, QStringLiteral("image.pfs.build"))) {
            m_ffpfsc = build(jobs::kFormatFfpfsc, m_src, QStringLiteral("game"));
            QVERIFY(!m_ffpfsc.isEmpty());
        }
        m_ctx->queue().clearFinished();
    }

    // The v5.0 keys still work: they open Image tools with the action selected.
    void oldKeysOpenImageTools()
    {
        auto* page = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        QVERIFY(page);
        QVERIFY(!m_window->pageKeys().contains(QStringLiteral("extract")));
        QVERIFY(!m_window->pageKeys().contains(QStringLiteral("edit")));
        m_window->navigate(QStringLiteral("extract"), {{QStringLiteral("source"), m_exfat}, {QStringLiteral("mode"), QStringLiteral("verify")}});
        QCOMPARE(m_window->currentPage(), QStringLiteral("imagetools"));
        QCOMPARE(page->imagePath(), m_exfat);
        QCOMPARE(page->action(), pages::ImageToolsPage::Action::Verify);
        QVERIFY(page->verifyOnly());
        QCOMPARE(page->primaryButton()->text(), QStringLiteral("Verify"));
        m_window->navigate(QStringLiteral("edit"), {{QStringLiteral("image"), m_ffpkg}});
        QCOMPARE(m_window->currentPage(), QStringLiteral("imagetools"));
        QCOMPARE(page->action(), pages::ImageToolsPage::Action::Edit);
        QCOMPARE(page->imagePath(), m_ffpkg);
        // PFS images cannot be edited; the page falls back to Extract.
        if (!m_ffpfsc.isEmpty()) {
            m_window->navigate(QStringLiteral("edit"), {{QStringLiteral("image"), m_ffpfsc}});
            QVERIFY(!page->isActionEnabled(pages::ImageToolsPage::Action::Edit));
            QCOMPARE(page->action(), pages::ImageToolsPage::Action::Extract);
        }
        page->openImage({});
        QVERIFY(!page->isActionEnabled(pages::ImageToolsPage::Action::Extract));
    }

    // The strip (and the Library's Images view) read the title from inside every format.
    void imageSummariesComeFromInsideTheImages()
    {
        QStringList images{m_exfat, m_ffpkg};
        if (!m_ffpfsc.isEmpty())
            images << m_ffpfsc;
        for (const QString& img : images) {
            const ui::ImageSummary s = ui::readImageSummary(img);
            QVERIFY2(s.error.isEmpty(), qPrintable(img + u' ' + s.error));
            QVERIFY2(s.fromImage, qPrintable(img));
            QCOMPARE(s.titleId, QStringLiteral("PPSA00002"));
            QCOMPARE(s.title, QStringLiteral("Image Test"));
            QCOMPARE(s.version, QStringLiteral("01.020.000"));
            QCOMPARE(s.files, qint64(4));
        }
        // From the file name when the image holds no metadata.
        ui::ImageSummary named;
        named.path = QStringLiteral("D:/x/PPSA01325 Astro Bot (01.002).ffpfsc");
        ui::guessFromFileName(named);
        QCOMPARE(named.titleId, QStringLiteral("PPSA01325"));
        QCOMPARE(named.version, QStringLiteral("01.002"));
        QCOMPARE(named.title, QStringLiteral("Astro Bot"));
        QCOMPARE(ui::findImageFiles({m_root.path()}).size(), images.size());

        auto* page = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        m_window->navigate(QStringLiteral("imagetools"), {{QStringLiteral("image"), m_ffpkg}});
        QTRY_COMPARE_WITH_TIMEOUT(page->imageSummary().title, QStringLiteral("Image Test"), 10000);
        QVERIFY(page->imageSummary().metaLine().contains(QStringLiteral("PPSA00002")));
        page->openImage({});
    }

    // Extract, verify and convert are submitted from the page as queue jobs.
    void extractVerifyAndConvertFromThePage()
    {
        auto* page = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        auto& q = m_ctx->queue();
        m_window->navigate(QStringLiteral("imagetools"), {{QStringLiteral("image"), m_exfat}, {QStringLiteral("action"), QStringLiteral("extract")}});
        const QString dest = m_root.filePath(QStringLiteral("extracted"));
        page->setExtractDestination(dest);
        const auto runPrimary = [&]() -> QString {
            const QString id = newJob(q, [&] { page->primaryAction(); });
            return !id.isEmpty() && waitTerminal(q, id) ? id : QString();
        };
        QString id = runPrimary();
        QVERIFY(!id.isEmpty());
        QCOMPARE(q.record(id)->kind, QStringLiteral("image.exfat.extract"));
        QCOMPARE(q.record(id)->state, JobState::Succeeded);
        QCOMPARE(fileBytes(dest + QStringLiteral("/data/notes.txt")), QByteArray("Hello, image!\n"));

        page->setAction(pages::ImageToolsPage::Action::Verify);
        QVERIFY(page->verifyOnly());
        id = runPrimary();
        QVERIFY(!id.isEmpty());
        QCOMPARE(q.record(id)->kind, QStringLiteral("image.exfat.verify"));
        QCOMPARE(q.record(id)->state, JobState::Succeeded);

        if (!jobs::isAvailable(*m_ctx, QStringLiteral("image.convert")) || m_ffpfsc.isEmpty())
            return;
        // exFAT -> FFPFSC packs the image as it is by default.
        page->setAction(pages::ImageToolsPage::Action::Convert);
        page->setConvertTarget(jobs::kFormatFfpfsc);
        QCOMPARE(page->convertMethod(), QStringLiteral("direct"));
        QCOMPARE(page->convertOutput(), m_root.filePath(QStringLiteral("game.ffpfsc")));
        const QString packed = m_root.filePath(QStringLiteral("packed ü.ffpfsc"));
        page->setConvertOutput(packed);
        id = runPrimary();
        QVERIFY(!id.isEmpty());
        QCOMPARE(q.record(id)->kind, QStringLiteral("image.convert"));
        QCOMPARE(q.record(id)->parameters.value(QStringLiteral("method")).toString(), QStringLiteral("direct"));
        QVERIFY2(q.record(id)->state == JobState::Succeeded, qPrintable(q.record(id)->error ? q.record(id)->error->message : QString()));
        QVERIFY(QFileInfo::exists(packed));
        // FFPKG -> uncompressed FFPFS.
        m_window->navigate(QStringLiteral("imagetools"), {{QStringLiteral("image"), m_ffpkg}, {QStringLiteral("action"), QStringLiteral("convert")},
                                                          {QStringLiteral("format"), jobs::kFormatFfpfs}});
        QCOMPARE(page->convertTarget(), jobs::kFormatFfpfs);
        QVERIFY(page->convertMethod().isEmpty());
        id = runPrimary();
        QVERIFY(!id.isEmpty());
        QCOMPARE(q.record(id)->parameters.value(QStringLiteral("format")).toString(), QStringLiteral("ffpfs"));
        QVERIFY2(q.record(id)->state == JobState::Succeeded, qPrintable(q.record(id)->error ? q.record(id)->error->message : QString()));
        QVERIFY(QFileInfo::exists(m_root.filePath(QStringLiteral("game.ffpfs"))));
        // The same conversion again asks before replacing; "Replace" passes replaceExisting.
        answerSheetSoon(this, m_window.get(), 1);
        id = runPrimary();
        QVERIFY(!id.isEmpty());
        QVERIFY(q.record(id)->parameters.value(QStringLiteral("replaceExisting")).toBool());
        QCOMPARE(q.record(id)->state, JobState::Succeeded);
        page->openImage({});
        q.clearFinished();
    }

    // Edit: staged changes are applied in one job after the confirmation.
    void editAppliesStagedChanges()
    {
        if (!jobs::isAvailable(*m_ctx, QStringLiteral("image.exfat.edit")))
            QSKIP("image editing not registered in this build");
        auto* page = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        auto& q = m_ctx->queue();
        const QString copy = m_root.filePath(QStringLiteral("edit-me.exfat"));
        QVERIFY(QFile::copy(m_exfat, copy));
        const QString extra = m_root.filePath(QStringLiteral("extra.txt"));
        QVERIFY(fwtest::writeTextFile(extra, "added by the test"));
        m_window->navigate(QStringLiteral("edit"), {{QStringLiteral("image"), copy}});
        QTRY_VERIFY_WITH_TIMEOUT(page->tree()->topLevelItemCount() >= 2, 15000);
        page->stage({QStringLiteral("add"), QStringLiteral("data/extra.txt"), extra, {}});
        page->stage({QStringLiteral("delete"), QStringLiteral("data/notes.txt"), {}, {}});
        QCOMPARE(page->primaryButton()->text(), QStringLiteral("Apply 2 changes"));
        answerSheetSoon(this, m_window.get(), 1);
        const QString id = newJob(q, [&] { page->primaryAction(); });
        QVERIFY(!id.isEmpty());
        QVERIFY(waitTerminal(q, id));
        QVERIFY2(q.record(id)->state == JobState::Succeeded, qPrintable(q.record(id)->error ? q.record(id)->error->message : QString()));
        QVERIFY(page->staged().empty());
        auto slice = ui::readHexSlice(copy, QStringLiteral("data/extra.txt"), 0);
        QVERIFY(slice.ok());
        QCOMPARE(slice.data, QByteArray("added by the test"));
        QVERIFY(!ui::readHexSlice(copy, QStringLiteral("data/notes.txt"), 0).ok());
        page->openImage({});
        q.clearFinished();
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_ctx.reset();
    }

    void hexLinesAreOffsetHexAndAscii()
    {
        const QByteArray bytes = QByteArray("\x7f" "ELF\x02\x01\x01\x00", 8) + QByteArray("Hello, world!\n", 14);
        const QStringList lines = ui::hexDumpLines(bytes, 0x10).split(u'\n');
        QCOMPARE(lines.size(), 2);
        QCOMPARE(lines[0], QStringLiteral("00000010  7f 45 4c 46 02 01 01 00  48 65 6c 6c 6f 2c 20 77  |.ELF....Hello, w|"));
        // The short last line keeps the ASCII column aligned.
        QCOMPARE(lines[1], QStringLiteral("00000020  6f 72 6c 64 21 0a                                 |orld!.          |"));
        QCOMPARE(lines[0].size(), lines[1].size());
        QCOMPARE(ui::hexOffsetDigits(0), 8);
        QCOMPARE(ui::hexOffsetDigits(0x100000000LL), 8);
        QCOMPARE(ui::hexOffsetDigits(0x100000001LL), 9);
        QVERIFY(ui::hexDumpLines({}, 0).isEmpty());
        QVERIFY(ui::hexDumpLines(QByteArray(1, 'a'), 0x123456789LL, 9).startsWith(QStringLiteral("123456789  61 ")));
    }

    void hexSlicesComeFromTheImages()
    {
        const QByteArray big = fileBytes(m_src + QStringLiteral("/data/big.bin"));
        for (const QString& image : {m_exfat, m_ffpkg}) {
            const ui::HexSlice first = ui::readHexSlice(image, QStringLiteral("data/big.bin"), 0);
            QVERIFY2(first.ok(), qPrintable(image + u' ' + first.error));
            QCOMPARE(first.fileSize, qint64(big.size()));
            QCOMPARE(first.data.size(), qsizetype(ui::kHexPreviewChunk));
            QCOMPARE(first.data, big.left(qsizetype(ui::kHexPreviewChunk)));
            const ui::HexSlice last = ui::readHexSlice(image, QStringLiteral("/data/big.bin"), 3 * 65536);
            QVERIFY(last.ok());
            QCOMPARE(last.data, big.mid(3 * 65536));
            QCOMPARE(last.end(), qint64(big.size()));
            const ui::HexSlice text = ui::readHexSlice(image, QStringLiteral("data/notes.txt"), 0);
            QCOMPARE(text.data, QByteArray("Hello, image!\n"));
            const ui::HexSlice missing = ui::readHexSlice(image, QStringLiteral("data/none.bin"), 0);
            QVERIFY(!missing.ok());
            QCOMPARE(missing.fileSize, qint64(-1));
        }
        // On a worker thread.
        ui::HexSliceReader reader;
        QSignalSpy spy(&reader, &ui::HexSliceReader::ready);
        reader.read(m_ffpkg, QStringLiteral("eboot.bin"), 100, 32);
        QVERIFY(reader.isBusy());
        QVERIFY(spy.wait(20000));
        QVERIFY(!reader.isBusy());
        const auto s = spy.takeFirst().at(0).value<ui::HexSlice>();
        QVERIFY(s.ok());
        QCOMPARE(s.offset, qint64(100));
        QCOMPARE(s.data, fileBytes(m_src + QStringLiteral("/eboot.bin")).mid(100, 32));
    }

    void editorPreviewLoadsMore()
    {
        auto* page = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        QVERIFY(page);
        m_window->navigate(QStringLiteral("edit"), {{QStringLiteral("image"), m_ffpkg}});
        QTRY_VERIFY_WITH_TIMEOUT(page->tree()->topLevelItemCount() >= 2, 15000);
        QVERIFY(!page->preview(QStringLiteral("data")));          // a folder
        QVERIFY(!page->preview(QStringLiteral("data/none.bin"))); // not in the image
        auto* sheet = page->preview(QStringLiteral("data/big.bin"));
        QVERIFY(sheet);
        QVERIFY(sheet->isOpen());
        QTRY_COMPARE_WITH_TIMEOUT(sheet->loadedBytes(), ui::kHexPreviewChunk, 15000);
        const QByteArray big = fileBytes(m_src + QStringLiteral("/data/big.bin"));
        QCOMPARE(sheet->fileSize(), qint64(big.size()));
        QVERIFY(sheet->text().startsWith(ui::hexDumpLines(big.left(16), 0)));
        QCOMPARE(sheet->text().count(u'\n') + 1, int(ui::kHexPreviewChunk / ui::kHexBytesPerLine));
        for (int i = 0; i < 3; ++i) {
            QVERIFY(sheet->loadMoreButton()->isEnabled());
            QTest::mouseClick(sheet->loadMoreButton(), Qt::LeftButton);
            QTRY_VERIFY_WITH_TIMEOUT(!sheet->isLoading(), 15000);
        }
        QCOMPARE(sheet->loadedBytes(), qint64(big.size()));
        QVERIFY(!sheet->loadMoreButton()->isEnabled()); // end of file
        QVERIFY(sheet->text().endsWith(ui::hexDumpLines(big.mid(big.size() - 100 + 96), big.size() - 4)));
        QVERIFY(sheet->errorText().isEmpty());
        sheet->close(0);
    }

    void compareCardRunsTheJobAndShowsTheResult()
    {
        auto* page = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        QVERIFY(page);
        m_window->navigate(QStringLiteral("extract"));
        QCOMPARE(page->compareStatus(), QStringLiteral("Not compared"));
        QVERIFY(page->compareButton()->isEnabled());

        // Validation stays in the card; nothing is queued.
        const auto before = m_ctx->queue().records().size();
        page->setCompareInputs({}, {});
        page->startCompare();
        page->setCompareInputs(m_exfat, m_exfat);
        page->startCompare();
        page->setCompareInputs(m_exfat, m_root.filePath(QStringLiteral("nope.ffpkg")));
        page->startCompare();
        QCOMPARE(m_ctx->queue().records().size(), before);

        // Identical: exFAT vs FFPKG of one folder.
        page->setCompareInputs(m_exfat, m_ffpkg, QStringLiteral("full"));
        QTest::mouseClick(page->compareButton(), Qt::LeftButton);
        QVERIFY(!page->compareJobId().isEmpty());
        QVERIFY(waitTerminal(m_ctx->queue(), page->compareJobId()));
        QTRY_COMPARE_WITH_TIMEOUT(page->compareStatus(), QStringLiteral("Identical"), 5000);
        QVERIFY(page->compareResultButton()->isVisible());
        QCOMPARE(m_ctx->queue().record(page->compareJobId())->parameters.value(QStringLiteral("depth")).toString(), QStringLiteral("full"));
        auto* same = page->showCompareResult();
        QVERIFY(same);
        QVERIFY(same->groupTitles().isEmpty());
        QVERIFY(same->report().contains(QStringLiteral("No differences")));
        same->close(0);

        // Against an edited folder: one file gone, one added, one resized.
        const QString edited = m_root.filePath(QStringLiteral("game-edited"));
        QVERIFY(fwtest::writeTextFile(edited + QStringLiteral("/sce_sys/param.json"), kParam));
        QVERIFY(fwtest::writeTextFile(edited + QStringLiteral("/data/notes.txt"), "Hello, edited image!\n"));
        QVERIFY(QFile::copy(m_src + QStringLiteral("/data/big.bin"), edited + QStringLiteral("/data/big.bin")));
        QVERIFY(fwtest::writeTextFile(edited + QStringLiteral("/data/new.bin"), "new"));
        page->setCompareInputs(m_exfat, edited, QStringLiteral("structure"));
        page->startCompare();
        QVERIFY(waitTerminal(m_ctx->queue(), page->compareJobId()));
        QTRY_COMPARE_WITH_TIMEOUT(page->compareStatus(), QStringLiteral("3 differences"), 5000);
        const auto rec = m_ctx->queue().record(page->compareJobId());
        QCOMPARE(rec->state, JobState::Succeeded);
        QCOMPARE(rec->outcome->verification, Verification::NotRun);
        QCOMPARE(rec->parameters.value(QStringLiteral("b")).toString(), edited);
        auto* sheet = page->showCompareResult();
        QVERIFY(sheet);
        QCOMPARE(sheet->groupTitles(), (QStringList{QStringLiteral("Only in A (missing from B) · 1"), QStringLiteral("Only in B (extra) · 1"),
                                                    QStringLiteral("Different size · 1")}));
        QCOMPARE(sheet->list()->topLevelItem(0)->child(0)->text(0), QStringLiteral("eboot.bin"));
        QCOMPARE(sheet->list()->topLevelItem(1)->child(0)->text(0), QStringLiteral("data/new.bin"));
        QCOMPARE(sheet->list()->topLevelItem(2)->child(0)->text(0), QStringLiteral("data/notes.txt"));
        QCOMPARE(sheet->list()->topLevelItem(2)->child(0)->text(1), QStringLiteral("A 14 · B 21 bytes"));
        const QString report = sheet->report();
        QVERIFY(report.contains(QStringLiteral("ONLY IN A (MISSING FROM B) (1)\n  eboot.bin")));
        QVERIFY(report.contains(QStringLiteral("data/notes.txt   A 14 · B 21 bytes")));
        QVERIFY(report.contains(QStringLiteral("Compared names and sizes")));
        sheet->close(0);

        // The result survives clearing the queue.
        m_ctx->queue().clearFinished();
        QCOMPARE(page->compareStatus(), QStringLiteral("3 differences"));
        auto* again = page->showCompareResult();
        QVERIFY(again);
        again->close(0);
    }

    // Library: a folder of dumps and images; the game shows the formats it has,
    // the Images view reads each image's title, and images rename by title.
    void libraryListsGamesImagesAndRenames()
    {
        auto* lib = qobject_cast<pages::LibraryPage*>(m_window->page(QStringLiteral("library")));
        QVERIFY(lib);
        m_window->navigate(QStringLiteral("library"));
        const QString dir = m_root.filePath(QStringLiteral("lib"));
        QVERIFY(fwtest::writeTextFile(dir + QStringLiteral("/Image Test dump/sce_sys/param.json"), kParam));
        QVERIFY(fwtest::writeRandomFile(dir + QStringLiteral("/Image Test dump/eboot.bin"), 5000, 3));
        const QString image = dir + QStringLiteral("/my game.exfat");
        QVERIFY(QFile::copy(m_exfat, image));
        lib->dropUrls({QUrl::fromLocalFile(dir)});
        QTRY_VERIFY_WITH_TIMEOUT(!lib->isScanningImages() && lib->images().size() == 1, 20000);
        QCOMPARE(lib->images().first().titleId, QStringLiteral("PPSA00002"));
        QCOMPARE(lib->imagesView()->count(), 1);
        QCOMPARE(lib->imagesView()->items().front().title, QStringLiteral("Image Test"));
        if (jobs::isAvailable(*m_ctx, QStringLiteral("library.scan"))) {
            QTRY_COMPARE_WITH_TIMEOUT(lib->gamesView()->count(), 1, 20000);
            QTRY_COMPARE_WITH_TIMEOUT(lib->gamesView()->items().front().chips, (QStringList{QStringLiteral("Folder"), QStringLiteral("exFAT")}), 20000);
        }
        // Rename by title ID, name and version; never over an existing file.
        const QString name = pages::LibraryPage::imageFileName(lib->images().first(), QStringLiteral("id-title-version"));
        QCOMPARE(name, QStringLiteral("PPSA00002 Image Test (01.020.000).exfat"));
        QCOMPARE(pages::LibraryPage::imageFileName(lib->images().first(), QStringLiteral("id")), QStringLiteral("PPSA00002.exfat"));
        QStringList skipped;
        QCOMPARE(lib->renameImages({{image, name}}, &skipped), 1);
        QVERIFY(QFileInfo::exists(dir + u'/' + name));
        QVERIFY(QFile::copy(m_exfat, image));
        QCOMPARE(lib->renameImages({{image, name}}, &skipped), 0);
        QCOMPARE(skipped.size(), 1);
        QVERIFY(QFileInfo::exists(image));
        m_ctx->queue().clearFinished();
    }

    void reportListsTruncatedGroups()
    {
        QJsonArray many;
        for (int i = 0; i < 3; ++i)
            many.append(QStringLiteral("f%1.bin").arg(i));
        const QJsonObject d{
            {QStringLiteral("a"), QJsonObject{{QStringLiteral("path"), QStringLiteral("/x/a.exfat")}, {QStringLiteral("kind"), QStringLiteral("exFAT image")}}},
            {QStringLiteral("b"), QJsonObject{{QStringLiteral("path"), QStringLiteral("/x/b")}, {QStringLiteral("kind"), QStringLiteral("folder")}}},
            {QStringLiteral("depth"), QStringLiteral("full")},
            {QStringLiteral("identical"), false},
            {QStringLiteral("counts"), QJsonObject{{QStringLiteral("missing"), 900}, {QStringLiteral("contentDifferent"), 1}, {QStringLiteral("differences"), 901}}},
            {QStringLiteral("missing"), many},
            {QStringLiteral("contentDifferent"), QJsonArray{QJsonObject{{QStringLiteral("path"), QStringLiteral("eboot.bin")}, {QStringLiteral("offset"), 4096}}}},
            {QStringLiteral("listLimit"), 3},
            {QStringLiteral("truncated"), true},
        };
        const QString text = pages::CompareResultSheet::reportText(d, QStringLiteral("901 differences"), {QStringLiteral("note")});
        QVERIFY(text.contains(QStringLiteral("  … and 897 more (only the first 3 are listed)")));
        QVERIFY(text.contains(QStringLiteral("eboot.bin   differs at byte 4,096 (0x1000)")));
        QVERIFY(text.contains(QStringLiteral("NOTES\n  note")));
        auto* sheet = new pages::CompareResultSheet(m_window.get(), d, QStringLiteral("901 differences"));
        QCOMPARE(sheet->groupTitles(), (QStringList{QStringLiteral("Only in A (missing from B) · 900"), QStringLiteral("Different content · 1")}));
        QCOMPARE(sheet->list()->topLevelItem(0)->childCount(), 4); // 3 listed + "… and 897 more"
        sheet->close(0);
    }
};

QTEST_MAIN(TstImageTools)
#include "tst_image_tools.moc"
