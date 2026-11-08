#include "app/pages/BuildImagePage.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/Icons.h"
#include "core/FileUtil.h"
#include "engines/general/Module.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QStorageInfo>
#include <QtConcurrent/QtConcurrentRun>

namespace fw::pages {

using namespace ui;
using namespace theme;


BuildImagePage::BuildImagePage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent)
    : JobFormPage(ctx, queueModel, QStringLiteral("build"), QStringLiteral("Build image"),
                  QStringLiteral("Create an exFAT, FFPKG or FFPFSC image from a game dump. Jobs run in the queue; you can keep working."),
                  QStringLiteral("Build now"), QStringLiteral("Image building"), parent)
{
    m_reader = new GameInfoReader(this);
    connect(m_reader, &GameInfoReader::ready, this, &BuildImagePage::onSummary);

    // ---- Source: a drop zone until a dump is chosen, then the game card.
    auto* source = new Card(this);
    setSourceCard(source);
    m_sourceBadge = new Badge(QStringLiteral("Step 1"), Tone::Neutral, source);
    m_sourceBadge->setIconName(QString());
    source->setTitle(QStringLiteral("Game dump"), m_sourceBadge);
    m_source = new PathField(PathField::Mode::Directory, source);
    m_source->setDialogTitle(QStringLiteral("Choose the game dump folder"));
    m_source->setPlaceholder(QStringLiteral("Folder that holds sce_sys\\param.json"));
    m_source->setStartDir(ctx.settings().value(general::keys::kLastSourceDir).toString());
    m_source->hide(); // the model behind the drop zone and the Change button
    source->body()->addWidget(m_source);
    m_drop = new DropZone(QStringLiteral("folder_open"), QStringLiteral("Drop a game dump folder here"),
                          QStringLiteral("or click to browse \u2014 the folder that holds eboot.bin and sce_sys"), source);
    connect(m_drop, &DropZone::clicked, m_source->browseButton(), &QPushButton::click);
    source->body()->addWidget(m_drop);
    m_strip = new Card(source, Card::Variant::Inset);
    auto* strip = static_cast<Card*>(m_strip);
    strip->body()->setContentsMargins(2, 4, 2, 4);
    auto* sh = new QHBoxLayout;
    sh->setSpacing(space::md);
    m_stripTile = new IconTile(QStringLiteral("game"), 48, strip);
    sh->addWidget(m_stripTile);
    auto* scol = new QVBoxLayout;
    scol->setSpacing(3);
    m_stripTitle = new ElideLabel({}, Qt::ElideRight, strip);
    m_stripTitle->setFont(font::subtitle());
    setTextColor(m_stripTitle, color::fg_0);
    m_stripMeta = new ElideLabel({}, Qt::ElideMiddle, strip);
    m_stripMeta->setFont(font::caption());
    setTextColor(m_stripMeta, color::fg_3);
    m_stripPath = new ElideLabel({}, Qt::ElideMiddle, strip);
    m_stripPath->setFont(font::monoSmall());
    setTextColor(m_stripPath, color::fg_4);
    scol->addWidget(m_stripTitle);
    scol->addWidget(m_stripMeta);
    scol->addWidget(m_stripPath);
    sh->addLayout(scol, 1);
    auto* rescan = new IconButton(QStringLiteral("refresh"), QStringLiteral("Read the dump again"), IconButton::Size::Regular, strip);
    connect(rescan, &QToolButton::clicked, this, &BuildImagePage::refresh);
    sh->addWidget(rescan);
    auto* change = button(QStringLiteral("Change\u2026"), ButtonVariant::Secondary, QStringLiteral("folder"), strip);
    connect(change, &QPushButton::clicked, m_source->browseButton(), &QPushButton::click);
    sh->addWidget(change);
    strip->body()->addLayout(sh);
    m_strip->hide();
    source->body()->addWidget(m_strip);
    m_sourceMsg = new MessageRow(source);
    source->body()->addWidget(m_sourceMsg);
    form()->addWidget(source);

    // ---- Format: three big tiles.
    auto* output = new Card(this);
    output->setTitle(QStringLiteral("Image format"));
    m_format = new ChoiceTiles(output);
    m_format->addChoice(QStringLiteral("exFAT"), QStringLiteral("Fastest to build. Mounts with ShadowMount+, any loader and on a PC."),
                        QStringLiteral("drive"), jobs::kFormatExfat, QStringLiteral("Popular"));
    m_format->addChoice(QStringLiteral("FFPKG \u00b7 UFS2"), QStringLiteral("The UFS2 image ShadowMount+ mounts without extracting."),
                        QStringLiteral("package"), jobs::kFormatFfpkg);
    m_format->addChoice(QStringLiteral("FFPFSC \u00b7 PFS"), QStringLiteral("Compressed PFS image. Smallest on disk; takes longer to build."),
                        QStringLiteral("compress"), jobs::kFormatFfpfsc, QStringLiteral("Smallest"));
    m_format->setAccessibleName(QStringLiteral("Image format"));
    m_formatHelp = caption({}, output);
    m_formatHelp->hide();
    output->body()->addWidget(m_format);
    form()->addWidget(output);
    auto* dest = new Card(this);
    dest->setTitle(QStringLiteral("Save to"));
    output = dest;
    connect(m_format, &ChoiceTiles::currentChanged, this, [this](int) { updateOutput(); });

    auto* grid = new QWidget(output);
    auto* gh = hbox(grid, space::md);
    gh->setAlignment(Qt::AlignTop);
    m_output = new PathField(PathField::Mode::Directory, grid);
    m_output->setDialogTitle(QStringLiteral("Choose the output folder"));
    m_output->setPlaceholder(QStringLiteral("Default output folder"));
    auto* outField = new QWidget(grid);
    auto* ol = vbox(outField, space::xs);
    ol->addWidget(label(QStringLiteral("Folder"), TextStyle::Body, color::fg_1, outField));
    ol->addWidget(m_output);
    m_spaceRow = new MessageRow(outField);
    ol->addWidget(m_spaceRow);
    gh->addWidget(outField, 1);
    m_fileName = textInput({}, true, grid);
    m_fileName->setReadOnly(true);
    m_fileName->setAccessibleName(QStringLiteral("File name"));
    m_fileName->setToolTip(QStringLiteral("From the title ID, name and version of the dump (Settings › Build › Output name)."));
    gh->addWidget(field(QStringLiteral("File name"), m_fileName, {}, grid), 1);
    output->body()->addWidget(grid);
    m_spaceBar = new InfoBar(Tone::Err, output);
    m_spaceBar->hide();
    output->body()->addWidget(m_spaceBar);
    form()->addWidget(output);
    connect(m_output, &PathField::pathChanged, this, [this](const QString&) { checkSpace(); });

    // ---- Job settings
    addJobOptionsCard(QStringLiteral("Cluster size, image size, verification, threads, temporary folder"),
                      {QStringLiteral("Build"), QStringLiteral("Verification"), QStringLiteral("Performance"), QStringLiteral("Storage"), QStringLiteral("PFS")},
                      {general::keys::kDefaultOutputDir}, QStringLiteral("All build settings"));

    connect(m_source, &PathField::pathChanged, this, &BuildImagePage::setSource);
    connect(&ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant&) {
        if (key == general::keys::kDefaultOutputDir)
            updateOutput();
    });
    setRequiredKinds({jobs::imageBuildKind(jobs::kFormatExfat), jobs::imageBuildKind(jobs::kFormatFfpkg), jobs::imageBuildKind(jobs::kFormatFfpfsc)});
    updateOutput();
}

void BuildImagePage::setDragHighlight(bool on)
{
    JobFormPage::setDragHighlight(on);
    m_drop->setHighlighted(on);
}

QString BuildImagePage::format() const { return m_format->currentValue().toString(); }

bool BuildImagePage::acceptsDrop(const QList<QUrl>& urls) const
{
    return !urls.isEmpty() && QFileInfo(urls.first().toLocalFile()).isDir();
}

void BuildImagePage::dropUrls(const QList<QUrl>& urls)
{
    if (acceptsDrop(urls))
        m_source->setPath(urls.first().toLocalFile());
}

void BuildImagePage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("source")))
        m_source->setPath(args.value(QStringLiteral("source")).toString());
    if (args.contains(QStringLiteral("format")))
        m_format->setCurrentValue(args.value(QStringLiteral("format")));
}

void BuildImagePage::refresh()
{
    if (!m_source->path().isEmpty())
        setSource(m_source->path());
}

void BuildImagePage::setSource(const QString& folder)
{
    if (m_source->path() != folder)
        m_source->setPath(folder);
    clearFormError();
    if (folder.isEmpty()) {
        m_strip->hide();
        m_drop->show();
        m_sourceMsg->clear();
        m_sourceBadge->set(QStringLiteral("Step 1"), Tone::Neutral);
        m_sourceBadge->setIconName(QString());
        m_source->setError({});
        m_summary = {};
        updateOutput();
        return;
    }
    m_sourceBadge->set(QStringLiteral("Scanning…"), Tone::Info);
    m_sourceBadge->setIconName(QStringLiteral("loader"));
    m_reader->summarize(folder, true);
    (void)m_ctx.settings().setValue(general::keys::kLastSourceDir, QFileInfo(folder).absolutePath());
}

void BuildImagePage::onSummary(const GameSummary& s)
{
    m_summary = s;
    m_stripPath->setFullText(QDir::toNativeSeparators(s.folder.isEmpty() ? m_source->path() : s.folder));
    if (!s.exists) {
        m_strip->hide();
        m_drop->show();
        m_sourceMsg->set(QStringLiteral("This folder does not exist. Choose another or check the drive."), Tone::Err);
        m_sourceBadge->set(QStringLiteral("Not found"), Tone::Err);
    } else if (!s.isGame) {
        m_sourceMsg->set(QStringLiteral("No sce_sys\param.json or eboot.bin here. Choose the folder that holds them."), Tone::Warn);
        m_drop->hide();
        m_strip->show();
        m_stripTile->setTone(Tone::Warn);
        m_stripTitle->setFullText(s.title);
        m_stripMeta->setFullText(s.metaLine());
        m_sourceBadge->set(QStringLiteral("Not a game dump"), Tone::Warn);
    } else {
        m_sourceMsg->clear();
        m_drop->hide();
        m_strip->show();
        m_stripTile->setTone(Tone::Ok);
        m_stripTitle->setFullText(s.title);
        m_stripMeta->setFullText(s.metaLine());
        m_sourceBadge->set(QStringLiteral("Ready"), Tone::Ok);
    }
    updateOutput();
    emit statusContextChanged();
}

QString BuildImagePage::fileName() const
{
    if (!m_summary.exists)
        return {};
    return defaultImageStem(m_summary) + jobs::outputExtension(format());
}

qint64 BuildImagePage::neededBytes() const
{
    if (m_summary.bytes <= 0)
        return 0;
    const QString f = format();
    if (f == jobs::kFormatFfpfsc)
        return m_summary.bytes; // compressed: never more than the dump
    return qint64(double(m_summary.bytes) * 1.006) + 64LL * 1024 * 1024;
}

void BuildImagePage::updateOutput()
{
    const QString f = format();
    if (f == jobs::kFormatExfat)
        m_formatHelp->setText(QStringLiteral("exFAT: plain image, fastest to build and mount. Size is the dump size plus about 0.6 %."));
    else if (f == jobs::kFormatFfpkg)
        m_formatHelp->setText(QStringLiteral("FFPKG (UFS2): the image format ShadowMount+ mounts without extraction."));
    else
        m_formatHelp->setText(QStringLiteral("FFPFSC: compressed PFS image. Smaller on disk; building takes longer."));
    m_fileName->setText(fileName());
    m_fileName->setCursorPosition(0);
    if (m_output->path().isEmpty())
        m_output->setPlaceholder(m_ctx.settings().value(general::keys::kDefaultOutputDir).toString().isEmpty()
                                     ? QStringLiteral("Next to the source folder")
                                     : nativePath(m_ctx.settings().value(general::keys::kDefaultOutputDir).toString()));
    checkSpace();
}

void BuildImagePage::checkSpace()
{
    QString folder = m_output->path();
    if (folder.isEmpty())
        folder = m_ctx.settings().value(general::keys::kDefaultOutputDir).toString();
    if (folder.isEmpty() && m_summary.exists)
        folder = QFileInfo(m_summary.folder).absolutePath();
    m_spaceBar->hide();
    m_spaceOk = true;
    m_spaceProblem.clear();
    if (folder.isEmpty()) {
        m_spaceRow->clear();
        return;
    }
    const qint64 needed = neededBytes();
    const quint64 gen = ++m_spaceGen;
    QPointer<BuildImagePage> self(this);
    (void)QtConcurrent::run([self, folder, needed, gen] {
        auto ds = diskSpace(folder);
        auto vol = checkOutputVolume(folder, needed);
        QMetaObject::invokeMethod(self.data(), [self, ds, vol, folder, needed, gen] {
            if (!self || gen != self->m_spaceGen)
                return;
            self->m_output->setError({});
            if (!ds.ok()) {
                self->m_spaceRow->set(QStringLiteral("This folder does not exist yet. It is created when the build starts."), Tone::Neutral);
                return;
            }
            const QString drive = driveLabel(folder);
            if (!vol.ok()) {
                self->m_spaceOk = false;
                self->m_spaceProblem = vol.error().message;
                self->m_output->setError(vol.error().message);
                self->m_spaceRow->clear();
                return;
            }
            if (needed > 0 && ds->available < needed + general::freeSpaceMarginBytes(self->m_ctx.settings().snapshot())) {
                self->showSpaceError(needed, ds->available, drive);
                return;
            }
            QString text = needed > 0 ? QStringLiteral("%1 free on %2. This image needs about %3.").arg(formatBytes(ds->available), drive, formatBytes(needed))
                                      : QStringLiteral("%1 free on %2.").arg(formatBytes(ds->available), drive);
            Tone tone = Tone::Ok;
            if (!vol->isEmpty()) {
                text += u' ' + vol->join(u' ');
                tone = Tone::Warn;
            }
            self->m_spaceRow->set(text, tone);
        }, Qt::QueuedConnection);
    });
}

void BuildImagePage::showSpaceError(qint64 needed, qint64 freeBytes, const QString& drive)
{
    m_spaceOk = false;
    m_spaceProblem = QStringLiteral("Not enough space on %1.").arg(drive);
    m_spaceRow->clear();
    m_output->setError(QStringLiteral("Not enough space"));
    m_spaceBar->setTone(Tone::Err);
    m_spaceBar->setTitle(QStringLiteral("Not enough space on %1").arg(drive));
    m_spaceBar->setMessage(QStringLiteral("This image needs %1; %2 is free. Free %3 or choose another output folder.")
                               .arg(formatBytes(needed), formatBytes(freeBytes), formatBytes(std::max<qint64>(0, needed - freeBytes))));
    m_spaceBar->clearLinks();
    // Suggest the volume with the most free space.
    QStorageInfo best;
    for (const QStorageInfo& si : QStorageInfo::mountedVolumes()) {
        if (!si.isValid() || !si.isReady() || si.isReadOnly() || si.bytesAvailable() < needed)
            continue;
        if (!best.isValid() || si.bytesAvailable() > best.bytesAvailable())
            best = si;
    }
    if (best.isValid()) {
        const QString root = best.rootPath();
        auto* l = m_spaceBar->addLink(QStringLiteral("Choose %1 (%2 free)").arg(driveLabel(root), formatBytes(best.bytesAvailable())));
        connect(l, &Link::activated, this, [this, root] { m_output->setPath(QDir(root).filePath(QStringLiteral("Images"))); });
    }
    auto* s = m_spaceBar->addLink(QStringLiteral("Open Storage settings"));
    connect(s, &Link::activated, this, [this] {
        emit navigateRequested({QStringLiteral("settings"), {{QStringLiteral("section"), QStringLiteral("Storage")}}});
    });
    m_spaceBar->show();
}

QString BuildImagePage::validateForm()
{
    if (m_source->path().isEmpty())
        return QStringLiteral("Choose the game dump folder first.");
    if (!m_summary.exists)
        return QStringLiteral("The source folder does not exist.");
    if (!m_summary.isGame)
        return QStringLiteral("The source folder is not a game dump: sce_sys\\param.json or eboot.bin is missing.");
    if (!m_spaceOk)
        return m_spaceProblem;
    return {};
}

Result<QString> BuildImagePage::submit(bool)
{
    QString out = m_output->path();
    if (out.isEmpty())
        out = m_ctx.settings().value(general::keys::kDefaultOutputDir).toString();
    if (out.isEmpty())
        out = QFileInfo(m_summary.folder).absolutePath();
    QJsonObject extra{{QStringLiteral("title"), m_summary.title}, {QStringLiteral("titleId"), m_summary.titleId},
                      {QStringLiteral("version"), m_summary.version}, {QStringLiteral("sourceBytes"), m_summary.bytes}};
    return jobs::submitImageBuild(m_ctx, format(), m_summary.folder, out, fileName(), options()->overrides(), extra);
}

QString BuildImagePage::statusContext() const
{
    return m_summary.isGame ? QStringLiteral("%1 · %2").arg(m_summary.title, formatBytes(m_summary.bytes)) : QString();
}

void BuildImagePage::applyDemoState(const QString& name)
{
    if (name == QLatin1String("space-error"))
        showSpaceError(48500000000LL, 34100000000LL, QStringLiteral("C:"));
    else
        JobFormPage::applyDemoState(name);
}

void BuildImagePage::clearDemoState()
{
    m_spaceBar->hide();
    m_output->setError({});
    m_spaceOk = true;
    JobFormPage::clearDemoState();
}

} // namespace fw::pages
