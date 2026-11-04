#include "app/pages/ImageEditorPage.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "engines/general/Module.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonObject>

namespace fw::pages {

using namespace ui;
using namespace theme;

QString StagedOp::describe() const
{
    const QString shown = QStringLiteral("/") + imagePath;
    if (op == QLatin1String("add"))
        return QStringLiteral("Add %1 → %2").arg(nativePath(localPath), shown);
    if (op == QLatin1String("replace"))
        return QStringLiteral("Replace %1 with %2").arg(shown, nativePath(localPath));
    if (op == QLatin1String("delete"))
        return QStringLiteral("Delete %1").arg(shown);
    if (op == QLatin1String("mkdir"))
        return QStringLiteral("New folder %1").arg(shown);
    if (op == QLatin1String("rename"))
        return QStringLiteral("Rename %1 → %2").arg(shown, newName);
    return op;
}

ImageEditorPage::ImageEditorPage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent)
    : JobFormPage(ctx, queueModel, QStringLiteral("edit"), QStringLiteral("Edit image"),
                  QStringLiteral("Open an .exfat or .ffpkg image, stage changes and apply them in one job. The original is never lost before the edited image has verified."),
                  QStringLiteral("Apply changes"), QStringLiteral("Image editing"), parent)
{
    m_reader = new ImageTreeReader(this);
    connect(m_reader, &ImageTreeReader::ready, this, &ImageEditorPage::onTree);
    queueButton()->hide();

    auto* source = new Card(this);
    setSourceCard(source);
    m_badge = new Badge(QStringLiteral("Choose an image"), Tone::Neutral, source);
    source->setTitle(QStringLiteral("Image"), m_badge);
    m_image = new PathField(PathField::Mode::OpenFile, source);
    m_image->setFilter(QStringLiteral("Images (*.exfat *.ffpkg)"));
    m_image->setDialogTitle(QStringLiteral("Choose an image to edit"));
    m_image->setPlaceholder(QStringLiteral(".exfat or .ffpkg"));
    m_image->setStartDir(ctx.settings().value(general::keys::kDefaultOutputDir).toString());
    source->body()->addWidget(field(QStringLiteral("Image file"), m_image, QStringLiteral("Drop an image anywhere on this page. FFPFSC images cannot be edited; convert them first."), source));
    m_summary = caption({}, source);
    source->body()->addWidget(m_summary);
    form()->addWidget(source);
    connect(m_image, &PathField::pathChanged, this, &ImageEditorPage::open);

    auto* treeCard = new Card(this, Card::Variant::Table);
    auto* h = treeCard->header();
    h->addWidget(heading(QStringLiteral("Contents"), TextStyle::Subtitle, treeCard));
    h->addStretch(1);
    m_btnAddFiles = button(QStringLiteral("Add files…"), ButtonVariant::Secondary, QStringLiteral("file_plus"), treeCard);
    m_btnAddFolder = button(QStringLiteral("Add folder…"), ButtonVariant::Secondary, QStringLiteral("folder_plus"), treeCard);
    m_btnReplace = button(QStringLiteral("Replace…"), ButtonVariant::Secondary, QStringLiteral("replace"), treeCard);
    m_btnMkdir = button(QStringLiteral("New folder"), ButtonVariant::Secondary, QStringLiteral("folder_plus"), treeCard);
    m_btnRename = button(QStringLiteral("Rename"), ButtonVariant::Secondary, QStringLiteral("edit"), treeCard);
    m_btnDelete = button(QStringLiteral("Delete"), ButtonVariant::Secondary, QStringLiteral("trash"), treeCard);
    for (auto* b : {m_btnAddFiles, m_btnAddFolder, m_btnReplace, m_btnMkdir, m_btnRename, m_btnDelete})
        h->addWidget(b);
    connect(m_btnAddFiles, &QPushButton::clicked, this, [this] { addFiles(false); });
    connect(m_btnAddFolder, &QPushButton::clicked, this, [this] { addFiles(true); });
    connect(m_btnReplace, &QPushButton::clicked, this, &ImageEditorPage::replaceSelected);
    connect(m_btnMkdir, &QPushButton::clicked, this, &ImageEditorPage::newFolder);
    connect(m_btnRename, &QPushButton::clicked, this, &ImageEditorPage::renameSelected);
    connect(m_btnDelete, &QPushButton::clicked, this, &ImageEditorPage::deleteSelected);
    m_treeView = new QTreeWidget(treeCard);
    m_treeView->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Size"), QStringLiteral("Modified")});
    m_treeView->header()->setFixedHeight(32);
    m_treeView->header()->setStretchLastSection(false);
    m_treeView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_treeView->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_treeView->setColumnWidth(1, 110);
    m_treeView->setColumnWidth(2, 160);
    m_treeView->setUniformRowHeights(true);
    m_treeView->setMinimumHeight(12 * size::rowCompact + 40);
    m_treeView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_treeView->setIndentation(20);
    m_treeView->setFont(font::mono());
    treeCard->body()->addWidget(m_treeView);
    m_treeEmpty = new EmptyState(QStringLiteral("drive"), QStringLiteral("No image open"), QStringLiteral("Choose an .exfat or .ffpkg image above to browse what it contains."), treeCard);
    treeCard->body()->addWidget(m_treeEmpty);
    connect(m_treeView, &QTreeWidget::itemSelectionChanged, this, [this] {
        bool dir = false;
        const QString p = selectedPath(&dir);
        const bool open = !m_tree.nodes.empty();
        m_btnReplace->setEnabled(open && !p.isEmpty() && !dir);
        m_btnDelete->setEnabled(open && !p.isEmpty());
        m_btnRename->setEnabled(open && !p.isEmpty() && m_tree.format == QLatin1String("ffpkg"));
    });
    form()->addWidget(treeCard);

    m_stagedCard = new Card(this);
    m_clear = button(QStringLiteral("Clear"), ButtonVariant::Subtle, QStringLiteral("close"), m_stagedCard);
    setButtonSmall(m_clear, true);
    m_stagedCard->setTitle(QStringLiteral("Staged changes"), m_clear);
    m_stagedCard->body()->setSpacing(0);
    m_stagedCaption = caption(QStringLiteral("Nothing staged. Changes are collected here and applied together; nothing touches the image until you apply."), m_stagedCard);
    m_stagedCard->body()->addWidget(m_stagedCaption);
    auto* stagedList = new QWidget(m_stagedCard);
    m_stagedLayout = vbox(stagedList, 0);
    m_stagedCard->body()->addWidget(stagedList);
    connect(m_clear, &QPushButton::clicked, this, &ImageEditorPage::clearStaged);
    form()->addWidget(m_stagedCard);

    addJobOptionsCard(QStringLiteral("Job settings"), {QStringLiteral("Build"), QStringLiteral("Verification"), QStringLiteral("Performance")}, {}, QStringLiteral("All build settings"), false);

    auto* conv = new Card(this);
    conv->setTitle(QStringLiteral("Convert to…"));
    m_convertFormat = new Segmented(conv);
    m_convertFormat->addSegment(QStringLiteral("exFAT"), QStringLiteral("drive"), jobs::kFormatExfat);
    m_convertFormat->addSegment(QStringLiteral("FFPKG (UFS2)"), QStringLiteral("package"), jobs::kFormatFfpkg);
    m_convertFormat->addSegment(QStringLiteral("FFPFSC"), QStringLiteral("compress"), jobs::kFormatFfpfsc);
    m_convertFormat->setAccessibleName(QStringLiteral("Target format"));
    conv->body()->addWidget(field(QStringLiteral("Target format"), m_convertFormat, QStringLiteral("The image is read back, written in the new format next to it and verified; the original stays."), conv));
    m_convertOutput = new PathField(PathField::Mode::SaveFile, conv);
    m_convertOutput->setDialogTitle(QStringLiteral("Save the converted image as"));
    conv->body()->addWidget(field(QStringLiteral("Save as"), m_convertOutput, QStringLiteral("Empty: next to the source with the new extension."), conv));
    auto* crow = new QWidget(conv);
    auto* ch = hbox(crow, space::sm);
    ch->addStretch(1);
    m_convert = button(QStringLiteral("Convert"), ButtonVariant::Secondary, QStringLiteral("convert"), crow);
    ch->addWidget(m_convert);
    conv->body()->addWidget(crow);
    connect(m_convert, &QPushButton::clicked, this, &ImageEditorPage::convert);
    aside()->insertWidget(0, conv);

    rebuildTree();
    rebuildStaged();
    setRequiredKinds({});
}

void ImageEditorPage::open(const QString& image)
{
    if (m_image->path() != image)
        m_image->setPath(image);
    clearFormError();
    m_ops.clear();
    rebuildStaged();
    if (image.isEmpty()) {
        m_tree = {};
        rebuildTree();
        setRequiredKinds({});
        return;
    }
    const QString kind = jobs::imageEditKind(image);
    setRequiredKinds(kind.isEmpty() ? QStringList{} : QStringList{kind});
    m_badge->set(QStringLiteral("Reading…"), Tone::Info);
    m_badge->setIconName(QStringLiteral("loader"));
    m_reader->read(image);
}

void ImageEditorPage::onTree(const ImageTreeResult& r)
{
    m_tree = r;
    if (!r.error.isEmpty()) {
        m_badge->set(QStringLiteral("Cannot open"), Tone::Err);
        m_image->setError(r.error);
        m_summary->clear();
    } else {
        m_image->setError({});
        m_badge->set(r.problems.isEmpty() ? QStringLiteral("Ready to edit") : QStringLiteral("Damaged"), r.problems.isEmpty() ? Tone::Ok : Tone::Err);
        QStringList parts{r.format == QLatin1String("exfat") ? QStringLiteral("exFAT") : QStringLiteral("FFPKG (UFS2)")};
        if (!r.label.isEmpty())
            parts << QStringLiteral("label %1").arg(r.label);
        parts << formatBytes(r.totalBytes) << QStringLiteral("%1 files").arg(formatCount(r.fileCount)) << QStringLiteral("%1 folders").arg(formatCount(r.dirCount));
        if (!r.problems.isEmpty())
            parts << QStringLiteral("%1 problem%2: %3").arg(r.problems.size()).arg(r.problems.size() == 1 ? QString() : QStringLiteral("s")).arg(r.problems.first());
        else if (!r.warnings.isEmpty())
            parts << r.warnings.first();
        m_summary->setText(parts.join(QStringLiteral(" · ")));
    }
    rebuildTree();
    emit statusContextChanged();
}

void ImageEditorPage::rebuildTree()
{
    m_treeView->clear();
    const bool open = !m_tree.nodes.empty() && m_tree.error.isEmpty();
    m_treeView->setVisible(open);
    m_treeEmpty->setVisible(!open);
    m_btnAddFiles->setEnabled(open);
    m_btnAddFolder->setEnabled(open);
    m_btnMkdir->setEnabled(open);
    m_btnReplace->setEnabled(false);
    m_btnDelete->setEnabled(false);
    m_btnRename->setEnabled(false);
    m_convert->setEnabled(!m_image->path().isEmpty() && jobs::isAvailable(m_ctx, QStringLiteral("image.convert")));
    if (!open)
        return;
    std::vector<QTreeWidgetItem*> items(m_tree.nodes.size(), nullptr);
    for (size_t i = 1; i < m_tree.nodes.size(); ++i) {
        const ImageNode& n = m_tree.nodes[i];
        auto* it = new QTreeWidgetItem;
        it->setText(0, n.name);
        it->setData(0, Qt::UserRole, n.path);
        it->setData(0, Qt::UserRole + 1, n.isDir);
        it->setIcon(0, icon(n.isDir ? QStringLiteral("folder") : QStringLiteral("file"), color::fg_3));
        it->setText(1, n.isDir ? QString() : formatBytes(n.size));
        it->setForeground(1, color::fg_3);
        it->setText(2, n.modified.isValid() ? humanTime(n.modified) : QString());
        it->setForeground(2, color::fg_3);
        items[i] = it;
    }
    for (size_t i = 1; i < m_tree.nodes.size(); ++i) {
        const int parent = m_tree.nodes[i].parent;
        if (parent <= 0)
            m_treeView->addTopLevelItem(items[i]);
        else if (items[size_t(parent)])
            items[size_t(parent)]->addChild(items[i]);
    }
    m_treeView->sortItems(0, Qt::AscendingOrder);
}

QString ImageEditorPage::selectedPath(bool* isDir) const
{
    const auto sel = m_treeView->selectedItems();
    if (sel.isEmpty())
        return {};
    if (isDir)
        *isDir = sel.first()->data(0, Qt::UserRole + 1).toBool();
    return sel.first()->data(0, Qt::UserRole).toString();
}

void ImageEditorPage::stage(const StagedOp& op)
{
    m_ops.push_back(op);
    rebuildStaged();
}

void ImageEditorPage::clearStaged()
{
    m_ops.clear();
    rebuildStaged();
}

void ImageEditorPage::rebuildStaged()
{
    while (QLayoutItem* it = m_stagedLayout->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    m_stagedCaption->setVisible(m_ops.empty());
    m_clear->setVisible(!m_ops.empty());
    for (size_t i = 0; i < m_ops.size(); ++i) {
        auto* row = new QWidget(this);
        auto* h = hbox(row, space::sm, QMargins(0, 6, 0, 6));
        const QString iconName = m_ops[i].op == QLatin1String("delete") ? QStringLiteral("trash") : m_ops[i].op == QLatin1String("mkdir") ? QStringLiteral("folder_plus")
                               : m_ops[i].op == QLatin1String("rename") ? QStringLiteral("edit") : m_ops[i].op == QLatin1String("replace") ? QStringLiteral("replace") : QStringLiteral("file_plus");
        auto* ic = new QLabel(row);
        ic->setPixmap(iconPixmap(iconName, size::icon, m_ops[i].op == QLatin1String("delete") ? color::danger : color::fg_3, devicePixelRatioF()));
        ic->setFixedSize(size::icon, size::icon);
        h->addWidget(ic);
        auto* t = new ElideLabel(m_ops[i].describe(), Qt::ElideMiddle, row);
        t->setFont(font::mono());
        setTextColor(t, color::fg_1);
        h->addWidget(t, 1);
        auto* x = new IconButton(QStringLiteral("close"), QStringLiteral("Remove this change"), IconButton::Size::Small, row);
        connect(x, &QToolButton::clicked, this, [this, i] {
            if (i < m_ops.size())
                m_ops.erase(m_ops.begin() + long(i));
            rebuildStaged();
        });
        h->addWidget(x);
        m_stagedLayout->addWidget(row);
        if (i + 1 < m_ops.size())
            m_stagedLayout->addWidget(hairline(this));
    }
    primaryButton()->setText(m_ops.empty() ? QStringLiteral("Apply changes") : QStringLiteral("Apply %1 change%2").arg(m_ops.size()).arg(m_ops.size() == 1 ? QString() : QStringLiteral("s")));
    emit statusContextChanged();
}

void ImageEditorPage::addFiles(bool folder)
{
    bool dir = false;
    QString target = selectedPath(&dir);
    if (!target.isEmpty() && !dir)
        target = target.contains(u'/') ? target.section(u'/', 0, -2) : QString();
    if (folder) {
        const QString d = QFileDialog::getExistingDirectory(window(), QStringLiteral("Choose a folder to add"));
        if (d.isEmpty())
            return;
        stage({QStringLiteral("add"), target.isEmpty() ? QFileInfo(d).fileName() : target + u'/' + QFileInfo(d).fileName(), d, {}});
        return;
    }
    for (const QString& f : QFileDialog::getOpenFileNames(window(), QStringLiteral("Choose files to add")))
        stage({QStringLiteral("add"), target.isEmpty() ? QFileInfo(f).fileName() : target + u'/' + QFileInfo(f).fileName(), f, {}});
}

void ImageEditorPage::replaceSelected()
{
    bool dir = false;
    const QString p = selectedPath(&dir);
    if (p.isEmpty() || dir)
        return;
    const QString f = QFileDialog::getOpenFileName(window(), QStringLiteral("Choose the new content for %1").arg(p));
    if (!f.isEmpty())
        stage({QStringLiteral("replace"), p, f, {}});
}

void ImageEditorPage::deleteSelected()
{
    const QString p = selectedPath();
    if (!p.isEmpty())
        stage({QStringLiteral("delete"), p, {}, {}});
}

void ImageEditorPage::newFolder()
{
    bool dir = false;
    QString target = selectedPath(&dir);
    if (!target.isEmpty() && !dir)
        target = target.contains(u'/') ? target.section(u'/', 0, -2) : QString();
    auto* s = new Sheet(window(), QStringLiteral("New folder"), Sheet::Icon::None);
    auto* name = textInput(QStringLiteral("Folder name"), true, s);
    s->body()->addWidget(field(target.isEmpty() ? QStringLiteral("Name (in the image root)") : QStringLiteral("Name (in /%1)").arg(target), name, {}, s));
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Stage"), Sheet::Role::Primary);
    name->setFocus();
    if (s->exec() != 1)
        return;
    const QString n = name->text().trimmed();
    if (n.isEmpty() || n.contains(u'/'))
        return;
    stage({QStringLiteral("mkdir"), target.isEmpty() ? n : target + u'/' + n, {}, {}});
}

void ImageEditorPage::renameSelected()
{
    const QString p = selectedPath();
    if (p.isEmpty())
        return;
    auto* s = new Sheet(window(), QStringLiteral("Rename"), Sheet::Icon::None);
    auto* name = textInput({}, true, s);
    name->setText(p.section(u'/', -1));
    s->body()->addWidget(field(QStringLiteral("New name"), name, {}, s));
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Stage"), Sheet::Role::Primary);
    name->setFocus();
    name->selectAll();
    if (s->exec() != 1)
        return;
    const QString n = name->text().trimmed();
    if (n.isEmpty() || n.contains(u'/') || n == p.section(u'/', -1))
        return;
    stage({QStringLiteral("rename"), p, {}, n});
}

QJsonArray ImageEditorPage::operationsJson() const
{
    QJsonArray arr;
    const bool exfat = m_tree.format != QLatin1String("ffpkg");
    for (const StagedOp& o : m_ops) {
        QJsonObject j{{QStringLiteral("op"), o.op}};
        if (exfat) {
            j.insert(QStringLiteral("imagePath"), o.imagePath);
            if (!o.localPath.isEmpty())
                j.insert(QStringLiteral("localPath"), o.localPath);
        } else {
            j.insert(QStringLiteral("path"), QStringLiteral("/") + o.imagePath);
            if (!o.localPath.isEmpty())
                j.insert(QStringLiteral("source"), o.localPath);
            if (!o.newName.isEmpty())
                j.insert(QStringLiteral("newName"), o.newName);
        }
        arr.append(j);
    }
    return arr;
}

QString ImageEditorPage::validateForm()
{
    if (m_image->path().isEmpty())
        return QStringLiteral("Open an image first.");
    if (!m_tree.error.isEmpty())
        return m_tree.error;
    if (m_ops.empty())
        return QStringLiteral("Stage at least one change.");
    return {};
}

Result<QString> ImageEditorPage::submit(bool)
{
    int deletes = 0;
    QStringList lines;
    for (const auto& o : m_ops) {
        if (o.op == QLatin1String("delete") || o.op == QLatin1String("replace"))
            ++deletes;
        lines << o.describe().toHtmlEscaped();
    }
    if (!Sheet::confirm(window(), QStringLiteral("Apply %1 change%2 to %3?").arg(m_ops.size()).arg(m_ops.size() == 1 ? QString() : QStringLiteral("s")).arg(QFileInfo(m_image->path()).fileName().toHtmlEscaped()),
                        Sheet::monoSpan(lines.join(QStringLiteral("<br>"))) + (deletes > 0 ? QStringLiteral("<br><br>Deleted and replaced files cannot be recovered from the image afterwards. The edit is verified before the original is replaced.") : QString()),
                        QStringLiteral("Keep editing"), QStringLiteral("Apply"), deletes > 0, deletes > 0 ? Sheet::Icon::Warning : Sheet::Icon::Info))
        return Error(ErrorCode::Cancelled, QStringLiteral("Not applied."));
    auto r = jobs::submitImageEdit(m_ctx, m_image->path(), operationsJson(), options()->overrides());
    if (r.ok()) {
        m_ops.clear();
        rebuildStaged();
    }
    return r;
}

void ImageEditorPage::convert()
{
    const QString image = m_image->path();
    if (image.isEmpty())
        return;
    const QString fmt = m_convertFormat->currentValue().toString();
    QString out = m_convertOutput->path();
    if (out.isEmpty()) {
        const QFileInfo fi(image);
        out = fi.absolutePath() + u'/' + fi.completeBaseName() + jobs::outputExtension(fmt);
    }
    if (QFileInfo::exists(out)) {
        if (!Sheet::confirm(window(), QStringLiteral("Replace the existing image?"), QStringLiteral("%1 exists. It is set aside while the new image is written and restored if the conversion fails.").arg(Sheet::monoSpan(nativePath(out))),
                            QStringLiteral("Cancel"), QStringLiteral("Replace")))
            return;
    }
    auto r = jobs::submitConvert(m_ctx, image, fmt, out, options()->overrides());
    if (!r.ok()) {
        showFormError(QStringLiteral("Conversion not started"), r.error().message);
        return;
    }
    m_ctx.queue().holdQueue(false);
    emit toastRequested(Tone::Info, QStringLiteral("Conversion queued"), nativePath(out));
}

bool ImageEditorPage::acceptsDrop(const QList<QUrl>& urls) const
{
    return !urls.isEmpty() && !jobs::imageEditKind(urls.first().toLocalFile()).isEmpty();
}

void ImageEditorPage::dropUrls(const QList<QUrl>& urls)
{
    if (acceptsDrop(urls))
        open(urls.first().toLocalFile());
}

void ImageEditorPage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("image")))
        open(args.value(QStringLiteral("image")).toString());
}

void ImageEditorPage::refresh()
{
    if (!m_image->path().isEmpty())
        open(m_image->path());
}

QString ImageEditorPage::statusContext() const
{
    if (m_tree.nodes.empty())
        return {};
    return QStringLiteral("%1 · %2 staged").arg(QFileInfo(m_tree.image).fileName()).arg(m_ops.size());
}

} // namespace fw::pages
