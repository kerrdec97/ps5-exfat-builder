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
#include "app/pages/BuildImagePage.h"
#include "app/pages/CompareResultSheet.h"
#include "app/pages/HexPreviewSheet.h"
#include "app/pages/ImageToolsPage.h"
#include "app/pages/LibraryPage.h"
#include "app/services/HexPreview.h"
#include "app/services/ImageInfo.h"
#include "app/services/ImageTree.h"
#include "app/services/JobSubmit.h"
#include "app/ui/LibraryGrid.h"
#include "app/ui/SettingRow.h"
#include "app/ui/Sheet.h"
#include "app/ui/Theme.h"
#include "app/ui/Widgets.h"
#include "engines/ampr/Module.h"
#include "engines/gameinfo/Module.h"
#include "engines/general/Module.h"
#include "support/TestSupport.h"

#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QTreeWidgetItemIterator>
#include <QUrl>

#include <atomic>

using namespace fw;

namespace {

const QByteArray kParam = R"({"titleId":"PPSA00002","contentVersion":"01.020.000","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Image Test"}}})";

// Answers the confirmation sheet that a page action is about to open, whenever
// it opens. The returned flag says whether one was shown; the watch ends with
// the answer, or when the caller lets go of the flag.
[[nodiscard]] std::shared_ptr<bool> answerSheetSoon(QObject* ctx, QWidget* window, int button)
{
    auto answered = std::make_shared<bool>(false);
    auto* watch = new QTimer(ctx);
    watch->setInterval(30);
    QObject::connect(watch, &QTimer::timeout, ctx, [watch, window, button, answered] {
        if (answered.use_count() == 1) { // nobody waits for the answer any more
            watch->stop();
            watch->deleteLater();
            return;
        }
        for (auto* s : window->findChildren<ui::Sheet*>())
            if (s->isOpen()) {
                watch->stop();
                watch->deleteLater();
                *answered = true;
                s->close(button);
                return;
            }
    });
    watch->start();
    return answered;
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

// Whether the Edit tree lists `path` ('/'-separated, as the page stages it).
bool treeHas(QTreeWidget* tree, const QString& path)
{
    for (QTreeWidgetItemIterator it(tree); *it; ++it)
        if ((*it)->data(0, Qt::UserRole).toString() == path)
            return true;
    return false;
}

QByteArray fileBytes(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

std::atomic<bool> g_gateOpen{true};

// Uses the paths in its parameters as a job of that shape does (an "image" as
// a verify or an upload, a "source" folder it reads, an "output" folder it
// saves into; the default busyPaths() name them) and runs until the test
// opens the gate.
class ImageUsingJob : public Job {
public:
    explicit ImageUsingJob(QJsonObject params) : m_params(std::move(params)) {}
    QString kind() const override { return QStringLiteral("test.image-in-use"); }
    QString title() const override { return QStringLiteral("Uses an image"); }
    QJsonObject parameters() const override { return m_params; }
    Result<JobOutcome> run(JobContext& ctx) override
    {
        while (!g_gateOpen.load()) {
            if (!ctx.token.checkpoint())
                return Error::cancelled();
            QThread::msleep(5);
        }
        return JobOutcome{};
    }

private:
    QJsonObject m_params;
};

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
            QVERIFY(jobs::isAvailable(*m_ctx, kind)); // every engine is linked into the app tests

        m_src = m_root.filePath(QStringLiteral("game"));
        QVERIFY(fwtest::writeTextFile(m_src + QStringLiteral("/sce_sys/param.json"), kParam));
        QVERIFY(fwtest::writeRandomFile(m_src + QStringLiteral("/eboot.bin"), 150000, 11));
        QVERIFY(fwtest::writeRandomFile(m_src + QStringLiteral("/data/big.bin"), 3 * 65536 + 100, 12));
        QVERIFY(fwtest::writeTextFile(m_src + QStringLiteral("/data/notes.txt"), "Hello, image!\n"));
        m_exfat = build(jobs::kFormatExfat, m_src, QStringLiteral("game"));
        QVERIFY(!m_exfat.isEmpty());
        m_ffpkg = build(jobs::kFormatFfpkg, m_src, QStringLiteral("game"));
        QVERIFY(!m_ffpkg.isEmpty());
        m_ffpfsc = build(jobs::kFormatFfpfsc, m_src, QStringLiteral("game"));
        QVERIFY(!m_ffpfsc.isEmpty());
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
        const QStringList images{m_exfat, m_ffpkg, m_ffpfsc};
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
        // Found again by a search of the folder (other functions may have added images of their own).
        QStringList found;
        for (const QString& f : ui::findImageFiles({m_root.path()}))
            found << QDir::cleanPath(f).toLower();
        for (const QString& img : images)
            QVERIFY2(found.contains(QDir::cleanPath(img).toLower()), qPrintable(img));

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
        // While the image's summary is still being read, Extract would pick the wrong job: it waits.
        QVERIFY(page->isReading());
        QVERIFY(newJob(q, [&] { page->primaryAction(); }).isEmpty());
        const auto runPrimary = [&]() -> QString {
            page->showForm(); // "Back to the image" after the job before
            if (!QTest::qWaitFor([&] { return !page->isReading(); }, 15000))
                return {};
            const QString id = newJob(q, [&] { page->primaryAction(); });
            return !id.isEmpty() && waitTerminal(q, id) ? id : QString();
        };
        QString id = runPrimary();
        QVERIFY(!id.isEmpty());
        // The monitor took the form's place: Ctrl+B and drops do not reach the hidden form.
        QVERIFY(page->isMonitorShown());
        QVERIFY(newJob(q, [&] { page->primaryAction(); }).isEmpty());
        QVERIFY(!page->acceptsDrop({QUrl::fromLocalFile(m_ffpkg)}));
        QCOMPARE(q.record(id)->kind, QStringLiteral("image.exfat.extract"));
        QCOMPARE(q.record(id)->state, JobState::Succeeded);
        QCOMPARE(fileBytes(dest + QStringLiteral("/data/notes.txt")), QByteArray("Hello, image!\n"));

        page->setAction(pages::ImageToolsPage::Action::Verify);
        QVERIFY(page->verifyOnly());
        id = runPrimary();
        QVERIFY(!id.isEmpty());
        QCOMPARE(q.record(id)->kind, QStringLiteral("image.exfat.verify"));
        QCOMPARE(q.record(id)->state, JobState::Succeeded);

        QVERIFY(jobs::isAvailable(*m_ctx, QStringLiteral("image.convert")));
        // exFAT -> FFPFSC packs the image as it is by default.
        page->setAction(pages::ImageToolsPage::Action::Convert);
        page->setConvertTarget(jobs::kFormatFfpfsc);
        QCOMPARE(page->convertMethod(), QStringLiteral("direct"));
        QCOMPARE(page->convertOutput(), m_root.filePath(QStringLiteral("game.ffpfsc")));
        // A chosen name with another format's extension follows the target, in the field too.
        page->setConvertOutput(m_root.filePath(QStringLiteral("chosen.ffpkg")));
        page->setConvertTarget(jobs::kFormatFfpfs);
        QCOMPARE(page->convertOutput(), m_root.filePath(QStringLiteral("chosen.ffpfs")));
        bool shown = false;
        for (auto* f : page->findChildren<ui::PathField*>())
            shown = shown || QDir::fromNativeSeparators(f->path()) == m_root.filePath(QStringLiteral("chosen.ffpfs"));
        QVERIFY(shown);
        page->setConvertTarget(jobs::kFormatFfpfsc);
        QCOMPARE(page->convertOutput(), m_root.filePath(QStringLiteral("chosen.ffpfsc")));
        page->setConvertOutput({});
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
        const auto askedToReplace = answerSheetSoon(this, m_window.get(), 1);
        id = runPrimary();
        QVERIFY2(*askedToReplace, "the existing image was replaced without a question");
        QVERIFY(!id.isEmpty());
        QVERIFY(q.record(id)->parameters.value(QStringLiteral("replaceExisting")).toBool());
        QCOMPARE(q.record(id)->state, JobState::Succeeded);
        page->openImage({});
        q.clearFinished();
    }

    // Edit: staged changes are applied in one job after the confirmation.
    void editAppliesStagedChanges()
    {
        QVERIFY(jobs::isAvailable(*m_ctx, QStringLiteral("image.exfat.edit"))); // every engine is linked into the app tests
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
        QVERIFY(treeHas(page->tree(), QStringLiteral("data/notes.txt")));
        QVERIFY(!treeHas(page->tree(), QStringLiteral("data/extra.txt")));
        page->showForm();
        const auto askedToApply = answerSheetSoon(this, m_window.get(), 1);
        const QString id = newJob(q, [&] { page->primaryAction(); });
        QVERIFY2(*askedToApply, "the changes were applied without a question");
        QVERIFY(!id.isEmpty());
        QVERIFY(waitTerminal(q, id));
        QVERIFY2(q.record(id)->state == JobState::Succeeded, qPrintable(q.record(id)->error ? q.record(id)->error->message : QString()));
        QVERIFY(page->staged().empty());
        // The tree is read again when the job ends: it shows the edited image.
        QTRY_VERIFY_WITH_TIMEOUT(treeHas(page->tree(), QStringLiteral("data/extra.txt")), 15000);
        QVERIFY(!treeHas(page->tree(), QStringLiteral("data/notes.txt")));
        auto slice = ui::readHexSlice(copy, QStringLiteral("data/extra.txt"), 0);
        QVERIFY(slice.ok());
        QCOMPARE(slice.data, QByteArray("added by the test"));
        QVERIFY(!ui::readHexSlice(copy, QStringLiteral("data/notes.txt"), 0).ok());
        page->openImage({});
        q.clearFinished();
    }

    // The confirmation lists the first changes and counts the rest (a sheet
    // does not scroll), with names shown literally; "Keep editing" keeps the
    // staged list, and so does reading the same image again.
    void editConfirmationListsTheFirstChanges()
    {
        QVERIFY(jobs::isAvailable(*m_ctx, QStringLiteral("image.exfat.edit"))); // every engine is linked into the app tests
        auto* page = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        auto& q = m_ctx->queue();
        m_window->navigate(QStringLiteral("edit"), {{QStringLiteral("image"), m_exfat}});
        QTRY_VERIFY_WITH_TIMEOUT(page->tree()->topLevelItemCount() >= 2, 15000);
        page->showForm();
        for (int i = 0; i < 30; ++i)
            page->stage({QStringLiteral("mkdir"), QStringLiteral("new <b>%1</b> & co").arg(i), {}, {}});
        QCOMPARE(page->staged().size(), size_t(30));
        page->refresh(); // F5 on the open image
        QCOMPARE(page->staged().size(), size_t(30));
        QTRY_VERIFY_WITH_TIMEOUT(page->tree()->topLevelItemCount() >= 2, 15000);

        QStringList texts;
        QTimer::singleShot(300, this, [&] {
            for (auto* s : m_window->findChildren<ui::Sheet*>())
                if (s->isOpen()) {
                    for (auto* l : s->findChildren<QLabel*>())
                        texts << l->text();
                    s->close(0); // Keep editing
                    return;
                }
        });
        QVERIFY(newJob(q, [&] { page->primaryAction(); }).isEmpty());
        const QString text = texts.join(u'\n');
        QVERIFY2(text.contains(QStringLiteral("Apply 30 changes to game.exfat?")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("… and 18 more")), qPrintable(text));
        QCOMPARE(text.count(QStringLiteral("New folder /new")), 12);
        QVERIFY(text.contains(QStringLiteral("&lt;b&gt;0&lt;/b&gt; &amp; co"))); // escaped exactly once
        QVERIFY(!text.contains(QStringLiteral("&lt;br")));
        QCOMPARE(page->staged().size(), size_t(30));
        // "Keep editing" is the user's own answer, not an error to report.
        for (auto* bar : page->findChildren<ui::InfoBar*>())
            QVERIFY(!bar->isVisible());

        // Another image is asked for: the staged changes are not dropped without a question.
        const auto askedToKeep = answerSheetSoon(this, m_window.get(), 0); // Keep editing
        m_window->navigate(QStringLiteral("imagetools"), {{QStringLiteral("image"), m_ffpkg}, {QStringLiteral("action"), QStringLiteral("verify")}});
        QVERIFY2(*askedToKeep, "the staged changes were not asked about");
        QCOMPARE(page->imagePath(), m_exfat);
        QCOMPARE(page->staged().size(), size_t(30));
        QCOMPARE(page->action(), pages::ImageToolsPage::Action::Edit);
        const auto askedToDiscard = answerSheetSoon(this, m_window.get(), 1); // Discard
        m_window->navigate(QStringLiteral("imagetools"), {{QStringLiteral("image"), m_ffpkg}, {QStringLiteral("action"), QStringLiteral("verify")}});
        QVERIFY2(*askedToDiscard, "the staged changes were not asked about");
        QCOMPARE(page->imagePath(), m_ffpkg);
        QVERIFY(page->staged().empty());
        page->openImage({});
    }

    // New folder: the name field has the focus, a name the image cannot hold
    // keeps the sheet open with the reason, a good one is staged.
    void newFolderSheetChecksTheName()
    {
        QVERIFY(jobs::isAvailable(*m_ctx, QStringLiteral("image.exfat.edit"))); // every engine is linked into the app tests
        auto* page = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        m_window->navigate(QStringLiteral("edit"), {{QStringLiteral("image"), m_exfat}});
        QTRY_VERIFY_WITH_TIMEOUT(page->tree()->topLevelItemCount() >= 2, 15000);
        QAbstractButton* mkdir = nullptr;
        for (auto* b : page->findChildren<ui::IconButton*>())
            if (b->accessibleName() == QLatin1String("New folder"))
                mkdir = b;
        QVERIFY(mkdir && mkdir->isEnabled());
        bool sheetSeen = false, focusOk = false, keptOpen = false, reasonShown = false;
        QTimer::singleShot(300, this, [&] {
            for (auto* s : m_window->findChildren<ui::Sheet*>()) {
                if (!s->isOpen())
                    continue;
                sheetSeen = true;
                auto* name = s->findChild<QLineEdit*>();
                QPushButton* stage = nullptr;
                for (auto* b : s->findChildren<QPushButton*>())
                    if (b->text() == QLatin1String("Stage change"))
                        stage = b;
                if (!name || !stage) {
                    s->close(0);
                    return;
                }
                focusOk = !m_window->isActiveWindow() || name->hasFocus();
                name->setText(QStringLiteral("a/b"));
                stage->click();
                keptOpen = s->isOpen();
                for (auto* row : s->findChildren<ui::MessageRow*>())
                    reasonShown = reasonShown || !row->text().isEmpty();
                name->setText(QStringLiteral("con:fig"));
                stage->click();
                keptOpen = keptOpen && s->isOpen();
                name->setText(QStringLiteral("  New Stuff "));
                stage->click();
                return;
            }
        });
        mkdir->click();
        QVERIFY(sheetSeen);
        QVERIFY(focusOk);
        QVERIFY(keptOpen);
        QVERIFY(reasonShown);
        QCOMPARE(page->staged().size(), size_t(1));
        QCOMPARE(page->staged().front().op, QStringLiteral("mkdir"));
        QCOMPARE(page->staged().front().imagePath, QStringLiteral("New Stuff"));
        page->clearStaged();
        page->openImage({});
    }

    // Job settings of rows that the current action hides are neither counted nor sent.
    void hiddenJobSettingsAreNotSent()
    {
        auto* page = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        auto& q = m_ctx->queue();
        m_window->navigate(QStringLiteral("imagetools"), {{QStringLiteral("image"), m_exfat}, {QStringLiteral("action"), QStringLiteral("convert")}});
        QTRY_VERIFY_WITH_TIMEOUT(!page->isReading(), 15000);
        auto* panel = page->findChild<ui::JobOptionsPanel*>();
        QVERIFY(panel);
        // A switch of the Build section: shown for Convert, hidden for Extract.
        QString key;
        for (const QString& k : panel->keys()) {
            const SettingDef* d = m_ctx->schema().find(k);
            if (d && d->section == QLatin1String("Build") && m_ctx->settings().value(k).typeId() == QMetaType::Bool
                && panel->row(k) && !panel->row(k)->isHidden() && d->enabledWhenKey.isEmpty()) {
                key = k;
                break;
            }
        }
        if (key.isEmpty())
            QSKIP("no per-job switch in the Build section");
        panel->setOverride(key, !m_ctx->settings().value(key).toBool());
        QVERIFY(panel->overrides().contains(key));
        page->setAction(pages::ImageToolsPage::Action::Extract);
        QVERIFY(panel->row(key)->isHidden());
        page->setExtractDestination(m_root.filePath(QStringLiteral("extract-hidden-rows")));
        q.holdQueue(true);
        const QString id = newJob(q, [&] { page->queueButton()->click(); });
        QVERIFY(!id.isEmpty());
        QVERIFY(!q.record(id)->overrides.contains(key));
        QVERIFY(q.remove(id));
        q.holdQueue(false);
        panel->clearOverrides();
        // An image in a drive root gets its defaults without a doubled separator.
        page->openImage(QStringLiteral("Q:/game.exfat"));
        QVERIFY2(page->convertOutput().startsWith(QStringLiteral("Q:/game")), qPrintable(page->convertOutput()));
        page->openImage({});
    }

    // GitHub #97: Extract and Convert show "Verify after …" up front. It
    // starts at Settings' "Verify every output", is that row of the job
    // options (hidden while the switch shows), is sent with the job, and an
    // extraction queued with it off is not verified. Where the action's job
    // has no verification step (a PFS image's extraction) there is no switch.
    void verifyAfterSwitchIsThisJobsVerification()
    {
        auto* page = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        auto& q = m_ctx->queue();
        const QString key = general::keys::kVerifyAfterBuild;
        auto* panel = page->findChild<ui::JobOptionsPanel*>();
        QVERIFY(panel && panel->row(key));
        m_window->navigate(QStringLiteral("imagetools"), {{QStringLiteral("image"), m_exfat}, {QStringLiteral("action"), QStringLiteral("extract")}});
        QTRY_VERIFY_WITH_TIMEOUT(!page->isReading(), 15000);
        ui::Switch* sw = page->verifySwitch();
        QVERIFY(sw);
        QVERIFY(sw->isVisibleTo(page));
        QCOMPARE(sw->accessibleName(), QStringLiteral("Verify after extracting"));
        QVERIFY(m_ctx->settings().value(key).toBool());
        QVERIFY(sw->isChecked());
        QVERIFY(panel->row(key)->isHidden());
        // The setting moves a switch nobody changed for this job.
        QVERIFY(m_ctx->settings().setValue(key, false).ok());
        QVERIFY(!sw->isChecked());
        m_ctx->settings().reset(key);
        QVERIFY(sw->isChecked());

        // Off for this job only: the override goes with the job, the setting stays.
        sw->click();
        QVERIFY(!sw->isChecked());
        QCOMPARE(panel->overrides().value(key), QVariant(false));
        QVERIFY(m_ctx->settings().value(key).toBool());
        const QString dest = m_root.filePath(QStringLiteral("extract-unverified"));
        page->setExtractDestination(dest);
        q.holdQueue(true);
        const QString id = newJob(q, [&] { page->queueButton()->click(); });
        QVERIFY(!id.isEmpty());
        QCOMPARE(q.record(id)->overrides.value(key), QVariant(false));
        q.holdQueue(false);
        QVERIFY(waitTerminal(q, id));
        QVERIFY2(q.record(id)->state == JobState::Succeeded, qPrintable(q.record(id)->error ? q.record(id)->error->message : QString()));
        QCOMPARE(q.record(id)->outcome->verification, Verification::Skipped);
        QCOMPARE(fileBytes(dest + QStringLiteral("/data/notes.txt")), QByteArray("Hello, image!\n"));
        page->showForm();

        // Convert has its own, holding the same value.
        page->setAction(pages::ImageToolsPage::Action::Convert);
        ui::Switch* convert = page->verifySwitch();
        QVERIFY(convert && convert != sw && convert->isVisibleTo(page));
        QCOMPARE(convert->accessibleName(), QStringLiteral("Verify after converting"));
        QVERIFY(!convert->isChecked());
        convert->click();
        QVERIFY(convert->isChecked() && sw->isChecked());
        QVERIFY(!panel->overrides().contains(key)); // back at the setting
        // The Verify action has no such switch; the options row shows again.
        page->setAction(pages::ImageToolsPage::Action::Verify);
        QVERIFY(!page->verifySwitch());
        QVERIFY(!panel->row(key)->isHidden());

        // A PFS image's extraction has no verification step.
        m_window->navigate(QStringLiteral("imagetools"), {{QStringLiteral("image"), m_ffpfsc}, {QStringLiteral("action"), QStringLiteral("extract")}});
        QTRY_VERIFY_WITH_TIMEOUT(!page->isReading(), 15000);
        QCOMPARE(page->action(), pages::ImageToolsPage::Action::Extract);
        QVERIFY(!page->verifySwitch());
        QVERIFY(!sw->isVisibleTo(page));
        page->openImage({});
        panel->clearOverrides();
        q.clearFinished();
    }

    // An invalid value in a row that the chosen action hides cannot be
    // corrected there, is not sent, and so does not block the job.
    void errorInAHiddenJobSettingDoesNotBlock()
    {
        auto* page = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        auto& q = m_ctx->queue();
        m_window->navigate(QStringLiteral("imagetools"), {{QStringLiteral("image"), m_exfat}, {QStringLiteral("action"), QStringLiteral("convert")}});
        QTRY_VERIFY_WITH_TIMEOUT(!page->isReading(), 15000);
        auto* panel = page->findChild<ui::JobOptionsPanel*>();
        QVERIFY(panel);
        // A typed value of the Build section: shown for Convert, hidden for Extract.
        ui::SettingRow* row = nullptr;
        for (const QString& k : panel->keys()) {
            const SettingDef* d = m_ctx->schema().find(k);
            ui::SettingRow* r = panel->row(k);
            if (!d || !r || r->isHidden() || d->section != QLatin1String("Build") || !d->enabledWhenKey.isEmpty()
                || d->type != SettingType::Int)
                continue;
            if (auto* edit = r->findChild<QLineEdit*>()) {
                edit->setText(QStringLiteral("abc"));
                emit edit->editingFinished();
                if (panel->errorKeys().contains(k)) {
                    row = r;
                    break;
                }
                emit r->useDefaultRequested();
            }
        }
        if (!row)
            QSKIP("no typed per-job number in the Build section");
        QVERIFY(panel->hasErrors());
        QVERIFY(!panel->errorMessage(panel->errorKeys().first()).isEmpty());
        page->setAction(pages::ImageToolsPage::Action::Extract);
        QVERIFY(row->isHidden());
        page->setExtractDestination(m_root.filePath(QStringLiteral("extract-hidden-error")));
        q.holdQueue(true);
        const QString id = newJob(q, [&] { page->queueButton()->click(); });
        QVERIFY2(!id.isEmpty(), "the hidden row's error blocked the job");
        QVERIFY(q.remove(id));
        // Shown again (Convert), the same error does block.
        page->setAction(pages::ImageToolsPage::Action::Convert);
        QVERIFY(!row->isHidden());
        QVERIFY(newJob(q, [&] { page->queueButton()->click(); }).isEmpty());
        q.holdQueue(false);
        emit row->useDefaultRequested();
        panel->clearOverrides();
        QVERIFY(!panel->hasErrors());
        page->openImage({});
    }

    // The file list of an image is read on a worker: a listing nobody waits
    // for any more is abandoned and never reported.
    void treeListingCanBeAbandoned()
    {
        CancellationSource stop;
        stop.cancel();
        QVERIFY(!ui::readImageTree(m_exfat, stop.token()).error.isEmpty());
        QVERIFY(ui::readImageTree(m_exfat).error.isEmpty());

        ui::ImageTreeReader reader;
        QSignalSpy ready(&reader, &ui::ImageTreeReader::ready);
        reader.read(m_exfat);
        QVERIFY(reader.isBusy());
        reader.cancel();
        QVERIFY(!reader.isBusy());
        QTest::qWait(400);
        QCOMPARE(ready.count(), 0);
        // A second read takes the place of the first: one answer, for the newest.
        reader.read(m_ffpkg);
        reader.read(m_exfat);
        QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 15000);
        const auto result = ready.first().first().value<ui::ImageTreeResult>();
        QCOMPARE(result.image, m_exfat);
        QVERIFY2(result.error.isEmpty(), qPrintable(result.error));
        QVERIFY(!reader.isBusy());
    }

    // Small texts of the two sheets: singular forms, and no "bytes 0–0" before anything was read.
    void sheetTextsCountCorrectly()
    {
        const auto side = [](const QString& path) {
            return QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("kind"), QStringLiteral("folder")},
                               {QStringLiteral("files"), 1}, {QStringLiteral("dirs"), 1}, {QStringLiteral("bytes"), 10}};
        };
        const QJsonObject d{{QStringLiteral("a"), side(QStringLiteral("D:/a"))}, {QStringLiteral("b"), side(QStringLiteral("D:/b"))},
                            {QStringLiteral("depth"), QStringLiteral("full")}, {QStringLiteral("identical"), true},
                            {QStringLiteral("counts"), QJsonObject{{QStringLiteral("differences"), 0}}}};
        const QStringList notes{QStringLiteral("n1"), QStringLiteral("n2"), QStringLiteral("n3"), QStringLiteral("<b>n4</b>")};
        auto* compare = new pages::CompareResultSheet(m_window.get(), d, QStringLiteral("Identical."), notes);
        QStringList texts;
        for (auto* l : compare->findChildren<QLabel*>())
            texts << l->text();
        QString all = texts.join(u'\n');
        QVERIFY2(all.contains(QStringLiteral("1 file · 1 folder ·")), qPrintable(all));
        QVERIFY(all.contains(QStringLiteral("1 more note in Copy report.")));
        QVERIFY(!all.contains(QStringLiteral("1 files")));
        delete compare;

        auto* hex = new pages::HexPreviewSheet(m_window.get(), m_exfat, QStringLiteral("data/notes.txt"), 14);
        texts.clear();
        for (auto* l : hex->findChildren<QLabel*>())
            texts << l->text();
        all = texts.join(u'\n');
        QVERIFY2(!all.contains(QStringLiteral("0–0")), qPrintable(all));
        QVERIFY(all.contains(QStringLiteral("14 bytes")));
        delete hex;
        // A file name that reads as markup stays out of the sheet's title.
        hex = new pages::HexPreviewSheet(m_window.get(), m_exfat, QStringLiteral("data/<b>x</b>.bin"), 14);
        for (auto* l : hex->findChildren<QLabel*>())
            QVERIFY2(!l->text().startsWith(QStringLiteral("Preview · <b>")), qPrintable(l->text()));
        delete hex;
    }

    // The result and preview sheets are information sheets: Close is the
    // default button (focused, Esc) in the secondary look, and the button that
    // acts on the content ("Copy report", "Load more") sits at the start of
    // the same row and leaves the sheet open.
    void resultSheetsCloseQuietly()
    {
        const auto side = [](const QString& path) {
            return QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("kind"), QStringLiteral("folder")},
                               {QStringLiteral("files"), 2}, {QStringLiteral("dirs"), 1}, {QStringLiteral("bytes"), 10}};
        };
        const QJsonObject d{{QStringLiteral("a"), side(QStringLiteral("D:/a"))}, {QStringLiteral("b"), side(QStringLiteral("D:/b"))},
                            {QStringLiteral("depth"), QStringLiteral("full")}, {QStringLiteral("identical"), false},
                            {QStringLiteral("counts"), QJsonObject{{QStringLiteral("missing"), 1}, {QStringLiteral("differences"), 1}}},
                            {QStringLiteral("missing"), QJsonArray{QStringLiteral("eboot.bin")}}};
        const auto buttonOf = [](QWidget* sheet, const QString& text) -> QPushButton* {
            for (QPushButton* b : sheet->findChildren<QPushButton*>())
                if (b->text() == text)
                    return b;
            return nullptr;
        };
        // `afterAside` waits for what the aside button did (nothing for Copy report).
        const auto check = [](ui::Sheet* sheet, QPushButton* aside, QPushButton* close, const auto& afterAside) {
            QVERIFY(aside && close);
            QSignalSpy finished(sheet, &ui::Sheet::finished);
            sheet->open();
            QTRY_VERIFY(close->hasFocus());
            QVERIFY(close->property("variant").toString() != QLatin1String("primary"));
            QCOMPARE(aside->parentWidget(), close->parentWidget()); // one button row
            QCOMPARE(aside->y(), close->y());
            QVERIFY(aside->mapTo(sheet, QPoint(0, 0)).x() < close->mapTo(sheet, QPoint(0, 0)).x());
            QTRY_VERIFY_WITH_TIMEOUT(aside->isEnabled(), 15000);
            aside->click();
            QVERIFY(sheet->isOpen());
            afterAside();
            if (QTest::currentTestFailed())
                return;
            QVERIFY(sheet->isOpen());
            QCOMPARE(finished.count(), 0);
            QTest::keyClick(close, Qt::Key_Escape);
            QTRY_COMPARE(finished.count(), 1);
            QCOMPARE(finished.first().first().toInt(), 1); // Close, the second button
        };
        auto* compare = new pages::CompareResultSheet(m_window.get(), d, QStringLiteral("1 difference"));
        check(compare, buttonOf(compare, QStringLiteral("Copy report")), buttonOf(compare, QStringLiteral("Close")), [] {});
        if (QTest::currentTestFailed())
            return;
        // A file longer than one slice, so "Load more" has something to read.
        const qint64 bigSize = QFileInfo(m_src + QStringLiteral("/data/big.bin")).size();
        QVERIFY(bigSize > 2 * ui::kHexPreviewChunk);
        auto* hex = new pages::HexPreviewSheet(m_window.get(), m_exfat, QStringLiteral("data/big.bin"), bigSize);
        hex->start();
        check(hex, hex->loadMoreButton(), buttonOf(hex, QStringLiteral("Close")), [hex] {
            QTRY_VERIFY_WITH_TIMEOUT(!hex->isLoading(), 15000);
            QCOMPARE(hex->loadedBytes(), 2 * ui::kHexPreviewChunk);
            QVERIFY(hex->errorText().isEmpty());
        });
    }

    // The comparison row follows comparisons of the open image only.
    void compareRowIgnoresOtherImages()
    {
        QVERIFY(jobs::isAvailable(*m_ctx, QStringLiteral("image.compare"))); // every engine is linked into the app tests
        auto* page = qobject_cast<pages::ImageToolsPage*>(m_window->page(QStringLiteral("imagetools")));
        auto& q = m_ctx->queue();
        page->showForm();
        page->openImage(m_exfat);
        QVERIFY(page->compareJobId().isEmpty());
        auto other = jobs::submitCompare(*m_ctx, m_ffpkg, m_src, QStringLiteral("structure"));
        QVERIFY(other.ok());
        QVERIFY(waitTerminal(q, other.value()));
        QTest::qWait(100);
        QVERIFY(page->compareJobId().isEmpty());
        QCOMPARE(page->compareStatus(), QStringLiteral("Not compared"));
        auto own = jobs::submitCompare(*m_ctx, m_exfat, m_src, QStringLiteral("structure"));
        QVERIFY(own.ok());
        QVERIFY(waitTerminal(q, own.value()));
        QTRY_COMPARE(page->compareJobId(), own.value());
        page->openImage({});
        q.clearFinished();
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
        QVERIFY(jobs::isAvailable(*m_ctx, QStringLiteral("library.scan")));
        QTRY_COMPARE_WITH_TIMEOUT(lib->gamesView()->count(), 1, 20000);
        QTRY_COMPARE_WITH_TIMEOUT(lib->gamesView()->items().front().chips, (QStringList{QStringLiteral("Folder"), QStringLiteral("exFAT")}), 20000);

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

        // A change of case only renames the file itself.
        const QString lower = name.toLower();
        skipped.clear();
        QCOMPARE(lib->renameImages({{dir + u'/' + name, lower}}, &skipped), 1);
        QVERIFY2(QDir(dir).entryList(QDir::Files).contains(lower), qPrintable(QDir(dir).entryList(QDir::Files).join(u'|')));

        // An image a running job uses is not renamed under it.
        auto& q = m_ctx->queue();
        g_gateOpen = false;
        auto openGate = qScopeGuard([] { g_gateOpen = true; });
        const auto startGated = [&](const QJsonObject& params) {
            g_gateOpen = false;
            const auto id = q.enqueue(std::make_unique<ImageUsingJob>(params));
            if (!id.ok())
                return QString();
            q.startNow(id.value());
            const bool running = QTest::qWaitFor([&] {
                auto r = q.record(id.value());
                return r && r->state == JobState::Running;
            }, 10000);
            return running ? id.value() : QString();
        };
        const QString busy = startGated({{QStringLiteral("image"), image}});
        QVERIFY(!busy.isEmpty());
        QVERIFY(q.isPathInUse(image));
        QVERIFY(!q.isPathInUse(dir + u'/' + lower));
        skipped.clear();
        QCOMPARE(lib->renameImages({{image, QStringLiteral("renamed while busy.exfat")}}, &skipped), 0);
        QCOMPARE(skipped, QStringList{QStringLiteral("my game.exfat: in use or read-only")});
        QVERIFY(QFileInfo::exists(image));
        QVERIFY(!QFileInfo::exists(dir + QStringLiteral("/renamed while busy.exfat")));
        g_gateOpen = true;
        QVERIFY(waitTerminal(q, busy));
        QVERIFY(!q.isPathInUse(image));

        // A job that only saves into the images' folder (an FPKG or DLC
        // "Save to" folder) does not hold the images already in it …
        const QString saving = startGated({{QStringLiteral("output"), dir}});
        QVERIFY(!saving.isEmpty());
        QVERIFY(q.isPathInUse(dir)); // the folder itself is the job's
        QVERIFY(!q.isPathInUse(image));
        skipped.clear();
        QCOMPARE(lib->renameImages({{image, QStringLiteral("renamed beside a build.exfat")}}, &skipped), 1);
        QVERIFY2(skipped.isEmpty(), qPrintable(skipped.join(u'|')));
        QCOMPARE(lib->renameImages({{dir + QStringLiteral("/renamed beside a build.exfat"), QStringLiteral("my game.exfat")}}, &skipped), 1);
        QVERIFY(QFileInfo::exists(image));
        g_gateOpen = true;
        QVERIFY(waitTerminal(q, saving));
        // … but one that reads the folder (an upload of it) does.
        const QString reading = startGated({{QStringLiteral("source"), dir}});
        QVERIFY(!reading.isEmpty());
        QVERIFY(q.isPathInUse(image));
        skipped.clear();
        QCOMPARE(lib->renameImages({{image, QStringLiteral("renamed while read.exfat")}}, &skipped), 0);
        QCOMPARE(skipped, QStringList{QStringLiteral("my game.exfat: in use or read-only")});
        g_gateOpen = true;
        QVERIFY(waitTerminal(q, reading));
        QVERIFY(QFileInfo::exists(image));
#ifdef Q_OS_WIN
        // Held open by another program (Qt opens files without delete sharing,
        // so the rename is refused): never copied to the new name instead,
        // which is minutes on the UI thread for an image. A copy would have
        // been made and removed again in the folder. Each refusal is at once,
        // not after a second of retries on the UI thread.
        {
            QFile holder(image), second(dir + u'/' + lower);
            QVERIFY(holder.open(QIODevice::ReadOnly));
            QVERIFY(second.open(QIODevice::ReadOnly));
            QTest::qWait(30);
            const QDateTime folderChanged = QFileInfo(dir).lastModified();
            skipped.clear();
            QElapsedTimer t;
            t.start();
            QCOMPARE(lib->renameImages({{image, QStringLiteral("renamed while open.exfat")},
                                        {dir + u'/' + lower, QStringLiteral("renamed while open 2.exfat")}},
                                       &skipped),
                     0);
            const qint64 took = t.elapsed();
            QVERIFY2(took < 800, qPrintable(QStringLiteral("%1 ms").arg(took)));
            QCOMPARE(skipped, (QStringList{QStringLiteral("my game.exfat: in use or read-only"),
                                           QStringLiteral("%1: in use or read-only").arg(lower)}));
            QVERIFY(!QFileInfo::exists(dir + QStringLiteral("/renamed while open.exfat")));
            QVERIFY(!QFileInfo::exists(dir + QStringLiteral("/renamed while open 2.exfat")));
            QCOMPARE(QFileInfo(dir).lastModified(), folderChanged);
        }
#endif
        q.clearFinished();
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

    // Library › Build images… with "Packed folder": one pack job for the
    // selected game, written to the chosen folder under the name the Build
    // page would give it, and the dump itself is left as it was.
    void libraryBulkBuildMakesAPackedFolder()
    {
        auto* lib = qobject_cast<pages::LibraryPage*>(m_window->page(QStringLiteral("library")));
        QVERIFY(lib);
        m_window->navigate(QStringLiteral("library"));
        // Settings › Build › Output name, as the Build page reads it.
        auto& st = m_ctx->settings();
        const QVariant presetBefore = st.value(gameinfo::keys::kNamePreset);
        const auto presetBack = qScopeGuard([&] { (void)st.setValue(gameinfo::keys::kNamePreset, presetBefore); });
        QVERIFY(st.setValue(gameinfo::keys::kNamePreset, QStringLiteral("ppsa_title_ver")).ok());
        const QString dir = m_root.filePath(QStringLiteral("bulk-lib"));
        const QString game = dir + QStringLiteral("/Bulk dump");
        // Both version fields, as a real dump has them (the Library reads the master version, the Build page the content version).
        QVERIFY(fwtest::writeTextFile(game + QStringLiteral("/sce_sys/param.json"),
                                      R"({"titleId":"PPSA00003","contentVersion":"01.000.000","masterVersion":"01.00",
                                          "localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"Bulk Test"}}})"));
        QVERIFY(fwtest::writeRandomFile(game + QStringLiteral("/eboot.bin"), 5000, 21));
        QVERIFY(fwtest::writeRandomFile(game + QStringLiteral("/data/big.bin"), 300000, 22));
        const auto dump = [&game] {
            QMap<QString, qint64> files;
            QDirIterator it(game, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                files.insert(QDir(game).relativeFilePath(it.filePath()), it.fileInfo().size());
            }
            return files;
        };
        const auto before = dump();
        QCOMPARE(before.size(), 3);

        lib->dropUrls({QUrl::fromLocalFile(dir)});
        QString key, seen;
        (void)QTest::qWaitFor([&] {
            for (const auto& item : lib->gamesView()->items())
                if (QDir::cleanPath(item.key).compare(QDir::cleanPath(game), Qt::CaseInsensitive) == 0) {
                    seen = item.subtitle;
                    if (seen.contains(QStringLiteral("01.000.000"))) // listed and read
                        key = item.key;
                }
            return !key.isEmpty();
        }, 20000);
        QVERIFY2(!key.isEmpty(), qPrintable(QStringLiteral("the library shows the dump as \"%1\"").arg(seen)));
        lib->gamesView()->setSelectedKeys({key});
        QPushButton* build = nullptr;
        for (auto* b : lib->findChildren<QPushButton*>())
            if (b->text() == QStringLiteral("Build images…"))
                build = b;
        QVERIFY(build);
        QTRY_VERIFY_WITH_TIMEOUT(build->isVisibleTo(lib) && build->isEnabled(), 5000);

        // The sheet runs its own event loop: a timer inside it picks the
        // format and the folder and presses Build.
        const QString outDir = m_root.filePath(QStringLiteral("bulk-out"));
        QVERIFY(QDir().mkpath(outDir));
        bool chosen = false;
        QTimer drive;
        drive.setInterval(30);
        connect(&drive, &QTimer::timeout, this, [&] {
            for (auto* s : m_window->findChildren<ui::Sheet*>()) {
                if (!s->isOpen())
                    continue;
                auto* format = s->findChild<ui::Segmented*>();
                auto* saveTo = s->findChild<ui::PathField*>();
                QPushButton* go = nullptr;
                for (auto* b : s->findChildren<QPushButton*>())
                    if (b->text() == QStringLiteral("Build"))
                        go = b;
                if (!format || !saveTo || !go)
                    continue;
                drive.stop();
                format->setCurrentValue(pages::kFormatZstdFolder);
                saveTo->setPath(outDir);
                chosen = format->currentValue().toString() == pages::kFormatZstdFolder;
                go->click();
                return;
            }
        });
        drive.start();
        auto& q = m_ctx->queue();
        const QString id = newJob(q, [&] { build->click(); });
        drive.stop();
        QVERIFY2(chosen, "the sheet did not open or has no Packed folder choice");
        QVERIFY2(!id.isEmpty(), "not exactly one job for the one selected game");

        auto rec = q.record(id);
        QVERIFY(rec);
        QCOMPARE(rec->kind, ampr::kPackJob);
        QCOMPARE(QDir::cleanPath(rec->parameters.value(QStringLiteral("source")).toString()).toLower(), QDir::cleanPath(game).toLower());
        const QString packed = QDir(outDir).filePath(QStringLiteral("PPSA00003 Bulk Test (01.000.000)"));
        QCOMPARE(QDir::cleanPath(rec->parameters.value(QStringLiteral("output")).toString()).toLower(), QDir::cleanPath(packed).toLower());
        QCOMPARE(rec->overrides.value(ampr::keys::kZstdPack).toBool(), true); // whatever the global switch says
        QVERIFY(waitTerminal(q, id));
        rec = q.record(id);
        QVERIFY2(rec->state == JobState::Succeeded, qPrintable(rec->error ? rec->error->toLogString() : QString()));
        QVERIFY(QFileInfo(packed + QStringLiteral("/ampr_zstd.index")).isFile());
        QVERIFY(QFileInfo(packed + QStringLiteral("/ampr_zstd-000.pak")).isFile());
        QVERIFY(QFileInfo(packed + QStringLiteral("/eboot.bin")).isFile());
        QVERIFY(!QFileInfo::exists(packed + QStringLiteral("/data/big.bin"))); // in the packs
        QCOMPARE(dump(), before);
        lib->gamesView()->setSelectedKeys({});
        q.clearFinished();
    }

    void cleanupTestCase()
    {
        m_window.reset();
        m_ctx.reset();
    }
};

QTEST_MAIN(TstImageTools)
#include "tst_image_tools.moc"
