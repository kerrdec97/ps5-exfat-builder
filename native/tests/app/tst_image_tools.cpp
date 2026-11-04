// Offscreen tests of the image tools on the Extract & verify and Edit image
// pages, against images built by the real engines: the hex preview's byte
// service (formatting, synchronous and worker-thread reads of .exfat and
// .ffpkg images), the Preview… sheet with "Load more", and the Compare card
// (validation, image.compare through the queue, the result sheet and its
// plain-text report).

#include "app/AppContext.h"
#include "app/MainWindow.h"
#include "app/pages/CompareResultSheet.h"
#include "app/pages/ExtractVerifyPage.h"
#include "app/pages/HexPreviewSheet.h"
#include "app/pages/ImageEditorPage.h"
#include "app/services/HexPreview.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Theme.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace fw;

namespace {

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
    QString m_src, m_exfat, m_ffpkg;

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
        QVERIFY(fwtest::writeTextFile(m_src + QStringLiteral("/sce_sys/param.json"), "{\"titleId\":\"PPSA00002\"}"));
        QVERIFY(fwtest::writeRandomFile(m_src + QStringLiteral("/eboot.bin"), 150000, 11));
        QVERIFY(fwtest::writeRandomFile(m_src + QStringLiteral("/data/big.bin"), 3 * 65536 + 100, 12));
        QVERIFY(fwtest::writeTextFile(m_src + QStringLiteral("/data/notes.txt"), "Hello, image!\n"));
        m_exfat = build(jobs::kFormatExfat, m_src, QStringLiteral("game"));
        QVERIFY(!m_exfat.isEmpty());
        m_ffpkg = build(jobs::kFormatFfpkg, m_src, QStringLiteral("game"));
        QVERIFY(!m_ffpkg.isEmpty());
        m_ctx->queue().clearFinished();
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
        auto* page = qobject_cast<pages::ImageEditorPage*>(m_window->page(QStringLiteral("edit")));
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
        auto* page = qobject_cast<pages::ExtractVerifyPage*>(m_window->page(QStringLiteral("extract")));
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
        QVERIFY(fwtest::writeTextFile(edited + QStringLiteral("/sce_sys/param.json"), "{\"titleId\":\"PPSA00002\"}"));
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
