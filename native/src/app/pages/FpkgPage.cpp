#include "app/pages/FpkgPage.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "core/FileUtil.h"
#include "engines/general/Module.h"

#include <QClipboard>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPointer>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrentRun>

namespace fw::pages {

using namespace ui;
using namespace theme;

FpkgPage::FpkgPage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent)
    : JobFormPage(ctx, queueModel, QStringLiteral("fpkg"), QStringLiteral("FPKG"),
                  QStringLiteral("Build a package from a folder, an .exfat, .ffpfsc or .ffpkg image, or a .gp5 project. A package counts as done only after it has been verified."),
                  QStringLiteral("Build package"), QStringLiteral("FPKG packaging"), parent)
{
    m_reader = new GameInfoReader(this);
    connect(m_reader, &GameInfoReader::ready, this, &FpkgPage::onSummary);

    // ---- Source
    auto* source = new Card(this);
    setSourceCard(source);
    source->setTitle(QStringLiteral("Source"));
    m_sourceType = new Segmented(source);
    m_sourceType->addSegment(QStringLiteral("Folder"), {}, QStringLiteral("folder"));
    m_sourceType->addSegment(QStringLiteral(".exfat"), {}, QStringLiteral("exfat"));
    m_sourceType->addSegment(QStringLiteral(".ffpfsc"), {}, QStringLiteral("ffpfsc"));
    m_sourceType->addSegment(QStringLiteral(".ffpkg"), {}, QStringLiteral("ffpkg"));
    m_sourceType->addSegment(QStringLiteral(".gp5 project"), {}, QStringLiteral("gp5"));
    m_sourceType->setAccessibleName(QStringLiteral("Source type"));
    auto* tw = new QWidget(source);
    auto* tl = vbox(tw, space::xs);
    tl->addWidget(label(QStringLiteral("Source type"), TextStyle::Body, color::fg_1, tw));
    tl->addWidget(m_sourceType, 0, Qt::AlignLeft);
    source->body()->addWidget(tw);
    m_source = new PathField(PathField::Mode::Directory, source);
    m_source->setStartDir(ctx.settings().value(general::keys::kLastSourceDir).toString());
    m_sourceHelp = caption(QStringLiteral("Content ID, title and version are read from sce_sys/param.json."), source);
    auto* sw = new QWidget(source);
    auto* sl = vbox(sw, space::xs);
    sl->addWidget(label(QStringLiteral("Game folder"), TextStyle::Body, color::fg_1, sw));
    sl->addWidget(m_source);
    sl->addWidget(m_sourceHelp);
    source->body()->addWidget(sw);
    auto* ow = new QWidget(source);
    auto* ol = vbox(ow, space::xs);
    ol->addWidget(label(QStringLiteral("Output folder"), TextStyle::Body, color::fg_1, ow));
    auto* orow = new QHBoxLayout;
    orow->setSpacing(space::sm);
    m_output = new PathField(PathField::Mode::Directory, ow);
    m_output->setDialogTitle(QStringLiteral("Choose the output folder for the package"));
    orow->addWidget(m_output, 1);
    m_nextToSource = new QCheckBox(QStringLiteral("Next to source"), ow);
    m_nextToSource->setChecked(true);
    orow->addWidget(m_nextToSource);
    ol->addLayout(orow);
    m_spaceRow = new MessageRow(ow);
    ol->addWidget(m_spaceRow);
    source->body()->addWidget(ow);
    form()->addWidget(source);
    connect(m_sourceType, &Segmented::currentChanged, this, [this](int) { onSourceTypeChanged(); });
    connect(m_source, &PathField::pathChanged, this, &FpkgPage::setSource);
    connect(m_nextToSource, &QCheckBox::toggled, this, [this](bool) { updateOutput(); });
    connect(m_output, &PathField::pathChanged, this, [this](const QString&) { checkSpace(); });

    // ---- Package details
    auto* details = new Card(this);
    details->setTitle(QStringLiteral("Package details"));
    m_contentId = textInput(QStringLiteral("UPxxxx-PPSAxxxxx_00-xxxxxxxxxxxxxxxx"), true, details);
    m_contentId->setMaxLength(36);
    details->body()->addWidget(field(QStringLiteral("Content ID"), m_contentId,
                                     QStringLiteral("36 characters, UPxxxx-PPSAxxxxx_00-xxxxxxxxxxxxxxxx. Read from param.json; edit only if you know why."), details));
    auto* grid = new QWidget(details);
    auto* gh = hbox(grid, space::md);
    gh->setAlignment(Qt::AlignTop);
    m_title = textInput({}, false, grid);
    m_version = textInput(QStringLiteral("01.000.000"), true, grid);
    gh->addWidget(field(QStringLiteral("Title"), m_title, {}, grid), 2);
    gh->addWidget(field(QStringLiteral("Version"), m_version, {}, grid), 1);
    m_packageType = new Segmented(grid);
    m_packageType->addSegment(QStringLiteral("App"), {}, QStringLiteral("app"));
    m_packageType->addSegment(QStringLiteral("Add-on"), {}, QStringLiteral("addon"));
    m_packageType->addSegment(QStringLiteral("Update"), {}, QStringLiteral("update"));
    m_packageType->setAccessibleName(QStringLiteral("Package type"));
    gh->addWidget(field(QStringLiteral("Package type"), m_packageType, {}, grid), 1);
    details->body()->addWidget(grid);
    m_basePackage = new PathField(PathField::Mode::OpenFile, details);
    m_basePackage->setFilter(QStringLiteral("Packages (*.pkg)"));
    m_basePackage->setDialogTitle(QStringLiteral("Choose the base package installed on the console"));
    m_baseField = field(QStringLiteral("Base package (.pkg installed on the console)"), m_basePackage,
                        QStringLiteral("Updates are built against the package the console already has."), details);
    m_baseField->hide();
    details->body()->addWidget(m_baseField);
    form()->addWidget(details);
    connect(m_packageType, &Segmented::currentChanged, this, [this](int) {
        m_baseField->setVisible(m_packageType->currentValue().toString() == QLatin1String("update"));
    });

    // ---- Option cards from the schema (Compression, PlayGo and param.json, Sony SDK ...)
    m_cards = new JobOptionsPanel(ctx, {QStringLiteral("FPKG"), QStringLiteral("PlayGo"), QStringLiteral("Firmware and SDK")}, {}, this);
    m_cards->setGroupCards(true);
    m_cards->build();
    form()->addWidget(m_cards);
    addJobOptionsCard(QStringLiteral("Build"), {QStringLiteral("Verification"), QStringLiteral("Performance"), QStringLiteral("Storage")},
                      {general::keys::kDefaultOutputDir}, QStringLiteral("All FPKG settings"));

    // ---- Right column: dependencies + summary
    QStringList depIds;
    for (const QString& section : {QStringLiteral("FPKG"), QStringLiteral("PlayGo"), QStringLiteral("Firmware and SDK"), QStringLiteral("Tools")})
        for (const SettingDef* d : ctx.schema().inSection(section))
            if (!d->dependencyId.isEmpty() && !depIds.contains(d->dependencyId))
                depIds << d->dependencyId;
    auto* depsCard = new Card(this);
    m_depsBadge = new Badge(QStringLiteral("Checking…"), Tone::Neutral, depsCard);
    auto* depsRow = depsCard->setTitle(QStringLiteral("Dependencies"), m_depsBadge);
    auto* again = button(QStringLiteral("Check again"), ButtonVariant::Subtle, QStringLiteral("refresh"), depsCard);
    setButtonSmall(again, true);
    depsRow->addWidget(again);
    depsCard->body()->setSpacing(0);
    m_deps = new DependencyList(ctx, depIds, depsCard);
    depsCard->body()->addWidget(m_deps);
    if (m_deps->isEmpty())
        depsCard->body()->addWidget(caption(QStringLiteral("No external tools are registered for FPKG builds in this build."), depsCard));
    connect(again, &QPushButton::clicked, m_deps, &DependencyList::checkAgain);
    connect(m_deps, &DependencyList::changed, this, [this] {
        if (m_deps->isEmpty()) {
            m_depsBadge->set(QStringLiteral("None"), Tone::Neutral);
            return;
        }
        const int ready = m_deps->readyCount(), total = m_deps->total();
        m_depsBadge->set(QStringLiteral("%1 of %2 ready").arg(ready).arg(total), ready == total ? Tone::Ok : Tone::Warn);
    });
    connect(m_deps, &DependencyList::fixRequested, this, [this](const QString&, const QString& how, const QString& settingKey) {
        emit navigateRequested({QStringLiteral("settings"), {{QStringLiteral("section"), QStringLiteral("Tools")}, {QStringLiteral("key"), settingKey}, {QStringLiteral("howToFix"), how}}});
    });
    aside()->insertWidget(0, depsCard);

    auto* summary = new Card(this);
    summary->setTitle(QStringLiteral("Build summary"));
    summary->body()->setSpacing(space::sm);
    auto addStat = [summary](const QString& name, QLabel*& value) {
        auto* row = new QWidget(summary);
        auto* h = hbox(row, space::md);
        h->addWidget(label(name, TextStyle::Body, color::fg_1, row), 1);
        value = monoLabel(QStringLiteral("—"), row, color::fg_0);
        h->addWidget(value);
        summary->body()->addWidget(row);
    };
    addStat(QStringLiteral("Source size"), m_estSize);
    addStat(QStringLiteral("Free on output drive"), m_freeSpace);
    aside()->insertWidget(1, summary);

    setRequiredKinds({QStringLiteral("fpkg.build")});
    onSourceTypeChanged();
    m_deps->refresh();
}

QString FpkgPage::sourceType() const { return m_sourceType->currentValue().toString(); }

void FpkgPage::onSourceTypeChanged()
{
    const QString t = sourceType();
    const bool folder = t == QLatin1String("folder");
    m_source->setMode(folder ? PathField::Mode::Directory : PathField::Mode::OpenFile);
    if (t == QLatin1String("exfat"))
        m_source->setFilter(QStringLiteral("exFAT images (*.exfat)"));
    else if (t == QLatin1String("ffpfsc"))
        m_source->setFilter(QStringLiteral("PFS images (*.ffpfsc *.ffpfs)"));
    else if (t == QLatin1String("ffpkg"))
        m_source->setFilter(QStringLiteral("FFPKG images (*.ffpkg)"));
    else if (t == QLatin1String("gp5"))
        m_source->setFilter(QStringLiteral("GP5 projects (*.gp5)"));
    m_sourceHelp->setText(folder ? QStringLiteral("Content ID, title and version are read from sce_sys/param.json.")
                                 : QStringLiteral("The image is read directly; nothing is extracted to disk unless the engine needs to."));
    m_source->setPlaceholder(folder ? QStringLiteral("Folder that holds sce_sys\\param.json") : QStringLiteral("Choose a file"));
    m_source->setDialogTitle(folder ? QStringLiteral("Choose the game folder") : QStringLiteral("Choose the source image"));
    refresh();
}

bool FpkgPage::acceptsDrop(const QList<QUrl>& urls) const
{
    if (urls.isEmpty())
        return false;
    const QFileInfo fi(urls.first().toLocalFile());
    static const QStringList exts = {QStringLiteral("exfat"), QStringLiteral("ffpfsc"), QStringLiteral("ffpfs"), QStringLiteral("ffpkg"), QStringLiteral("gp5")};
    return fi.isDir() || exts.contains(fi.suffix().toLower());
}

void FpkgPage::dropUrls(const QList<QUrl>& urls)
{
    if (acceptsDrop(urls))
        setSource(urls.first().toLocalFile());
}

void FpkgPage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("source")))
        setSource(args.value(QStringLiteral("source")).toString());
}

void FpkgPage::refresh()
{
    if (!m_source->path().isEmpty())
        setSource(m_source->path());
}

void FpkgPage::setSource(const QString& path)
{
    const QFileInfo fi(path);
    if (fi.isDir())
        m_sourceType->setCurrentValue(QStringLiteral("folder"));
    else {
        const QString ext = fi.suffix().toLower();
        if (ext == QLatin1String("exfat")) m_sourceType->setCurrentValue(QStringLiteral("exfat"));
        else if (ext == QLatin1String("ffpfsc") || ext == QLatin1String("ffpfs")) m_sourceType->setCurrentValue(QStringLiteral("ffpfsc"));
        else if (ext == QLatin1String("ffpkg")) m_sourceType->setCurrentValue(QStringLiteral("ffpkg"));
        else if (ext == QLatin1String("gp5")) m_sourceType->setCurrentValue(QStringLiteral("gp5"));
    }
    if (m_source->path() != path)
        m_source->setPath(path);
    clearFormError();
    if (path.isEmpty()) {
        m_summary = {};
        updateOutput();
        return;
    }
    if (fi.isDir()) {
        m_reader->summarize(path, true);
        (void)m_ctx.settings().setValue(general::keys::kLastSourceDir, fi.absolutePath());
    } else {
        GameSummary s;
        s.folder = path;
        s.exists = fi.exists();
        s.isGame = fi.exists();
        s.bytes = fi.size();
        s.title = fi.completeBaseName();
        static const QRegularExpression re(QStringLiteral("(CUSA|PPSA|PPLH)[-_ ]?(\\d{5})"), QRegularExpression::CaseInsensitiveOption);
        if (const auto m = re.match(fi.fileName()); m.hasMatch())
            s.titleId = (m.captured(1) + m.captured(2)).toUpper();
        onSummary(s);
    }
}

void FpkgPage::onSummary(const GameSummary& s)
{
    m_summary = s;
    m_source->setError(s.exists ? QString() : QStringLiteral("This source does not exist."));
    if (s.exists) {
        if (!s.contentId.isEmpty())
            m_contentId->setText(s.contentId);
        if (!s.title.isEmpty())
            m_title->setText(s.title);
        if (!s.version.isEmpty())
            m_version->setText(s.version);
    }
    m_estSize->setText(s.bytes > 0 ? formatBytes(s.bytes) : QStringLiteral("—"));
    updateOutput();
    emit statusContextChanged();
}

void FpkgPage::updateOutput()
{
    if (m_nextToSource->isChecked()) {
        if (m_summary.exists) {
            const QFileInfo fi(m_summary.folder);
            m_output->setPath(fi.absolutePath() + u'/' + fi.completeBaseName() + QStringLiteral("-pkg"));
        }
        m_output->setReadOnly(true);
    } else {
        m_output->setReadOnly(false);
    }
    checkSpace();
}

void FpkgPage::checkSpace()
{
    const QString folder = m_output->path().isEmpty() ? (m_summary.exists ? QFileInfo(m_summary.folder).absolutePath() : QString()) : m_output->path();
    m_spaceOk = true;
    m_spaceProblem.clear();
    if (folder.isEmpty()) {
        m_spaceRow->clear();
        m_freeSpace->setText(QStringLiteral("—"));
        return;
    }
    const qint64 needed = m_summary.bytes;
    const quint64 gen = ++m_spaceGen;
    QPointer<FpkgPage> self(this);
    (void)QtConcurrent::run([self, folder, needed, gen] {
        QString probe = folder;
        while (!probe.isEmpty() && !QFileInfo::exists(probe))
            probe = QFileInfo(probe).absolutePath() == probe ? QString() : QFileInfo(probe).absolutePath();
        auto ds = diskSpace(probe.isEmpty() ? folder : probe);
        auto vol = checkOutputVolume(probe.isEmpty() ? folder : probe, needed);
        QMetaObject::invokeMethod(self.data(), [self, ds, vol, folder, needed, gen] {
            if (!self || gen != self->m_spaceGen)
                return;
            self->m_output->setError({});
            if (!ds.ok()) {
                self->m_spaceRow->set(QStringLiteral("The output drive is not available."), Tone::Warn);
                self->m_freeSpace->setText(QStringLiteral("—"));
                return;
            }
            self->m_freeSpace->setText(formatBytes(ds->available));
            if (!vol.ok()) {
                self->m_spaceOk = false;
                self->m_spaceProblem = vol.error().message;
                self->m_output->setError(vol.error().message);
                self->m_spaceRow->clear();
                return;
            }
            const QString drive = driveLabel(folder);
            if (needed > 0 && ds->available < needed + general::freeSpaceMarginBytes(self->m_ctx.settings().snapshot())) {
                self->m_spaceOk = false;
                self->m_spaceProblem = QStringLiteral("Not enough space on %1: the package may need up to %2; %3 is free.").arg(drive, formatBytes(needed), formatBytes(ds->available));
                self->m_spaceRow->set(self->m_spaceProblem, Tone::Err);
                self->m_output->setError(QStringLiteral("Not enough space"));
                return;
            }
            QString text = QStringLiteral("%1 free on %2.").arg(formatBytes(ds->available), drive);
            if (needed > 0)
                text += QStringLiteral(" The package needs up to %1.").arg(formatBytes(needed));
            Tone tone = Tone::Ok;
            if (!vol->isEmpty()) {
                text += u' ' + vol->join(u' ');
                tone = Tone::Warn;
            }
            self->m_spaceRow->set(text, tone);
        }, Qt::QueuedConnection);
    });
}

QString FpkgPage::validateForm()
{
    if (m_source->path().isEmpty())
        return QStringLiteral("Choose the source first.");
    if (!m_summary.exists)
        return QStringLiteral("The source does not exist.");
    if (sourceType() == QLatin1String("folder") && !m_summary.isGame)
        return QStringLiteral("The folder is not a game dump: sce_sys\\param.json is missing.");
    static const QRegularExpression cid(QStringLiteral("^[A-Z]{2}[0-9]{4}-[A-Z]{4}[0-9]{5}_00-[A-Z0-9]{16}$"));
    const QString c = m_contentId->text().trimmed();
    setInputError(m_contentId, !c.isEmpty() && !cid.match(c).hasMatch());
    if (!c.isEmpty() && !cid.match(c).hasMatch())
        return QStringLiteral("The Content ID must look like UP9000-PPSA01325_00-GHOSTOFYOTEI0000 (36 characters).");
    static const QRegularExpression ver(QStringLiteral("^\\d{2}\\.\\d{3}\\.\\d{3}$"));
    const QString v = m_version->text().trimmed();
    setInputError(m_version, !v.isEmpty() && !ver.match(v).hasMatch());
    if (!v.isEmpty() && !ver.match(v).hasMatch())
        return QStringLiteral("The version must be NN.NNN.NNN, for example 01.000.000.");
    if (m_packageType->currentValue().toString() == QLatin1String("update") && m_basePackage->path().isEmpty())
        return QStringLiteral("An update needs the base package that is installed on the console.");
    if (!m_spaceOk)
        return m_spaceProblem;
    if (m_cards->hasErrors())
        return m_cards->errorMessages().join(u' ');
    return {};
}

Result<QString> FpkgPage::submit(bool)
{
    QJsonObject details;
    if (!m_contentId->text().trimmed().isEmpty())
        details.insert(QStringLiteral("contentId"), m_contentId->text().trimmed());
    if (!m_title->text().trimmed().isEmpty())
        details.insert(QStringLiteral("title"), m_title->text().trimmed());
    if (!m_version->text().trimmed().isEmpty())
        details.insert(QStringLiteral("contentVersion"), m_version->text().trimmed());
    details.insert(QStringLiteral("packageType"), m_packageType->currentValue().toString());
    if (m_packageType->currentValue().toString() == QLatin1String("update"))
        details.insert(QStringLiteral("basePackage"), m_basePackage->path());
    details.insert(QStringLiteral("sourceBytes"), m_summary.bytes);
    QVariantMap overrides = m_cards->overrides();
    const QVariantMap more = options()->overrides();
    for (auto it = more.cbegin(); it != more.cend(); ++it)
        overrides.insert(it.key(), it.value());
    return jobs::submitFpkgBuild(m_ctx, sourceType(), m_summary.folder, m_output->path(), details, overrides);
}

QString FpkgPage::statusContext() const
{
    return m_summary.exists ? QStringLiteral("%1 · %2").arg(m_summary.title, formatBytes(m_summary.bytes)) : QString();
}

} // namespace fw::pages
