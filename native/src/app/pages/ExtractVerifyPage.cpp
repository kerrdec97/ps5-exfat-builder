#include "app/pages/ExtractVerifyPage.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "core/FileUtil.h"
#include "engines/general/Module.h"

#include <QFileInfo>

namespace fw::pages {

using namespace ui;
using namespace theme;

static QString describe(const QString& path)
{
    const QFileInfo fi(path);
    const QString ext = fi.suffix().toLower();
    QString kind;
    if (ext == QLatin1String("exfat")) kind = QStringLiteral("exFAT image");
    else if (ext == QLatin1String("ffpkg")) kind = QStringLiteral("FFPKG (UFS2) image");
    else if (ext == QLatin1String("ffpfsc")) kind = QStringLiteral("FFPFSC image");
    else if (ext == QLatin1String("ffpfs")) kind = QStringLiteral("FFPFS image");
    else if (ext == QLatin1String("pkg")) kind = QStringLiteral("FPKG package");
    else return {};
    return QStringLiteral("%1 · %2").arg(kind, formatBytes(fi.size()));
}

ExtractVerifyPage::ExtractVerifyPage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent)
    : JobFormPage(ctx, queueModel, QStringLiteral("extract"), QStringLiteral("Extract & verify"),
                  QStringLiteral("Read an image or package back: check it against its own checksums, or extract it to a folder."),
                  QStringLiteral("Extract"), QStringLiteral("Extracting and verifying"), parent)
{
    auto* source = new Card(this);
    setSourceCard(source);
    m_sourceBadge = new Badge(QStringLiteral("Choose a file"), Tone::Neutral, source);
    source->setTitle(QStringLiteral("Source"), m_sourceBadge);
    m_source = new PathField(PathField::Mode::OpenFile, source);
    m_source->setFilter(QStringLiteral("Images and packages (*.exfat *.ffpkg *.ffpfsc *.ffpfs *.pkg);;All files (*)"));
    m_source->setDialogTitle(QStringLiteral("Choose an image or package"));
    m_source->setPlaceholder(QStringLiteral(".exfat, .ffpkg, .ffpfsc or .pkg"));
    m_source->setStartDir(ctx.settings().value(general::keys::kDefaultOutputDir).toString());
    auto* sw = new QWidget(source);
    auto* sl = vbox(sw, space::xs);
    sl->addWidget(label(QStringLiteral("Image or package"), TextStyle::Body, color::fg_1, sw));
    sl->addWidget(m_source);
    m_detected = caption(QStringLiteral("Drop a file anywhere on this page. The format is detected from the extension."), sw);
    sl->addWidget(m_detected);
    source->body()->addWidget(sw);
    m_mode = new Segmented(source);
    m_mode->addSegment(QStringLiteral("Extract and verify"), QStringLiteral("extract"), QStringLiteral("extract"));
    m_mode->addSegment(QStringLiteral("Verify only"), QStringLiteral("verified"), QStringLiteral("verify"));
    m_mode->setAccessibleName(QStringLiteral("Mode"));
    source->body()->addWidget(field(QStringLiteral("Mode"), m_mode, QStringLiteral("Verify only reads the whole file and compares checksums; nothing is written."), source));
    form()->addWidget(source);

    auto* dest = new Card(this);
    dest->setTitle(QStringLiteral("Destination"));
    m_destination = new PathField(PathField::Mode::Directory, dest);
    m_destination->setDialogTitle(QStringLiteral("Choose where to extract"));
    m_destination->setPlaceholder(QStringLiteral("A new folder next to the image"));
    m_destinationField = field(QStringLiteral("Extract to"), m_destination, {}, dest);
    dest->body()->addWidget(m_destinationField);
    m_destHelp = caption(QStringLiteral("The folder is created if it does not exist. Existing files are never overwritten."), dest);
    dest->body()->addWidget(m_destHelp);
    form()->addWidget(dest);

    addJobOptionsCard(QStringLiteral("Job settings"), {QStringLiteral("Verification"), QStringLiteral("Performance")}, {},
                      QStringLiteral("All verification settings"));

    connect(m_source, &PathField::pathChanged, this, &ExtractVerifyPage::setSource);
    connect(m_mode, &Segmented::currentChanged, this, [this](int) {
        const bool verify = verifyOnly();
        m_destinationField->setEnabled(!verify);
        m_destHelp->setText(verify ? QStringLiteral("Nothing is written in verify-only mode.")
                                   : QStringLiteral("The folder is created if it does not exist. Existing files are never overwritten."));
        primaryButton()->setText(verify ? QStringLiteral("Verify") : QStringLiteral("Extract"));
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
        m_sourceBadge->set(QStringLiteral("Choose a file"), Tone::Neutral);
        m_source->setError({});
    } else if (!fi.exists()) {
        m_sourceBadge->set(QStringLiteral("Not found"), Tone::Err);
        m_source->setError(QStringLiteral("This file does not exist."));
    } else if (describe(file).isEmpty()) {
        m_sourceBadge->set(QStringLiteral("Unknown format"), Tone::Warn);
        m_source->setError(QStringLiteral("Choose an .exfat, .ffpkg, .ffpfsc or .pkg file."));
    } else {
        m_sourceBadge->set(QStringLiteral("Ready"), Tone::Ok);
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
        return QStringLiteral("Choose an image or package first.");
    if (!QFileInfo::exists(file))
        return QStringLiteral("The file does not exist.");
    if (describe(file).isEmpty())
        return QStringLiteral("Choose an .exfat, .ffpkg, .ffpfsc or .pkg file.");
    if (!verifyOnly() && !m_destination->path().isEmpty() && isSameOrInside(file, m_destination->path()))
        return QStringLiteral("The destination cannot contain the source file.");
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

} // namespace fw::pages
