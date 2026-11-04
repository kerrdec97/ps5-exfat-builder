#include "app/pages/ExtractVerifyPage.h"
#include "app/i18n/I18n.h"

#include "app/AppContext.h"
#include "app/pages/CompareResultSheet.h"
#include "app/services/JobSubmit.h"
#include "core/FileUtil.h"
#include "engines/general/Module.h"

#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QScrollBar>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

const QString kCompareKind = QStringLiteral("image.compare");

// Badge::setIconName sticks, so every state sets its glyph.
void setBadge(Badge* b, const QString& text, Tone tone, const QString& icon = {})
{
    b->set(text, tone);
    b->setIconName(icon.isEmpty() ? defaultBadgeIcon(tone) : icon);
}

bool isCompareImage(const QString& path)
{
    const QString ext = QFileInfo(path).suffix().toLower();
    return ext == QLatin1String("exfat") || ext == QLatin1String("ffpkg") || ext == QLatin1String("ffpfs") || ext == QLatin1String("ffpfsc");
}

// What the screenshot demo shows (CI evidence of the result sheet).
QJsonObject demoCompareDetails()
{
    const auto side = [](const QString& path, const QString& kind, qint64 files, qint64 dirs, qint64 bytes) {
        return QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("kind"), kind}, {QStringLiteral("files"), files},
                           {QStringLiteral("dirs"), dirs}, {QStringLiteral("bytes"), bytes}};
    };
    return QJsonObject{
        {QStringLiteral("a"), side(QStringLiteral("D:/Images/PPSA01325 Astro Bot (01.002).exfat"), tr("exFAT image"), 18442, 1203, 48500000000LL)},
        {QStringLiteral("b"), side(QStringLiteral("D:/Images/PPSA01325 Astro Bot (01.002).ffpkg"), tr("FFPKG (UFS2) image"), 18443, 1204, 48500004096LL)},
        {QStringLiteral("depth"), QStringLiteral("full")},
        {QStringLiteral("identical"), false},
        {QStringLiteral("counts"), QJsonObject{{QStringLiteral("missing"), 1}, {QStringLiteral("extra"), 2}, {QStringLiteral("missingDirs"), 0},
                                               {QStringLiteral("extraDirs"), 1}, {QStringLiteral("sizeDifferent"), 1}, {QStringLiteral("contentDifferent"), 1},
                                               {QStringLiteral("unreadable"), 0}, {QStringLiteral("differences"), 6}, {QStringLiteral("filesInBoth"), 18441},
                                               {QStringLiteral("contentCompared"), 18440}, {QStringLiteral("bytesCompared"), 48499990000LL}, {QStringLiteral("padded"), 0}}},
        {QStringLiteral("missing"), QJsonArray{QStringLiteral("sce_sys/about/right.sprx")}},
        {QStringLiteral("extra"), QJsonArray{QStringLiteral("fakelib/libSceAmpr.sprx"), QStringLiteral("fakelib/README.txt")}},
        {QStringLiteral("missingDirs"), QJsonArray{}},
        {QStringLiteral("extraDirs"), QJsonArray{QStringLiteral("fakelib")}},
        {QStringLiteral("sizeDifferent"), QJsonArray{QJsonObject{{QStringLiteral("path"), QStringLiteral("sce_sys/param.json")}, {QStringLiteral("a"), 2214}, {QStringLiteral("b"), 2231}}}},
        {QStringLiteral("contentDifferent"), QJsonArray{QJsonObject{{QStringLiteral("path"), QStringLiteral("eboot.bin")}, {QStringLiteral("offset"), 1048592}}}},
        {QStringLiteral("unreadable"), QJsonArray{}},
        {QStringLiteral("listLimit"), 500},
        {QStringLiteral("truncated"), false},
    };
}

} // namespace

static QString describe(const QString& path)
{
    const QFileInfo fi(path);
    const QString ext = fi.suffix().toLower();
    QString kind;
    if (ext == QLatin1String("exfat")) kind = tr("exFAT image");
    else if (ext == QLatin1String("ffpkg")) kind = tr("FFPKG (UFS2) image");
    else if (ext == QLatin1String("ffpfsc")) kind = tr("FFPFSC image");
    else if (ext == QLatin1String("ffpfs")) kind = tr("FFPFS image");
    else if (ext == QLatin1String("pkg")) kind = tr("FPKG package");
    else return {};
    return QStringLiteral("%1 · %2").arg(kind, formatBytes(fi.size()));
}

ExtractVerifyPage::ExtractVerifyPage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent)
    : JobFormPage(ctx, queueModel, QStringLiteral("extract"), tr("Extract & verify"),
                  tr("Read an image or package back: check it against its own checksums, or extract it to a folder."),
                  tr("Extract"), tr("Extracting and verifying"), parent)
{
    auto* source = new Card(this);
    setSourceCard(source);
    m_sourceBadge = new Badge(tr("Choose a file"), Tone::Neutral, source);
    source->setTitle(tr("Source"), m_sourceBadge);
    m_source = new PathField(PathField::Mode::OpenFile, source);
    m_source->setFilter(tr("Images and packages (*.exfat *.ffpkg *.ffpfsc *.ffpfs *.pkg);;All files (*)"));
    m_source->setDialogTitle(tr("Choose an image or package"));
    m_source->setPlaceholder(tr(".exfat, .ffpkg, .ffpfsc or .pkg"));
    m_source->setStartDir(ctx.settings().value(general::keys::kDefaultOutputDir).toString());
    auto* sw = new QWidget(source);
    auto* sl = vbox(sw, space::xs);
    sl->addWidget(label(tr("Image or package"), TextStyle::Body, color::fg_1, sw));
    sl->addWidget(m_source);
    m_detected = caption(tr("Drop a file anywhere on this page. The format is detected from the extension."), sw);
    sl->addWidget(m_detected);
    source->body()->addWidget(sw);
    m_mode = new Segmented(source);
    m_mode->addSegment(tr("Extract and verify"), QStringLiteral("extract"), QStringLiteral("extract"));
    m_mode->addSegment(tr("Verify only"), QStringLiteral("verified"), QStringLiteral("verify"));
    m_mode->setAccessibleName(tr("Mode"));
    source->body()->addWidget(field(tr("Mode"), m_mode, tr("Verify only reads the whole file and compares checksums; nothing is written."), source));
    form()->addWidget(source);

    auto* dest = new Card(this);
    dest->setTitle(tr("Destination"));
    m_destination = new PathField(PathField::Mode::Directory, dest);
    m_destination->setDialogTitle(tr("Choose where to extract"));
    m_destination->setPlaceholder(tr("A new folder next to the image"));
    m_destinationField = field(tr("Extract to"), m_destination, {}, dest);
    dest->body()->addWidget(m_destinationField);
    m_destHelp = caption(tr("The folder is created if it does not exist. Existing files are never overwritten."), dest);
    dest->body()->addWidget(m_destHelp);
    form()->addWidget(dest);

    addJobOptionsCard(tr("Job settings"), {QStringLiteral("Verification"), QStringLiteral("Performance")}, {},
                      tr("All verification settings"));
    buildCompareCard();

    connect(m_source, &PathField::pathChanged, this, &ExtractVerifyPage::setSource);
    connect(m_mode, &Segmented::currentChanged, this, [this](int) {
        const bool verify = verifyOnly();
        m_destinationField->setEnabled(!verify);
        m_destHelp->setText(verify ? tr("Nothing is written in verify-only mode.")
                                   : tr("The folder is created if it does not exist. Existing files are never overwritten."));
        primaryButton()->setText(verify ? tr("Verify") : tr("Extract"));
        updateKinds();
    });
    updateKinds();
}

bool ExtractVerifyPage::verifyOnly() const { return m_mode->currentValue().toString() == QLatin1String("verify"); }

bool ExtractVerifyPage::acceptsDrop(const QList<QUrl>& urls) const
{
    return !urls.isEmpty() && !jobs::imageKindForFile(urls.first().toLocalFile(), QStringLiteral("verify")).isEmpty();
}

void ExtractVerifyPage::dropUrls(const QList<QUrl>& urls)
{
    if (acceptsDrop(urls))
        setSource(urls.first().toLocalFile());
}

void ExtractVerifyPage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("source")))
        setSource(args.value(QStringLiteral("source")).toString());
    if (args.value(QStringLiteral("mode")).toString() == QLatin1String("verify"))
        m_mode->setCurrentValue(QStringLiteral("verify"));
}

void ExtractVerifyPage::setSource(const QString& file)
{
    if (m_source->path() != file)
        m_source->setPath(file);
    clearFormError();
    const QFileInfo fi(file);
    if (file.isEmpty()) {
        m_sourceBadge->set(tr("Choose a file"), Tone::Neutral);
        m_source->setError({});
    } else if (!fi.exists()) {
        m_sourceBadge->set(tr("Not found"), Tone::Err);
        m_source->setError(tr("This file does not exist."));
    } else if (describe(file).isEmpty()) {
        m_sourceBadge->set(tr("Unknown format"), Tone::Warn);
        m_source->setError(tr("Choose an .exfat, .ffpkg, .ffpfsc or .pkg file."));
    } else {
        m_sourceBadge->set(tr("Ready"), Tone::Ok);
        m_source->setError({});
        m_detected->setText(describe(file));
        if (m_destination->path().isEmpty())
            m_destination->setPlaceholder(nativePath(fi.absolutePath() + u'/' + fi.completeBaseName()));
    }
    updateKinds();
}

void ExtractVerifyPage::updateKinds()
{
    const QString file = m_source->path();
    const QString kind = jobs::imageKindForFile(file, verifyOnly() ? QStringLiteral("verify") : QStringLiteral("extract"));
    setRequiredKinds(kind.isEmpty() ? QStringList{} : QStringList{kind});
}

QString ExtractVerifyPage::validateForm()
{
    const QString file = m_source->path();
    if (file.isEmpty())
        return tr("Choose an image or package first.");
    if (!QFileInfo::exists(file))
        return tr("The file does not exist.");
    if (describe(file).isEmpty())
        return tr("Choose an .exfat, .ffpkg, .ffpfsc or .pkg file.");
    if (!verifyOnly() && !m_destination->path().isEmpty() && isSameOrInside(file, m_destination->path()))
        return tr("The destination cannot contain the source file.");
    return {};
}

Result<QString> ExtractVerifyPage::submit(bool)
{
    const QString file = m_source->path();
    if (verifyOnly())
        return jobs::submitVerify(m_ctx, file, options()->overrides());
    QString dest = m_destination->path();
    if (dest.isEmpty()) {
        const QFileInfo fi(file);
        dest = fi.absolutePath() + u'/' + fi.completeBaseName();
    }
    return jobs::submitExtract(m_ctx, file, dest, options()->overrides());
}

// ---- Compare --------------------------------------------------------------------------

void ExtractVerifyPage::buildCompareCard()
{
    m_compareCard = new Card(this);
    m_compareBadge = new Badge(tr("Not compared"), Tone::Neutral, m_compareCard);
    m_compareCard->setTitle(tr("Compare"), m_compareBadge);
    m_compareCard->body()->addWidget(caption(tr("Find what differs between two images, or between an image and a folder: files only on one side, different sizes and different contents. Nothing is written."), m_compareCard));

    m_compareA = new PathField(PathField::Mode::OpenFile, m_compareCard);
    m_compareA->setFilter(tr("Images (*.exfat *.ffpkg *.ffpfs *.ffpfsc);;All files (*)"));
    m_compareA->setDialogTitle(tr("Choose image A"));
    m_compareA->setPlaceholder(tr(".exfat, .ffpkg, .ffpfs or .ffpfsc"));
    m_compareA->setStartDir(m_ctx.settings().value(general::keys::kDefaultOutputDir).toString());
    m_compareA->setAccessibleName(tr("Image A"));
    m_compareCard->body()->addWidget(field(tr("Image A (reference)"), m_compareA, {}, m_compareCard));

    auto* bw = new QWidget(m_compareCard);
    auto* bl = vbox(bw, space::xs);
    auto* bHead = new QWidget(bw);
    auto* bh = hbox(bHead, space::sm);
    bh->addWidget(label(tr("Compare with (B)"), TextStyle::Body, color::fg_1, bHead));
    bh->addStretch(1);
    m_compareBKind = new Segmented(bHead);
    m_compareBKind->addSegment(tr("Image"), QStringLiteral("drive"), QStringLiteral("image"));
    m_compareBKind->addSegment(tr("Folder"), QStringLiteral("folder"), QStringLiteral("folder"));
    m_compareBKind->setAccessibleName(tr("B is an image or a folder"));
    bh->addWidget(m_compareBKind);
    bl->addWidget(bHead);
    m_compareB = new PathField(PathField::Mode::OpenFile, bw);
    m_compareB->setStartDir(m_ctx.settings().value(general::keys::kDefaultOutputDir).toString());
    m_compareB->setAccessibleName(tr("Image or folder B"));
    bl->addWidget(m_compareB);
    m_compareCard->body()->addWidget(bw);

    m_compareDepth = new Segmented(m_compareCard);
    m_compareDepth->addSegment(tr("Names, sizes and contents"), QStringLiteral("file_search"), QStringLiteral("full"));
    m_compareDepth->addSegment(tr("Names and sizes"), QStringLiteral("list"), QStringLiteral("structure"));
    m_compareDepth->setAccessibleName(tr("Depth"));
    m_compareCard->body()->addWidget(field(tr("Depth"), m_compareDepth, tr("Comparing contents reads every file on both sides and takes as long as a full verify; names and sizes take seconds."), m_compareCard));

    m_compareMessage = new MessageRow(m_compareCard);
    m_compareCard->body()->addWidget(m_compareMessage);

    auto* row = new QWidget(m_compareCard);
    auto* h = hbox(row, space::sm);
    m_compareSummary = caption({}, row);
    m_compareSummary->setWordWrap(true);
    h->addWidget(m_compareSummary, 1);
    m_compareView = button(tr("View result"), ButtonVariant::Secondary, QStringLiteral("list"), row);
    m_compareView->hide();
    h->addWidget(m_compareView, 0, Qt::AlignTop);
    m_compareButton = button(tr("Compare"), ButtonVariant::Secondary, QStringLiteral("files"), row);
    h->addWidget(m_compareButton, 0, Qt::AlignTop);
    m_compareCard->body()->addWidget(row);
    form()->addWidget(m_compareCard);

    const bool available = jobs::isAvailable(m_ctx, kCompareKind);
    m_compareButton->setEnabled(available);
    m_compareButton->setToolTip(available ? QString() : tr("Not available in this build"));

    connect(m_compareBKind, &Segmented::currentChanged, this, [this](int) { updateCompareKind(); });
    connect(m_compareA, &PathField::pathChanged, this, [this](const QString&) { m_compareA->setError({}); m_compareMessage->clear(); });
    connect(m_compareB, &PathField::pathChanged, this, [this](const QString& p) {
        m_compareB->setError({});
        m_compareMessage->clear();
        // A dropped or typed folder switches B to folder mode (and back).
        const QFileInfo fi(p);
        if (fi.isDir() && m_compareBKind->currentValue().toString() != QLatin1String("folder"))
            m_compareBKind->setCurrentValue(QStringLiteral("folder"));
        else if (fi.isFile() && m_compareBKind->currentValue().toString() != QLatin1String("image"))
            m_compareBKind->setCurrentValue(QStringLiteral("image"));
    });
    connect(m_compareButton, &QPushButton::clicked, this, &ExtractVerifyPage::startCompare);
    connect(m_compareView, &QPushButton::clicked, this, [this] { (void)showCompareResult(); });
    connect(&m_ctx.queue(), &JobQueue::jobChanged, this, [this](const QString& id) { onCompareJob(id, false); });
    connect(&m_ctx.queue(), &JobQueue::jobFinished, this, [this](const QString& id) { onCompareJob(id, true); });
    updateCompareKind();

    // The latest comparison still in the queue (after a restart, or a page rebuild).
    for (const JobRecord& rec : m_ctx.queue().records())
        if (rec.kind == kCompareKind)
            m_compareJobId = rec.id;
    if (!m_compareJobId.isEmpty())
        onCompareJob(m_compareJobId, isTerminal(m_ctx.queue().record(m_compareJobId)->state));
    showCompareState();
}

void ExtractVerifyPage::updateCompareKind()
{
    const bool folder = m_compareBKind->currentValue().toString() == QLatin1String("folder");
    m_compareB->setMode(folder ? PathField::Mode::Directory : PathField::Mode::OpenFile);
    m_compareB->setFilter(tr("Images (*.exfat *.ffpkg *.ffpfs *.ffpfsc);;All files (*)"));
    m_compareB->setDialogTitle(folder ? tr("Choose folder B") : tr("Choose image B"));
    m_compareB->setPlaceholder(folder ? tr("A game folder, e.g. the dump the image was built from") : tr(".exfat, .ffpkg, .ffpfs or .ffpfsc"));
    m_compareB->setError({});
}

void ExtractVerifyPage::setCompareInputs(const QString& a, const QString& b, const QString& depth)
{
    m_compareA->setPath(a);
    m_compareBKind->setCurrentValue(QFileInfo(b).isDir() ? QStringLiteral("folder") : QStringLiteral("image"));
    m_compareB->setPath(b);
    m_compareDepth->setCurrentValue(depth == QLatin1String("structure") ? QStringLiteral("structure") : QStringLiteral("full"));
}

void ExtractVerifyPage::startCompare()
{
    m_compareMessage->clear();
    const QString a = m_compareA->path();
    const QString b = m_compareB->path();
    const bool bFolder = m_compareBKind->currentValue().toString() == QLatin1String("folder");
    bool ok = true;
    const QFileInfo fa(a), fb(b);
    if (a.isEmpty()) {
        m_compareA->setError(tr("Choose image A."));
        ok = false;
    } else if (!fa.isFile()) {
        m_compareA->setError(tr("This file does not exist."));
        ok = false;
    } else if (!isCompareImage(a)) {
        m_compareA->setError(tr("Choose an .exfat, .ffpkg, .ffpfs or .ffpfsc image."));
        ok = false;
    }
    if (b.isEmpty()) {
        m_compareB->setError(bFolder ? tr("Choose folder B.") : tr("Choose image B."));
        ok = false;
    } else if (bFolder ? !fb.isDir() : !fb.isFile()) {
        m_compareB->setError(bFolder ? tr("This folder does not exist.") : tr("This file does not exist."));
        ok = false;
    } else if (!bFolder && !isCompareImage(b)) {
        m_compareB->setError(tr("Choose an .exfat, .ffpkg, .ffpfs or .ffpfsc image, or switch to Folder."));
        ok = false;
    } else if (ok && QDir::cleanPath(fa.absoluteFilePath()) == QDir::cleanPath(fb.absoluteFilePath())) {
        m_compareB->setError(tr("Choose a different image than A."));
        ok = false;
    }
    if (!ok)
        return;
    auto r = jobs::submitCompare(m_ctx, a, b, m_compareDepth->currentValue().toString());
    if (!r.ok()) {
        m_compareMessage->set(r.error().message, Tone::Err);
        return;
    }
    m_compareJobId = r.value();
    m_ctx.queue().holdQueue(false);
    onCompareJob(m_compareJobId, false);
    emit toastRequested(Tone::Info, tr("Comparison queued"),
                        QStringLiteral("%1 ↔ %2").arg(fa.fileName(), fb.fileName()));
}

void ExtractVerifyPage::onCompareJob(const QString& id, bool finished)
{
    if (id.isEmpty())
        return;
    const auto rec = m_ctx.queue().record(id);
    if (!rec || rec->kind != kCompareKind)
        return;
    if (id != m_compareJobId) {
        // A comparison started elsewhere (Queue › Retry): the card follows the latest one.
        if (!finished && rec->state != JobState::Queued && rec->state != JobState::Running)
            return;
        m_compareJobId = id;
    }
    if (finished && isTerminal(rec->state)) {
        m_compareResult = {};
        m_compareResult.valid = true;
        m_compareResult.state = rec->state;
        if (rec->outcome) {
            m_compareResult.details = rec->outcome->details;
            m_compareResult.summary = rec->outcome->summary;
            m_compareResult.warnings = rec->outcome->warnings;
        }
        if (rec->error)
            m_compareResult.error = rec->error->message;
    }
    showCompareState();
}

void ExtractVerifyPage::showCompareState()
{
    if (m_compareDemo)
        return;
    const auto rec = m_compareJobId.isEmpty() ? std::nullopt : m_ctx.queue().record(m_compareJobId);
    const bool pending = rec && !isTerminal(rec->state);
    if (pending) {
        if (rec->state == JobState::Running) {
            setBadge(m_compareBadge, tr("Comparing…"), Tone::Info, QStringLiteral("loader"));
        } else {
            setBadge(m_compareBadge, rec->state == JobState::Paused ? tr("Paused") : tr("Queued"), Tone::Neutral);
        }
        m_compareSummary->setText(tr("%1 · follow it in the Jobs dock.").arg(rec->title));
        m_compareView->hide();
        m_compareButton->setEnabled(false);
        return;
    }
    m_compareButton->setEnabled(jobs::isAvailable(m_ctx, kCompareKind));
    const CompareResult& r = m_compareResult;
    const bool hasDetails = r.valid && r.state == JobState::Succeeded && r.details.contains(QStringLiteral("counts"));
    m_compareView->setVisible(hasDetails);
    if (!r.valid) {
        setBadge(m_compareBadge, tr("Not compared"), Tone::Neutral);
        m_compareSummary->setText(jobs::isAvailable(m_ctx, kCompareKind) ? QString() : tr("Comparing images is not available in this build."));
        return;
    }
    switch (r.state) {
    case JobState::Succeeded: {
        if (!hasDetails) {
            setBadge(m_compareBadge, tr("Done"), Tone::Neutral);
            m_compareSummary->setText(r.summary);
            break;
        }
        const qint64 n = r.details.value(QStringLiteral("counts")).toObject().value(QStringLiteral("differences")).toInteger();
        if (n == 0)
            setBadge(m_compareBadge, tr("Identical"), Tone::Ok);
        else
            setBadge(m_compareBadge, tr("%1 difference%2").arg(formatCount(n), n == 1 ? QString() : tr("s")), Tone::Warn);
        m_compareSummary->setText(r.summary);
        break;
    }
    case JobState::Failed:
        setBadge(m_compareBadge, tr("Failed"), Tone::Err);
        m_compareSummary->setText(r.error);
        break;
    default:
        setBadge(m_compareBadge, r.state == JobState::Cancelled ? tr("Cancelled") : tr("Interrupted"), Tone::Neutral);
        m_compareSummary->setText(tr("The comparison did not finish."));
        break;
    }
}

QString ExtractVerifyPage::compareStatus() const { return m_compareBadge->text(); }

CompareResultSheet* ExtractVerifyPage::showCompareResult()
{
    if (!m_compareResult.valid || m_compareResult.state != JobState::Succeeded
        || !m_compareResult.details.contains(QStringLiteral("counts")))
        return nullptr;
    auto* s = new CompareResultSheet(window(), m_compareResult.details, m_compareResult.summary, m_compareResult.warnings);
    s->open();
    return s;
}

QStringList ExtractVerifyPage::demoStates() const
{
    return {QStringLiteral("error"), QStringLiteral("compare"), QStringLiteral("compare-result")};
}

void ExtractVerifyPage::applyDemoState(const QString& name)
{
    if (name == QLatin1String("error")) {
        JobFormPage::applyDemoState(name);
        return;
    }
    const QJsonObject d = demoCompareDetails();
    const QString summary = tr("6 differences: 1 only in A, 3 only in B, 1 different size, 1 with different content.");
    if (name == QLatin1String("compare")) {
        m_compareDemo = true;
        m_compareA->setPath(d.value(QStringLiteral("a")).toObject().value(QStringLiteral("path")).toString());
        m_compareB->setPath(d.value(QStringLiteral("b")).toObject().value(QStringLiteral("path")).toString());
        setBadge(m_compareBadge, tr("6 differences"), Tone::Warn);
        m_compareSummary->setText(summary);
        m_compareView->show();
        scrollArea()->ensureWidgetVisible(m_compareCard, 0, space::xl);
    } else if (name == QLatin1String("compare-result")) {
        auto* s = new CompareResultSheet(window(), d, summary, {tr("B: Skipped fakelib/link.sprx: a link or special file.")});
        s->open();
        m_demoSheet = s;
    }
}

void ExtractVerifyPage::clearDemoState()
{
    JobFormPage::clearDemoState();
    if (auto* s = qobject_cast<Sheet*>(m_demoSheet.data()))
        s->close(0);
    if (m_compareDemo) {
        m_compareDemo = false;
        m_compareA->setPath({});
        m_compareB->setPath({});
        showCompareState();
        scrollArea()->verticalScrollBar()->setValue(0);
    }
}

} // namespace fw::pages
