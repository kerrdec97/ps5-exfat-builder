#include "app/pages/ImageToolsPage.h"

#include "app/AppContext.h"
#include "app/pages/CompareResultSheet.h"
#include "app/pages/HexPreviewSheet.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/Icons.h"
#include "app/ui/Motion.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "core/Tr.h"
#include "engines/gameinfo/Naming.h"
#include "engines/general/Module.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QHeaderView>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollBar>
#include <QTextDocument>
#include <QTimer>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

const QString kCompareKind = QStringLiteral("image.compare");
const QString kConvertKind = QStringLiteral("image.convert");
// A function, not a constant: the descriptions are translated when the dialog is set up.
QString imageFilter()
{
    return FW_TR("Images") + QStringLiteral(" (*.exfat *.ffpkg *.ffpfs *.ffpfsc);;") + FW_TR("FPKG packages") + QStringLiteral(" (*.pkg);;")
           + FW_TR("All files") + QStringLiteral(" (*)");
}

// Badge::setIconName sticks, so every state sets its glyph.
void setBadge(Badge* b, const QString& text, Tone tone, const QString& icon = {})
{
    b->set(text, tone);
    b->setIconName(icon.isEmpty() ? defaultBadgeIcon(tone) : icon);
}

bool isCompareImage(const QString& path) { return !imageFormatOf(path).isEmpty(); }

bool samePath(const QString& a, const QString& b)
{
    return QString::compare(QDir::cleanPath(QFileInfo(a).absoluteFilePath()), QDir::cleanPath(QFileInfo(b).absoluteFilePath()),
                            Qt::CaseInsensitive) == 0;
}

// "<folder of the image>/<its base name><suffix>": the default place for what
// is made from an image (also right for an image in a drive root).
QString nextToImage(const QString& image, const QString& suffix = {})
{
    const QFileInfo fi(image);
    return QDir(fi.absolutePath()).filePath(fi.completeBaseName() + suffix);
}

// Why `name` cannot be a file or folder name in an image of `format` ("" = it can).
QString imageNameProblem(const QString& name, const QString& format)
{
    if (name.isEmpty())
        return FW_TR("Enter a name.");
    if (name.contains(u'/'))
        return FW_TR("A name cannot contain \"/\". Create the folders one at a time.");
    if (name == QLatin1String(".") || name == QLatin1String(".."))
        return FW_TR("\".\" and \"..\" cannot be used as names.");
    // exFAT has the Windows rules; a UFS2 image (.ffpkg) takes any other name.
    return format == QLatin1String("ffpkg") ? QString() : gameinfo::folderNameProblem(name);
}

// The four actions as one row of tiles: icon square, title, one short line.
// Selection uses the signature gradient like ChoiceTiles; two columns when narrow.
class ActionBar : public QWidget {
public:
    struct Item {
        QString title, text, icon, tip;
        bool enabled = true;
    };
    std::function<void(int)> onChanged;

    explicit ActionBar(QWidget* parent) : QWidget(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setAccessibleName(FW_TR("Action"));
        m_hover = new motion::HoverFade(this, [this](int i) {
            if (i >= 0 && i < int(m_items.size()))
                update(tile(i));
        });
        m_glow = new SelectionGlow(this);
    }
    void add(const QString& title, const QString& text, const QString& icon)
    {
        m_items.push_back({title, text, icon, {}, true});
        m_hover->reset();
        updateGeometry();
        syncGlow();
    }
    void setEnabledAt(int i, bool on, const QString& tip = {})
    {
        m_items[size_t(i)].enabled = on;
        m_items[size_t(i)].tip = tip;
        update();
    }
    bool enabledAt(int i) const { return m_items[size_t(i)].enabled; }
    int current() const { return m_current; }
    void setCurrent(int i)
    {
        if (i == m_current || i < 0 || i >= int(m_items.size()))
            return;
        m_current = i;
        setAccessibleDescription(FW_TR("Selected: %1").arg(m_items[size_t(i)].title)); // the tiles are painted, not widgets
        update();
        syncGlow();
        if (onChanged)
            onChanged(i);
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override
    {
        const int rows = (int(m_items.size()) + columns(w) - 1) / columns(w);
        return rows * kH + (rows - 1) * kGap;
    }
    QSize sizeHint() const override { return {720, heightForWidth(720)}; }
    QSize minimumSizeHint() const override { return {300, kH}; }

protected:
    int columns(int w) const { return w >= int(m_items.size()) * 150 + (int(m_items.size()) - 1) * kGap ? int(m_items.size()) : 2; }
    QRect tile(int i) const
    {
        const int cols = columns(width());
        const int w = (width() - (cols - 1) * kGap) / cols;
        return mirrored(this, QRect((i % cols) * (w + kGap), (i / cols) * (kH + kGap), w, kH)); // RTL: first at the right
    }
    int at(const QPoint& p) const
    {
        for (int i = 0; i < int(m_items.size()); ++i)
            if (tile(i).contains(p))
                return i;
        return -1;
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal dpr = devicePixelRatioF();
        for (int i = 0; i < int(m_items.size()); ++i) {
            const Item& it = m_items[size_t(i)];
            const QRectF tileR = QRectF(tile(i)).adjusted(0.5, 0.5, -0.5, -0.5);
            // The inside is worked out left to right and mirrored as it is drawn.
            const QRectF r = mirrored(this, tileR);
            const auto vis = [this](const QRectF& x) { return mirrored(this, x); };
            const bool sel = i == m_current;
            const qreal hov = it.enabled && !sel ? m_hover->amount(i) : 0.0; // the hover tint fades
            if (sel) {
                p.setPen(Qt::NoPen);
                p.setBrush(color::bg_2);
                p.drawRoundedRect(tileR, radius::md, radius::md);
                p.setBrush(accentGradient(tileR, 40));
                p.drawRoundedRect(tileR, radius::md, radius::md);
                p.setPen(QPen(QBrush(accentGradient(tileR, 230)), 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(tileR.adjusted(0.5, 0.5, -0.5, -0.5), radius::md, radius::md);
            } else {
                // The tiles sit on the page, between two cards: plates like them.
                paintPlate(p, tileR, radius::md, hov);
            }
            const QRectF ir = vis(QRectF(r.left() + kPad, r.center().y() - 17, 34, 34));
            p.setPen(Qt::NoPen);
            p.setBrush(sel ? QBrush(accentFillGradient(ir)) : QBrush(color::bg_4)); // white only on the fill gradient
            p.drawRoundedRect(ir, 10, 10);
            const QColor glyph = sel ? color::on_accent : it.enabled ? color::fg_2 : color::fg_5;
            p.drawPixmap(QRectF(ir.center().x() - 9, ir.center().y() - 9, 18, 18).toRect(), iconPixmap(it.icon, 18, glyph, dpr));
            const qreal tx = r.left() + kPad + 34 + kPad;
            const qreal tw = r.right() - kPad - tx;
            p.setFont(font::bodyStrong());
            p.setPen(it.enabled ? color::fg_0 : color::fg_5);
            p.drawText(vis(QRectF(tx, r.center().y() - 19, tw, 20)), Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(font::bodyStrong()).elidedText(it.title, Qt::ElideRight, int(tw)));
            p.setFont(font::caption());
            p.setPen(it.enabled ? (sel ? color::fg_2 : color::fg_3) : color::fg_6);
            p.drawText(vis(QRectF(tx, r.center().y() + 1, tw, 18)), Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(font::caption()).elidedText(it.text, Qt::ElideRight, int(tw)));
            if (hasFocus() && sel) {
                p.setPen(QPen(color::focus, 2));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(tileR.adjusted(-3, -3, 3, 3), radius::md + 3, radius::md + 3);
            }
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        if (e->button() != Qt::LeftButton)
            return;
        const int i = at(e->position().toPoint());
        if (i >= 0 && m_items[size_t(i)].enabled) {
            setFocus(Qt::MouseFocusReason);
            setCurrent(i);
        }
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        const int i = at(e->position().toPoint());
        if (i != m_hover->hovered()) {
            m_hover->setHovered(i);
            // Why a tile cannot be chosen, or its second line when a narrow tile cuts it.
            QString tip = i >= 0 ? m_items[size_t(i)].tip : QString();
            if (i >= 0 && tip.isEmpty()
                && QFontMetrics(font::caption()).horizontalAdvance(m_items[size_t(i)].text) > tile(i).width() - 2 * kPad - 34 - kPad - 1)
                tip = m_items[size_t(i)].text;
            setToolTip(tip);
            setCursor(i >= 0 && m_items[size_t(i)].enabled ? Qt::PointingHandCursor : Qt::ArrowCursor);
        }
    }
    void leaveEvent(QEvent*) override { m_hover->setHovered(-1); }
    void resizeEvent(QResizeEvent* e) override
    {
        QWidget::resizeEvent(e);
        syncGlow();
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        // Left and right as they are on screen.
        const bool rtl = isRightToLeft();
        const int k = e->key();
        const int step = k == Qt::Key_Down || k == (rtl ? Qt::Key_Left : Qt::Key_Right) ? 1
                       : k == Qt::Key_Up || k == (rtl ? Qt::Key_Right : Qt::Key_Left) ? -1 : 0;
        if (!step) {
            QWidget::keyPressEvent(e);
            return;
        }
        for (int i = m_current + step; i >= 0 && i < int(m_items.size()); i += step)
            if (m_items[size_t(i)].enabled) {
                setCurrent(i);
                break;
            }
    }

private:
    // The selected tile's soft glow lies outside the tile: its own widget draws it.
    void syncGlow()
    {
        const bool any = m_current >= 0 && m_current < int(m_items.size());
        m_glow->setItemRect(any ? QRectF(tile(m_current)).adjusted(0.5, 0.5, -0.5, -0.5) : QRectF(), radius::md);
    }
    static constexpr int kH = 64;
    static constexpr int kGap = 10;
    static constexpr int kPad = 10; // around the icon square; 10 leaves the second line room at form width
    std::vector<Item> m_items;
    int m_current = 0;
    motion::HoverFade* m_hover; // which tile the pointer is over; the tint fades
    SelectionGlow* m_glow;
};

ActionBar* bar(QWidget* w) { return static_cast<ActionBar*>(w); }

// What the screenshot demo shows (CI evidence of the result sheet).
QJsonObject demoCompareDetails()
{
    const auto side = [](const QString& path, const QString& kind, qint64 files, qint64 dirs, qint64 bytes) {
        return QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("kind"), kind}, {QStringLiteral("files"), files},
                           {QStringLiteral("dirs"), dirs}, {QStringLiteral("bytes"), bytes}};
    };
    return QJsonObject{
        {QStringLiteral("a"), side(QStringLiteral("D:/Images/PPSA01325 Astro Bot (01.002).exfat"), QStringLiteral("exFAT image"), 18442, 1203, 48500000000LL)},
        {QStringLiteral("b"), side(QStringLiteral("D:/Dumps/PPSA01325-app"), QStringLiteral("folder"), 18443, 1204, 48500004096LL)},
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

const QString kDemoImage = QStringLiteral("D:/Images/PPSA01325 Astro Bot (01.002).exfat");
const QString kDemoSummary = QStringLiteral("6 differences: 1 only in A, 3 only in B, 1 different size, 1 with different content.");

} // namespace

QString StagedOp::describe() const
{
    const QString shown = QStringLiteral("/") + imagePath;
    if (op == QLatin1String("add"))
        return FW_TR("Add %1 → %2").arg(nativePath(localPath), shown);
    if (op == QLatin1String("replace"))
        return FW_TR("Replace %1 with %2").arg(shown, nativePath(localPath));
    if (op == QLatin1String("delete"))
        return FW_TR("Delete %1").arg(shown);
    if (op == QLatin1String("mkdir"))
        return FW_TR("New folder %1").arg(shown);
    if (op == QLatin1String("rename"))
        return FW_TR("Rename %1 → %2").arg(shown, newName);
    return op;
}

// ---- construction --------------------------------------------------------------------

ImageToolsPage::ImageToolsPage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent)
    : JobFormPage(ctx, queueModel, QStringLiteral("imagetools"), FW_TR("Image tools"),
                  FW_TR("Extract, verify, convert or edit an image you already have."),
                  FW_TR("Extract"), FW_TR("Image tools"), parent)
{
    setNewJobText(FW_TR("Back to the image"));
    m_info = new ImageSummaryReader(this);
    connect(m_info, &ImageSummaryReader::ready, this, &ImageToolsPage::onSummary);
    m_reader = new ImageTreeReader(this);
    connect(m_reader, &ImageTreeReader::ready, this, &ImageToolsPage::onTree);

    buildImageCard();

    auto* actions = new ActionBar(this);
    actions->add(FW_TR("Extract"), FW_TR("Copy the files out"), QStringLiteral("folder_out"));
    actions->add(FW_TR("Verify"), FW_TR("Check or compare"), QStringLiteral("shield_check"));
    actions->add(FW_TR("Convert"), FW_TR("Change the format"), QStringLiteral("convert"));
    actions->add(FW_TR("Edit"), FW_TR("Change its files"), QStringLiteral("file_edit"));
    actions->onChanged = [this](int i) { setAction(Action(i)); };
    m_actionsBar = actions;
    form()->addWidget(actions);

    buildExtractPanel();
    buildVerifyPanel();
    buildConvertPanel();
    buildEditPanel();

    addJobOptionsCard(FW_TR("Verification, threads"), {QStringLiteral("Build"), QStringLiteral("Verification"), QStringLiteral("Performance")}, {},
                      FW_TR("All job settings"));
    for (QWidget* w = options(); w; w = w->parentWidget())
        if ((m_advanced = qobject_cast<Disclosure*>(w)))
            break;

    // Comparisons run through the queue; the result row follows the latest one.
    connect(&m_ctx.queue(), &JobQueue::jobChanged, this, [this](const QString& id) { onCompareJob(id, false); });
    connect(&m_ctx.queue(), &JobQueue::jobFinished, this, [this](const QString& id) {
        onCompareJob(id, true);
        onEditJobFinished(id);
    });
    // The file dialogs start in the output folder of the moment, not the one at start-up.
    connect(&m_ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant& value) {
        if (key != general::keys::kDefaultOutputDir)
            return;
        m_imageField->setStartDir(value.toString());
        m_compareB->setStartDir(value.toString());
    });
    openImage({});
}

void ImageToolsPage::buildImageCard()
{
    m_imageCard = new Card(this);
    setSourceCard(m_imageCard);
    m_imageBadge = new Badge(FW_TR("Step 1"), Tone::Neutral, m_imageCard);
    m_imageBadge->setIconName(QString());
    m_imageCard->setTitle(FW_TR("Image"), m_imageBadge);
    m_imageField = new PathField(PathField::Mode::OpenFile, m_imageCard);
    m_imageField->setFilter(imageFilter());
    m_imageField->setDialogTitle(FW_TR("Open an image"));
    m_imageField->setStartDir(m_ctx.settings().value(general::keys::kDefaultOutputDir).toString());
    m_imageField->hide(); // the model behind the drop zone and Change…
    m_imageCard->body()->addWidget(m_imageField);
    connect(m_imageField, &PathField::pathChanged, this, [this](const QString& p) {
        if (p != m_path)
            requestImage(p);
    });

    m_drop = new DropZone(QStringLiteral("drive"), FW_TR("Open an image"),
                          FW_TR("Drop an .exfat, .ffpkg, .ffpfs or .ffpfsc file here, or click to browse"), m_imageCard);
    m_drop->setToolTip(FW_TR("FPKG packages (.pkg) can be opened too, to extract or verify them."));
    connect(m_drop, &DropZone::clicked, m_imageField->browseButton(), &QPushButton::click);
    m_imageCard->body()->addWidget(m_drop);

    m_strip = new Card(m_imageCard, Card::Variant::Inset);
    m_strip->body()->setContentsMargins(2, 4, 2, 4);
    auto* sh = new QHBoxLayout;
    sh->setSpacing(space::md);
    m_stripTile = new IconTile(QStringLiteral("drive"), 48, m_strip);
    sh->addWidget(m_stripTile);
    auto* col = new QVBoxLayout;
    col->setSpacing(3);
    // Title, IDs and path are read from the image and its file name: shown literally.
    m_stripTitle = new ElideLabel({}, Qt::ElideRight, m_strip);
    m_stripTitle->setFont(font::subtitle());
    setTextColor(m_stripTitle, color::fg_0);
    m_stripMeta = new ElideLabel({}, Qt::ElideRight, m_strip);
    m_stripMeta->setFont(font::caption());
    setTextColor(m_stripMeta, color::fg_3);
    m_stripPath = new ElideLabel({}, Qt::ElideMiddle, m_strip);
    m_stripPath->setFont(font::monoSmall());
    keepLeftToRight(m_stripPath);
    setTextColor(m_stripPath, color::fg_4);
    col->addWidget(m_stripTitle);
    col->addWidget(m_stripMeta);
    col->addWidget(m_stripPath);
    sh->addLayout(col, 1);
    auto* show = new IconButton(QStringLiteral("folder_open"), FW_TR("Show in folder"), IconButton::Size::Regular, m_strip);
    show->setToolTip(FW_TR("Show in folder"));
    connect(show, &QToolButton::clicked, this, [this] { openInFileManager(m_path); });
    sh->addWidget(show);
    auto* change = button(FW_TR("Change…"), ButtonVariant::Secondary, QStringLiteral("folder"), m_strip);
    connect(change, &QPushButton::clicked, m_imageField->browseButton(), &QPushButton::click);
    sh->addWidget(change);
    auto* close = new IconButton(QStringLiteral("close"), FW_TR("Close the image"), IconButton::Size::Regular, m_strip);
    close->setToolTip(FW_TR("Close the image"));
    connect(close, &QToolButton::clicked, this, [this] { requestImage({}); });
    sh->addWidget(close);
    m_strip->body()->addLayout(sh);
    m_strip->hide();
    m_imageCard->body()->addWidget(m_strip);
    m_imageMsg = new MessageRow(m_imageCard);
    m_imageCard->body()->addWidget(m_imageMsg);
    form()->addWidget(m_imageCard);
}

void ImageToolsPage::buildExtractPanel()
{
    auto* card = new Card(this);
    card->setTitle(FW_TR("Extract to"));
    m_destination = new PathField(PathField::Mode::Directory, card);
    m_destination->setDialogTitle(FW_TR("Choose where to extract"));
    m_destination->setPlaceholder(FW_TR("A new folder next to the image"));
    m_destination->setToolTip(FW_TR("The folder is created if it does not exist. Existing files are never overwritten."));
    card->body()->addWidget(m_destination);
    m_extractNote = caption(FW_TR("Existing files are never overwritten."), card);
    card->body()->addWidget(m_extractNote);
    // A game whose files are in zstd or LZ4 packs: extract them as plain files.
    m_unpackRow = new QWidget(card);
    auto* uh = hbox(m_unpackRow, space::lg, QMargins(0, space::sm, 0, 0));
    auto* ul = new QWidget(m_unpackRow);
    auto* uv = vbox(ul, 2);
    uv->addWidget(label(FW_TR("Unpack the zstd and LZ4 packs"), TextStyle::Body, color::fg_1, ul));
    uv->addWidget(caption(FW_TR("On: the game's own files come out of the packs (and ampr_emu.index is rewritten). "
                                         "Off: the pack files are copied as they are."), ul));
    uh->addWidget(ul, 1);
    m_unpackPacks = new Switch(m_unpackRow);
    m_unpackPacks->setChecked(true);
    m_unpackPacks->setAccessibleName(FW_TR("Unpack the zstd and LZ4 packs"));
    uh->addWidget(m_unpackPacks, 0, Qt::AlignTop);
    m_unpackRow->setVisible(false);
    card->body()->addWidget(m_unpackRow);
    m_extractPanel = card;
    form()->addWidget(card);
}

void ImageToolsPage::buildVerifyPanel()
{
    auto* card = new Card(this);
    card->setTitle(FW_TR("Check against"));
    m_verifyMode = new Segmented(card);
    m_verifyMode->addSegment(FW_TR("Its own checksums"), QStringLiteral("shield_check"), QStringLiteral("self"));
    m_verifyMode->addSegment(FW_TR("A folder or another image"), QStringLiteral("files"), QStringLiteral("compare"));
    m_verifyMode->setAccessibleName(FW_TR("Check against"));
    m_verifyMode->setCurrentIndex(0);
    card->body()->addWidget(m_verifyMode, 0, Qt::AlignLeft);
    m_verifyHelp = caption({}, card);
    card->body()->addWidget(m_verifyHelp);

    m_compareBox = new QWidget(card);
    auto* cl = vbox(m_compareBox, space::md);
    auto* bHead = new QWidget(m_compareBox);
    auto* bh = hbox(bHead, space::sm);
    bh->addWidget(label(FW_TR("Compare with"), TextStyle::Body, color::fg_1, bHead));
    bh->addStretch(1);
    m_compareBKind = new Segmented(bHead);
    m_compareBKind->addSegment(FW_TR("Folder"), QStringLiteral("folder"), QStringLiteral("folder"));
    m_compareBKind->addSegment(FW_TR("Image"), QStringLiteral("drive"), QStringLiteral("image"));
    m_compareBKind->setAccessibleName(FW_TR("B is an image or a folder"));
    m_compareBKind->setCurrentIndex(0);
    bh->addWidget(m_compareBKind);
    cl->addWidget(bHead);
    m_compareB = new PathField(PathField::Mode::Directory, m_compareBox);
    m_compareB->setStartDir(m_ctx.settings().value(general::keys::kDefaultOutputDir).toString());
    m_compareB->setAccessibleName(FW_TR("Image or folder B"));
    cl->addWidget(m_compareB);
    m_compareDepth = new Segmented(m_compareBox);
    m_compareDepth->addSegment(FW_TR("Names, sizes and contents"), QStringLiteral("file_search"), QStringLiteral("full"));
    m_compareDepth->addSegment(FW_TR("Names and sizes"), QStringLiteral("list"), QStringLiteral("structure"));
    m_compareDepth->setAccessibleName(FW_TR("Depth"));
    m_compareDepth->setCurrentIndex(0);
    m_compareDepth->setToolTip(FW_TR("Comparing contents reads every file on both sides (as long as a full verify); names and sizes take seconds."));
    cl->addWidget(field(FW_TR("Depth"), m_compareDepth, {}, m_compareBox));

    m_compareResultRow = new Card(m_compareBox, Card::Variant::Inset);
    auto* rh = new QHBoxLayout;
    rh->setSpacing(space::md);
    m_compareBadge = new Badge(FW_TR("Not compared"), Tone::Neutral, m_compareResultRow);
    rh->addWidget(m_compareBadge, 0, Qt::AlignTop);
    m_compareSummary = caption({}, m_compareResultRow);
    m_compareSummary->setWordWrap(true);
    rh->addWidget(m_compareSummary, 1);
    m_compareView = button(FW_TR("View result"), ButtonVariant::Secondary, QStringLiteral("list"), m_compareResultRow);
    m_compareView->hide();
    rh->addWidget(m_compareView, 0, Qt::AlignVCenter);
    m_compareResultRow->body()->addLayout(rh);
    cl->addWidget(m_compareResultRow);
    card->body()->addWidget(m_compareBox);
    m_verifyPanel = card;
    form()->addWidget(card);

    connect(m_verifyMode, &Segmented::currentChanged, this, [this](int) { updateActions(); });
    connect(m_compareBKind, &Segmented::currentChanged, this, [this](int) { updateCompareKind(); });
    connect(m_compareB, &PathField::pathChanged, this, [this](const QString& p) {
        m_compareB->setError({});
        // A dropped or typed folder switches B to folder mode (and back).
        const QFileInfo fi(p);
        if (fi.isDir() && m_compareBKind->currentValue().toString() != QLatin1String("folder"))
            m_compareBKind->setCurrentValue(QStringLiteral("folder"));
        else if (fi.isFile() && m_compareBKind->currentValue().toString() != QLatin1String("image"))
            m_compareBKind->setCurrentValue(QStringLiteral("image"));
    });
    connect(m_compareView, &QPushButton::clicked, this, [this] { (void)showCompareResult(); });
    updateCompareKind();
}

void ImageToolsPage::buildConvertPanel()
{
    auto* card = new Card(this);
    card->setTitle(FW_TR("Convert to"));
    m_convertTilesHost = new QWidget(card);
    vbox(m_convertTilesHost, 0);
    card->body()->addWidget(m_convertTilesHost);
    m_methodRow = new QWidget(card);
    auto* ml = vbox(m_methodRow, space::xs);
    m_method = new Segmented(m_methodRow);
    m_method->addSegment(FW_TR("Pack the image as it is"), QStringLiteral("zap"), QStringLiteral("direct"));
    m_method->addSegment(FW_TR("Repack the files"), QStringLiteral("files"), QStringLiteral("repack"));
    m_method->setAccessibleName(FW_TR("Method"));
    m_method->setCurrentIndex(0);
    m_methodHelp = caption({}, m_methodRow);
    ml->addWidget(label(FW_TR("Method"), TextStyle::Body, color::fg_1, m_methodRow));
    ml->addWidget(m_method, 0, Qt::AlignLeft);
    ml->addWidget(m_methodHelp);
    card->body()->addWidget(m_methodRow);
    m_convertOutput = new PathField(PathField::Mode::SaveFile, card);
    m_convertOutput->setDialogTitle(FW_TR("Save the converted image as"));
    m_convertOutput->setToolTip(FW_TR("The new image is written and verified first; the original stays."));
    card->body()->addWidget(field(FW_TR("Save as"), m_convertOutput, {}, card));
    m_convertPanel = card;
    form()->addWidget(card);
    connect(m_method, &Segmented::currentChanged, this, [this](int) { updateConvert(); });
    connect(m_convertOutput, &PathField::pathChanged, this, [this](const QString&) { m_convertOutput->setError({}); });
}

void ImageToolsPage::buildEditPanel()
{
    m_editPanel = new QWidget(this);
    auto* el = vbox(m_editPanel, space::lg);

    auto* treeCard = new Card(m_editPanel, Card::Variant::Table);
    auto* h = treeCard->header();
    h->addWidget(heading(FW_TR("Contents"), TextStyle::Subtitle, treeCard));
    m_treeCount = caption({}, treeCard);
    h->addWidget(m_treeCount);
    h->addStretch(1);
    m_btnAddFiles = button(FW_TR("Add files…"), ButtonVariant::Secondary, QStringLiteral("file_plus"), treeCard);
    m_btnAddFolder = button(FW_TR("Add folder…"), ButtonVariant::Secondary, QStringLiteral("folder_plus"), treeCard);
    h->addWidget(m_btnAddFiles);
    h->addWidget(m_btnAddFolder);
    h->addSpacing(space::xs);
    m_btnMkdir = new IconButton(QStringLiteral("folder_plus"), FW_TR("New folder"), IconButton::Size::Regular, treeCard);
    m_btnReplace = new IconButton(QStringLiteral("replace"), FW_TR("Replace…"), IconButton::Size::Regular, treeCard);
    m_btnRename = new IconButton(QStringLiteral("pencil"), FW_TR("Rename"), IconButton::Size::Regular, treeCard);
    m_btnDelete = new IconButton(QStringLiteral("trash"), FW_TR("Delete"), IconButton::Size::Regular, treeCard);
    m_btnMkdir->setToolTip(FW_TR("New folder"));
    m_btnReplace->setToolTip(FW_TR("Replace the selected file…"));
    m_btnRename->setToolTip(FW_TR("Rename (FFPKG images)"));
    m_btnDelete->setToolTip(FW_TR("Delete the selection"));
    for (auto* b : {m_btnMkdir, m_btnReplace, m_btnRename, m_btnDelete})
        h->addWidget(b);
    connect(m_btnAddFiles, &QPushButton::clicked, this, [this] { addFiles(false); });
    connect(m_btnAddFolder, &QPushButton::clicked, this, [this] { addFiles(true); });
    connect(m_btnReplace, &QToolButton::clicked, this, &ImageToolsPage::replaceSelected);
    connect(m_btnMkdir, &QToolButton::clicked, this, &ImageToolsPage::newFolder);
    connect(m_btnRename, &QToolButton::clicked, this, &ImageToolsPage::renameSelected);
    connect(m_btnDelete, &QToolButton::clicked, this, &ImageToolsPage::deleteSelected);

    m_treeView = new QTreeWidget(treeCard);
    m_treeView->setHeaderLabels({FW_TR("Name"), FW_TR("Size"), FW_TR("Modified")});
    m_treeView->header()->setFixedHeight(32);
    m_treeView->header()->setStretchLastSection(false);
    m_treeView->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_treeView->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_treeView->setColumnWidth(1, 110);
    m_treeView->setColumnWidth(2, 150);
    keepColumnLeftToRight(m_treeView, 1); // sizes, dates, paths, IDs: left to right in any language
    keepColumnLeftToRight(m_treeView, 2);
    m_treeView->setUniformRowHeights(true);
    m_treeView->setMinimumHeight(11 * size::rowCompact + 40);
    m_treeView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_treeView->setIndentation(20);
    m_treeView->setFont(font::mono());
    m_treeView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_treeView->setToolTip(FW_TR("Double-click a file to preview its bytes · right-click for more"));
    connect(m_treeView, &QTreeWidget::customContextMenuRequested, this, &ImageToolsPage::showTreeMenu);
    connect(m_treeView, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* it, int) {
        if (it && !it->data(0, Qt::UserRole + 1).toBool())
            (void)preview(it->data(0, Qt::UserRole).toString());
    });
    connect(m_treeView, &QTreeWidget::itemSelectionChanged, this, &ImageToolsPage::updateTreeButtons);
    treeCard->body()->addWidget(m_treeView);
    m_treeEmpty = new EmptyState(QStringLiteral("folder_tree"), FW_TR("Reading the image…"), FW_TR("Its folders and files appear here."), treeCard);
    m_treeEmpty->setCompact(true);
    treeCard->body()->addWidget(m_treeEmpty);
    m_treeError = new ErrorState(FW_TR("This image cannot be edited"), {}, {}, treeCard);
    m_treeError->hide();
    treeCard->body()->addWidget(m_treeError);
    el->addWidget(treeCard);

    m_stagedCard = new Card(m_editPanel);
    m_clear = button(FW_TR("Clear"), ButtonVariant::Subtle, QStringLiteral("close"), m_stagedCard);
    setButtonSmall(m_clear, true);
    m_stagedCard->setTitle(FW_TR("Staged changes"), m_clear);
    m_stagedCard->body()->setSpacing(0);
    m_stagedCaption = caption(FW_TR("Nothing staged yet. Changes are applied together, in one job."), m_stagedCard);
    m_stagedCard->body()->addWidget(m_stagedCaption);
    auto* stagedList = new QWidget(m_stagedCard);
    m_stagedLayout = vbox(stagedList, 0);
    m_stagedCard->body()->addWidget(stagedList);
    connect(m_clear, &QPushButton::clicked, this, &ImageToolsPage::clearStaged);
    el->addWidget(m_stagedCard);
    form()->addWidget(m_editPanel);
    rebuildTree();
    rebuildStaged();
}

// ---- the image -------------------------------------------------------------------------

void ImageToolsPage::openImage(const QString& path)
{
    clearFormError();
    loadImage(path);
}

// The user asks for another image (or to close this one). The staged changes
// are the only unsaved work on this page: they are not dropped without a question.
void ImageToolsPage::requestImage(const QString& path)
{
    if (path != m_path && !m_ops.empty() && !m_demo) {
        const QString title = m_ops.size() == 1 ? FW_TR("Discard %1 staged change?").arg(m_ops.size())
                                                : FW_TR("Discard %1 staged changes?").arg(m_ops.size());
        if (!Sheet::confirm(window(), title,
                            FW_TR("They have not been applied to %1.").arg(Sheet::monoSpan(QFileInfo(m_path).fileName())),
                            FW_TR("Keep editing"), FW_TR("Discard"))) {
            const QSignalBlocker block(m_imageField);
            m_imageField->setPath(m_path);
            return;
        }
    }
    openImage(path);
}

void ImageToolsPage::onEditJobFinished(const QString& id)
{
    const auto rec = m_ctx.queue().record(id);
    if (!rec || m_demo || m_path.isEmpty() || !rec->kind.endsWith(QLatin1String(".edit")))
        return;
    if (!samePath(rec->parameters.value(QStringLiteral("image")).toString(), m_path))
        return;
    // The file is the edited image now: the strip and the tree are read again.
    // Changes staged since the job was started stay.
    loadImage(m_path);
}

void ImageToolsPage::loadImage(const QString& path)
{
    const bool same = path == m_path && !path.isEmpty();
    m_path = path;
    if (m_imageField->path() != path) {
        const QSignalBlocker block(m_imageField);
        m_imageField->setPath(path);
    }
    // Reading the same image again keeps the staged changes, like the
    // destination and the comparison below.
    if (!same)
        m_ops.clear();
    m_tree = {};
    m_treeFor.clear();
    m_reader->cancel(); // a listing of the image shown before is abandoned
    m_summary = {};
    m_summary.path = path;
    m_summary.format = imageFormatOf(path, true);
    m_imageMsg->clear();
    if (!same) {
        m_destination->setPath({});
        m_convertOutput->setPath({});
        m_compareB->setPath({});
        m_compareB->setError({});
        // The last comparison was about the previous image.
        m_compareJobId.clear();
        m_compareResult = {};
    }
    const QFileInfo fi(path);
    if (path.isEmpty()) {
        m_imageBadge->set(FW_TR("Step 1"), Tone::Neutral);
        m_imageBadge->setIconName(QString());
    } else if (!fi.isFile()) {
        setBadge(m_imageBadge, FW_TR("Not found"), Tone::Err);
        m_imageMsg->set(FW_TR("This file does not exist. Choose another or check the drive."), Tone::Err);
    } else if (m_summary.format.isEmpty()) {
        setBadge(m_imageBadge, FW_TR("Unknown format"), Tone::Warn);
        m_imageMsg->set(FW_TR("Choose an .exfat, .ffpkg, .ffpfs, .ffpfsc or .pkg file."), Tone::Warn);
    } else {
        m_summary.exists = true;
        m_summary.bytes = fi.size();
        guessFromFileName(m_summary);
        setBadge(m_imageBadge, FW_TR("Reading…"), Tone::Info, QStringLiteral("loader"));
        m_info->read(path);
    }
    showStrip();
    rebuildTree();
    rebuildStaged();
    updateActions();
    ensureTree();
    emit statusContextChanged();
}

void ImageToolsPage::onSummary(const ImageSummary& s)
{
    if (s.path != m_path || m_demo)
        return;
    m_summary = s;
    if (!s.exists)
        setBadge(m_imageBadge, FW_TR("Not found"), Tone::Err);
    else if (!s.error.isEmpty() && s.format != u"pkg")
        setBadge(m_imageBadge, FW_TR("Not readable"), Tone::Warn);
    else
        setBadge(m_imageBadge, FW_TR("Ready"), Tone::Ok);
    if (!s.error.isEmpty() && s.exists && s.format != u"pkg")
        m_imageMsg->set(s.error, Tone::Warn);
    showStrip();
    // What is inside decides the extract job, and a file that is gone closes the panels.
    updateActions();
    emit statusContextChanged();
}

void ImageToolsPage::showStrip()
{
    const bool open = !m_path.isEmpty() && m_summary.exists && !m_summary.format.isEmpty();
    m_drop->setVisible(!open);
    m_strip->setVisible(open);
    if (!open)
        return;
    const ImageSummary& s = m_summary;
    m_stripTile->setIconName(imageFormatIcon(s.format));
    m_stripTile->setTone(!s.error.isEmpty() && s.format != u"pkg" ? Tone::Warn : Tone::Ok);
    m_stripTitle->setFullText(s.title.isEmpty() ? QFileInfo(s.path).fileName() : s.title);
    QString meta = s.metaLine();
    if (!s.inner.isEmpty())
        meta += QStringLiteral(" · ") + FW_TR("holds %1").arg(s.inner);
    if (!s.packs.isEmpty())
        meta += QStringLiteral(" · ") + FW_TR("%1-packed").arg(s.packs);
    if (m_unpackRow)
        m_unpackRow->setVisible(!s.packs.isEmpty());
    m_stripMeta->setFullText(meta);
    m_stripPath->setFullText(nativePath(s.path));
    m_stripPath->setToolTip(nativePath(s.path));
}

bool ImageToolsPage::isActionEnabled(Action a) const
{
    const QString f = m_summary.format;
    if (m_path.isEmpty() || f.isEmpty())
        return false;
    switch (a) {
    case Action::Extract:
    case Action::Verify: return true;
    case Action::Convert: return f != u"pkg";
    case Action::Edit: return f == u"exfat" || f == u"ffpkg";
    }
    return false;
}

void ImageToolsPage::setAction(Action a)
{
    if (!m_path.isEmpty() && !isActionEnabled(a))
        return;
    m_action = a;
    if (bar(m_actionsBar)->current() != int(a)) {
        auto keep = bar(m_actionsBar)->onChanged;
        bar(m_actionsBar)->onChanged = nullptr;
        bar(m_actionsBar)->setCurrent(int(a));
        bar(m_actionsBar)->onChanged = keep;
    }
    clearFormError();
    updateActions();
    ensureTree();
    emit statusContextChanged();
}

void ImageToolsPage::updateActions()
{
    const bool open = !m_path.isEmpty() && m_summary.exists && !m_summary.format.isEmpty();
    auto* ab = bar(m_actionsBar);
    const QString f = m_summary.format;
    ab->setEnabledAt(int(Action::Extract), open);
    ab->setEnabledAt(int(Action::Verify), open);
    ab->setEnabledAt(int(Action::Convert), open && f != u"pkg", f == u"pkg" ? FW_TR("FPKG packages cannot be converted here.") : QString());
    ab->setEnabledAt(int(Action::Edit), open && (f == u"exfat" || f == u"ffpkg"),
                     f == u"ffpfs" || f == u"ffpfsc" ? FW_TR("PFS images cannot be edited. Convert to exFAT or FFPKG first.")
                     : f == u"pkg" ? FW_TR("FPKG packages cannot be edited.") : QString());
    if (open && !isActionEnabled(m_action))
        m_action = Action::Extract;
    if (ab->current() != int(m_action)) {
        auto keep = ab->onChanged;
        ab->onChanged = nullptr;
        ab->setCurrent(int(m_action));
        ab->onChanged = keep;
    }
    m_actionsBar->setVisible(open);
    m_extractPanel->setVisible(open && m_action == Action::Extract);
    m_verifyPanel->setVisible(open && m_action == Action::Verify);
    m_convertPanel->setVisible(open && m_action == Action::Convert);
    m_editPanel->setVisible(open && m_action == Action::Edit);

    // Verify: comparing needs a readable image (not a .pkg).
    m_verifyMode->setSegmentEnabled(1, f != u"pkg");
    if (f == u"pkg" && compareMode())
        m_verifyMode->setCurrentIndex(0);
    const bool cmp = compareMode();
    m_compareBox->setVisible(cmp);
    m_verifyHelp->setText(cmp ? QString() : FW_TR("Reads the whole image and checks every checksum. Nothing is written."));
    m_verifyHelp->setVisible(!cmp);

    if (open)
        m_destination->setPlaceholder(nativePath(nextToImage(m_path)));
    // The exFAT extract job takes a new or empty folder only; the others add to a folder.
    const bool emptyOnly = f == u"exfat" && !usesGameExtract();
    m_extractNote->setText(emptyOnly ? FW_TR("The folder must be new or empty.") : FW_TR("Existing files are never overwritten."));
    m_destination->setToolTip(emptyOnly ? FW_TR("The folder is created if it does not exist. A folder that has files in it is refused.")
                                        : FW_TR("The folder is created if it does not exist. Existing files are never overwritten."));
    queueButton()->setVisible(!(m_action == Action::Edit || (m_action == Action::Verify && cmp)) || !open);
    // A comparison's result is the row in this card (and its sheet): keep the form.
    setMonitorOnStart(!(m_action == Action::Verify && cmp));
    rebuildConvertChoices();
    updateConvert();
    updateAdvanced();
    updatePrimary();
    updateKinds();
    showCompareState();
}

void ImageToolsPage::updatePrimary()
{
    QString text = FW_TR("Extract");
    QString icon = QStringLiteral("play");
    switch (m_action) {
    case Action::Extract: text = FW_TR("Extract"); break;
    case Action::Verify: text = compareMode() ? FW_TR("Compare") : FW_TR("Verify"); break;
    case Action::Convert: text = FW_TR("Convert"); break;
    case Action::Edit:
        text = m_ops.empty() ? FW_TR("Apply changes")
                             : m_ops.size() == 1 ? FW_TR("Apply %1 change").arg(m_ops.size()) : FW_TR("Apply %1 changes").arg(m_ops.size());
        break;
    }
    primaryButton()->setText(text);
    // What is known to be missing, in validateForm()'s words.
    if (m_path.isEmpty())
        setStartHint(FW_TR("Open an image first."));
    else if (m_action == Action::Edit && m_ops.empty() && m_tree.error.isEmpty())
        setStartHint(FW_TR("Stage at least one change."));
    else
        setStartHint({});
}

void ImageToolsPage::updateKinds()
{
    if (m_path.isEmpty() || m_summary.format.isEmpty()) {
        setRequiredKinds({});
        return;
    }
    QString kind;
    switch (m_action) {
    case Action::Extract: kind = usesGameExtract() ? QStringLiteral("game.extract") : jobs::imageKindForFile(m_path, QStringLiteral("extract")); break;
    case Action::Verify: kind = compareMode() ? kCompareKind : jobs::imageKindForFile(m_path, QStringLiteral("verify")); break;
    case Action::Convert: kind = kConvertKind; break;
    case Action::Edit: kind = jobs::imageEditKind(m_path); break;
    }
    setRequiredKinds(kind.isEmpty() ? QStringList{} : QStringList{kind});
}

void ImageToolsPage::updateAdvanced()
{
    if (!m_advanced)
        return;
    const bool open = !m_path.isEmpty() && m_summary.exists;
    // Build settings matter when an image is written (convert, edit).
    const bool writes = m_action == Action::Convert || m_action == Action::Edit;
    for (const QString& key : options()->keys()) {
        const SettingDef* d = m_ctx.schema().find(key);
        if (auto* row = options()->row(key))
            row->setVisible(writes || !d || d->section != QLatin1String("Build"));
    }
    refreshOptionsSummary();
    m_advanced->setCaption(writes ? FW_TR("Cluster size, verification, threads") : FW_TR("Verification, threads"));
    m_advanced->setVisible(open && !(m_action == Action::Verify && compareMode()));
}

void ImageToolsPage::ensureTree()
{
    if (m_demo || m_action != Action::Edit || m_path.isEmpty() || !isActionEnabled(Action::Edit) || m_treeFor == m_path)
        return;
    m_treeFor = m_path;
    m_tree = {};
    rebuildTree();
    m_reader->read(m_path);
}

// Packed games and images inside a PFS container are extracted through the
// format-neutral reader (game.extract); everything else by its own engine.
bool ImageToolsPage::usesGameExtract() const
{
    return !m_summary.packs.isEmpty() || (!m_summary.inner.isEmpty() && m_summary.fromImage);
}

// ---- Extract / Verify -------------------------------------------------------------------

void ImageToolsPage::setExtractDestination(const QString& folder) { m_destination->setPath(folder); }

bool ImageToolsPage::verifyOnly() const { return m_action == Action::Verify && !compareMode(); }

bool ImageToolsPage::compareMode() const { return m_verifyMode->currentValue().toString() == QLatin1String("compare"); }

// ---- Compare ----------------------------------------------------------------------------

void ImageToolsPage::updateCompareKind()
{
    const bool folder = m_compareBKind->currentValue().toString() == QLatin1String("folder");
    m_compareB->setMode(folder ? PathField::Mode::Directory : PathField::Mode::OpenFile);
    m_compareB->setFilter(FW_TR("Images") + QStringLiteral(" (*.exfat *.ffpkg *.ffpfs *.ffpfsc);;") + FW_TR("All files") + QStringLiteral(" (*)"));
    m_compareB->setDialogTitle(folder ? FW_TR("Choose the folder to compare with") : FW_TR("Choose the image to compare with"));
    m_compareB->setPlaceholder(folder ? FW_TR("A game folder, e.g. the dump the image was built from") : FW_TR(".exfat, .ffpkg, .ffpfs or .ffpfsc"));
    m_compareB->setError({});
}

void ImageToolsPage::setCompareInputs(const QString& a, const QString& b, const QString& depth)
{
    if (a != m_path)
        openImage(a);
    if (!a.isEmpty())
        setAction(Action::Verify);
    m_verifyMode->setCurrentValue(QStringLiteral("compare"));
    m_compareBKind->setCurrentValue(QFileInfo(b).isDir() ? QStringLiteral("folder") : QStringLiteral("image"));
    m_compareB->setPath(b);
    m_compareDepth->setCurrentValue(depth == QLatin1String("structure") ? QStringLiteral("structure") : QStringLiteral("full"));
    updateActions();
}

void ImageToolsPage::startCompare()
{
    if (!m_path.isEmpty() && isActionEnabled(Action::Verify) && m_action != Action::Verify)
        setAction(Action::Verify);
    m_verifyMode->setCurrentValue(QStringLiteral("compare"));
    showForm(); // an explicit request: the comparison's row lives in the form
    primaryAction();
}

bool ImageToolsPage::checkCompareInputs(QString* problem)
{
    const QString b = m_compareB->path();
    const bool bFolder = m_compareBKind->currentValue().toString() == QLatin1String("folder");
    const QFileInfo fa(m_path), fb(b);
    QString msg;
    if (!isCompareImage(m_path))
        msg = FW_TR("Only .exfat, .ffpkg, .ffpfs and .ffpfsc images can be compared.");
    else if (b.isEmpty())
        msg = bFolder ? FW_TR("Choose the folder to compare with.") : FW_TR("Choose the image to compare with.");
    else if (bFolder ? !fb.isDir() : !fb.isFile())
        msg = bFolder ? FW_TR("This folder does not exist.") : FW_TR("This file does not exist.");
    else if (!bFolder && !isCompareImage(b))
        msg = FW_TR("Choose an .exfat, .ffpkg, .ffpfs or .ffpfsc image, or switch to Folder.");
    else if (QDir::cleanPath(fa.absoluteFilePath()) == QDir::cleanPath(fb.absoluteFilePath()))
        msg = FW_TR("Choose a different image than the one that is open.");
    m_compareB->setError(msg);
    if (problem)
        *problem = msg;
    return msg.isEmpty();
}

void ImageToolsPage::onCompareJob(const QString& id, bool finished)
{
    if (id.isEmpty())
        return;
    const auto rec = m_ctx.queue().record(id);
    if (!rec || rec->kind != kCompareKind)
        return;
    if (id != m_compareJobId) {
        // A comparison of the open image started elsewhere (Queue › Retry): the
        // row follows the latest one. Another image's comparison is not its business.
        if (!finished && rec->state != JobState::Queued && rec->state != JobState::Running)
            return;
        if (m_path.isEmpty() || !samePath(rec->parameters.value(QStringLiteral("a")).toString(), m_path))
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

void ImageToolsPage::showCompareState()
{
    if (m_compareDemo)
        return;
    const auto rec = m_compareJobId.isEmpty() ? std::nullopt : m_ctx.queue().record(m_compareJobId);
    const bool pending = rec && !isTerminal(rec->state);
    m_compareResultRow->setVisible(pending || m_compareResult.valid);
    if (pending) {
        if (rec->state == JobState::Running)
            setBadge(m_compareBadge, FW_TR("Comparing…"), Tone::Info, QStringLiteral("loader"));
        else
            setBadge(m_compareBadge, rec->state == JobState::Paused ? FW_TR("Paused")
                                     : rec->state == JobState::Cancelling ? FW_TR("Cancelling…") : FW_TR("Queued"),
                     Tone::Neutral);
        m_compareSummary->setText(FW_TR("%1 · follow it in the Jobs dock.").arg(rec->title));
        m_compareView->hide();
        return;
    }
    const CompareResult& r = m_compareResult;
    const bool hasDetails = r.valid && r.state == JobState::Succeeded && r.details.contains(QStringLiteral("counts"));
    m_compareView->setVisible(hasDetails);
    if (!r.valid) {
        setBadge(m_compareBadge, FW_TR("Not compared"), Tone::Neutral);
        m_compareSummary->setText(jobs::isAvailable(m_ctx, kCompareKind) ? QString() : FW_TR("Comparing images is not available in this build."));
        return;
    }
    switch (r.state) {
    case JobState::Succeeded: {
        if (!hasDetails) {
            setBadge(m_compareBadge, FW_TR("Done"), Tone::Neutral);
            m_compareSummary->setText(r.summary);
            break;
        }
        const qint64 n = r.details.value(QStringLiteral("counts")).toObject().value(QStringLiteral("differences")).toInteger();
        if (n == 0)
            setBadge(m_compareBadge, FW_TR("Identical"), Tone::Ok);
        else
            setBadge(m_compareBadge, n == 1 ? FW_TR("%1 difference").arg(formatCount(n)) : FW_TR("%1 differences").arg(formatCount(n)), Tone::Warn);
        const QString a = QFileInfo(r.details.value(QStringLiteral("a")).toObject().value(QStringLiteral("path")).toString()).fileName();
        const QString b = QFileInfo(r.details.value(QStringLiteral("b")).toObject().value(QStringLiteral("path")).toString()).fileName();
        m_compareSummary->setText(a.isEmpty() ? r.summary : QStringLiteral("%1 ↔ %2\n%3").arg(a, b, r.summary));
        break;
    }
    case JobState::Failed:
        setBadge(m_compareBadge, FW_TR("Failed"), Tone::Err);
        m_compareSummary->setText(r.error);
        break;
    default:
        setBadge(m_compareBadge, r.state == JobState::Cancelled ? FW_TR("Cancelled") : FW_TR("Interrupted"), Tone::Neutral);
        m_compareSummary->setText(FW_TR("The comparison did not finish."));
        break;
    }
}

QString ImageToolsPage::compareStatus() const { return m_compareBadge->text(); }

CompareResultSheet* ImageToolsPage::showCompareResult()
{
    if (!m_compareResult.valid || m_compareResult.state != JobState::Succeeded || !m_compareResult.details.contains(QStringLiteral("counts")))
        return nullptr;
    auto* s = new CompareResultSheet(window(), m_compareResult.details, m_compareResult.summary, m_compareResult.warnings);
    s->open();
    return s;
}

// ---- Convert ----------------------------------------------------------------------------

void ImageToolsPage::rebuildConvertChoices()
{
    const QString from = m_summary.format;
    if (m_convertTo && from == m_convertFrom)
        return;
    const QString keep = m_convertTo ? m_convertTo->currentValue().toString() : QString();
    delete m_convertTo;
    m_convertFrom = from;
    m_convertTo = new ChoiceTiles(m_convertTilesHost);
    m_convertTo->setAccessibleName(FW_TR("Target format"));
    struct Target {
        QString title, text, icon, value, tag;
    };
    const QList<Target> all = {
        {QStringLiteral("exFAT"), FW_TR("Mounts with ShadowMount+, any loader and on a PC."), QStringLiteral("drive"), jobs::kFormatExfat, {}},
        {QStringLiteral("FFPKG"), FW_TR("UFS2 image. ShadowMount+ mounts it without extracting."), QStringLiteral("package"), jobs::kFormatFfpkg, {}},
        {QStringLiteral("FFPFSC"), FW_TR("Compressed PFS image. Saves space; how much depends on the game."), QStringLiteral("compress"), jobs::kFormatFfpfsc, {}},
        {QStringLiteral("FFPFS"), FW_TR("Uncompressed PFS image. Fastest to read."), QStringLiteral("layers"), jobs::kFormatFfpfs, {}},
    };
    for (const Target& t : all)
        if (t.value != from)
            m_convertTo->addChoice(t.title, t.text, t.icon, t.value, t.tag);
    m_convertTilesHost->layout()->addWidget(m_convertTo);
    m_convertTo->setCurrentIndex(0);
    if (!keep.isEmpty())
        m_convertTo->setCurrentValue(keep);
    connect(m_convertTo, &ChoiceTiles::currentChanged, this, [this](int) { updateConvert(); });
}

void ImageToolsPage::setConvertTarget(const QString& format)
{
    rebuildConvertChoices();
    if (m_convertTo)
        m_convertTo->setCurrentValue(format);
    updateConvert();
}

QString ImageToolsPage::convertTarget() const { return m_convertTo ? m_convertTo->currentValue().toString() : QString(); }

QString ImageToolsPage::convertMethod() const
{
    const QString f = m_summary.format;
    if ((f != u"exfat" && f != u"ffpkg") || convertTarget() != jobs::kFormatFfpfsc)
        return {};
    return m_method->currentValue().toString();
}

void ImageToolsPage::setConvertMethod(const QString& method)
{
    m_method->setCurrentValue(method);
    updateConvert();
}

QString ImageToolsPage::convertOutput() const
{
    const QString ext = jobs::outputExtension(convertTarget());
    QString out = m_convertOutput->path();
    if (out.isEmpty())
        return nextToImage(m_path, ext);
    if (!out.endsWith(ext, Qt::CaseInsensitive))
        out = (QFileInfo(out).suffix().isEmpty() || imageFormatOf(out).isEmpty() ? out + ext : nextToImage(out, ext));
    return out;
}

void ImageToolsPage::setConvertOutput(const QString& file) { m_convertOutput->setPath(file); }

void ImageToolsPage::updateConvert()
{
    if (!m_convertTo)
        return;
    const QString method = convertMethod();
    m_methodRow->setVisible(!method.isEmpty());
    m_methodHelp->setText(method == u"repack" ? FW_TR("Rebuilds a PFS image of the game; needs room for a temporary copy.")
                                              : FW_TR("Fast and needs no extra space; the .ffpfsc holds the image file."));
    if (!m_path.isEmpty()) {
        m_convertOutput->setPlaceholder(nativePath(nextToImage(m_path, jobs::outputExtension(convertTarget()))));
        m_convertOutput->setFilter(FW_TR("%1 image").arg(imageFormatName(convertTarget())) + QStringLiteral(" (*%1)").arg(jobs::outputExtension(convertTarget())));
    }
    // A chosen name that carries another image format's extension is written
    // with the target's: the field shows the file the job will make.
    const QString chosen = m_convertOutput->path();
    if (!chosen.isEmpty() && !convertTarget().isEmpty() && !imageFormatOf(chosen).isEmpty()
        && !chosen.endsWith(jobs::outputExtension(convertTarget()), Qt::CaseInsensitive))
        m_convertOutput->setPath(nativePath(convertOutput()));
}

// ---- Edit -------------------------------------------------------------------------------

void ImageToolsPage::onTree(const ImageTreeResult& r)
{
    if (r.image != m_treeFor || m_demo)
        return;
    m_tree = r;
    rebuildTree();
    emit statusContextChanged();
}

void ImageToolsPage::rebuildTree()
{
    m_treeView->clear();
    const bool open = !m_tree.nodes.empty() && m_tree.error.isEmpty();
    m_treeView->setVisible(open);
    m_treeError->setVisible(!m_tree.error.isEmpty());
    m_treeError->setBody(m_tree.error);
    m_treeEmpty->setVisible(!open && m_tree.error.isEmpty());
    m_btnAddFiles->setEnabled(open);
    m_btnAddFolder->setEnabled(open);
    m_btnMkdir->setEnabled(open);
    if (open) {
        QStringList parts{m_tree.fileCount == 1 ? FW_TR("1 file") : FW_TR("%1 files").arg(formatCount(m_tree.fileCount)),
                          formatBytes(m_tree.totalBytes)};
        if (!m_tree.problems.isEmpty())
            parts << (m_tree.problems.size() == 1 ? FW_TR("%1 problem").arg(m_tree.problems.size()) : FW_TR("%1 problems").arg(m_tree.problems.size()));
        m_treeCount->setText(parts.join(QStringLiteral(" · ")));
        m_treeCount->setToolTip((m_tree.problems + m_tree.warnings).join(u'\n'));
    } else {
        m_treeCount->clear();
    }
    updateTreeButtons();
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
        if (!n.isDir)
            it->setToolTip(0, plainTip(FW_TR("/%1\nDouble-click to preview its bytes").arg(n.path)));
        it->setText(1, n.isDir ? QString() : formatBytes(n.size));
        it->setForeground(1, color::fg_3);
        it->setText(2, n.modified.isValid() ? humanTime(n.modified) : QString());
        it->setForeground(2, color::fg_3);
        items[i] = it;
    }
    // The whole tree is put together first and handed to the view in one go
    // (an image can hold a hundred thousand files).
    QList<QTreeWidgetItem*> top, orphans;
    for (size_t i = 1; i < m_tree.nodes.size(); ++i) {
        const int parent = m_tree.nodes[i].parent;
        if (parent <= 0)
            top << items[i];
        else if (size_t(parent) < items.size() && items[size_t(parent)])
            items[size_t(parent)]->addChild(items[i]);
        else
            orphans << items[i];
    }
    qDeleteAll(orphans); // nodes without their folder: nothing to hang them on
    m_treeView->setUpdatesEnabled(false);
    m_treeView->addTopLevelItems(top);
    m_treeView->sortItems(0, Qt::AscendingOrder);
    m_treeView->setUpdatesEnabled(true);
}

void ImageToolsPage::updateTreeButtons()
{
    bool dir = false;
    const QString p = selectedPath(&dir);
    const bool open = !m_tree.nodes.empty() && m_tree.error.isEmpty();
    m_btnReplace->setEnabled(open && !p.isEmpty() && !dir);
    m_btnDelete->setEnabled(open && !p.isEmpty());
    m_btnRename->setEnabled(open && !p.isEmpty() && m_tree.format == QLatin1String("ffpkg"));
}

QString ImageToolsPage::selectedPath(bool* isDir) const
{
    const auto sel = m_treeView->selectedItems();
    if (sel.isEmpty())
        return {};
    if (isDir)
        *isDir = sel.first()->data(0, Qt::UserRole + 1).toBool();
    return sel.first()->data(0, Qt::UserRole).toString();
}

void ImageToolsPage::stage(const StagedOp& op)
{
    m_ops.push_back(op);
    rebuildStaged();
}

void ImageToolsPage::clearStaged()
{
    m_ops.clear();
    rebuildStaged();
}

void ImageToolsPage::rebuildStaged()
{
    // Later, not now: a row's own "remove" button is on the stack when it asks for this.
    while (QLayoutItem* it = m_stagedLayout->takeAt(0)) {
        if (QWidget* w = it->widget()) {
            w->hide();
            w->deleteLater();
        }
        delete it;
    }
    m_stagedRemove.clear();
    m_stagedCaption->setVisible(m_ops.empty());
    m_clear->setVisible(!m_ops.empty());
    m_stagedCard->setTitleText(m_ops.empty() ? FW_TR("Staged changes") : FW_TR("Staged changes · %1").arg(m_ops.size()));
    for (size_t i = 0; i < m_ops.size(); ++i) {
        auto* row = new QWidget(m_stagedCard);
        auto* h = hbox(row, space::sm, QMargins(0, 6, 0, 6));
        const QString& op = m_ops[i].op;
        const QString iconName = op == QLatin1String("delete") ? QStringLiteral("trash") : op == QLatin1String("mkdir") ? QStringLiteral("folder_plus")
                               : op == QLatin1String("rename") ? QStringLiteral("pencil") : op == QLatin1String("replace") ? QStringLiteral("replace") : QStringLiteral("file_plus");
        auto* ic = new QLabel(row);
        ic->setPixmap(iconPixmap(iconName, size::icon, op == QLatin1String("delete") ? color::danger : color::fg_3, devicePixelRatioF()));
        ic->setFixedSize(size::icon, size::icon);
        h->addWidget(ic);
        auto* t = new ElideLabel(m_ops[i].describe(), Qt::ElideMiddle, row);
        t->setFont(font::mono());
        setTextColor(t, color::fg_1);
        t->setToolTip(m_ops[i].describe());
        h->addWidget(t, 1);
        auto* x = new IconButton(QStringLiteral("close"), FW_TR("Remove this change"), IconButton::Size::Small, row);
        connect(x, &QToolButton::clicked, this, [this, i] {
            if (i < m_ops.size())
                m_ops.erase(m_ops.begin() + long(i));
            rebuildStaged();
            // The keyboard stays in the list: on the row that took this one's place.
            if (!m_stagedRemove.isEmpty())
                m_stagedRemove[std::min(qsizetype(i), m_stagedRemove.size() - 1)]->setFocus();
            else
                m_treeView->setFocus();
        });
        m_stagedRemove << x;
        h->addWidget(x);
        m_stagedLayout->addWidget(row);
        if (i + 1 < m_ops.size())
            m_stagedLayout->addWidget(hairline(m_stagedCard));
    }
    updatePrimary();
    emit statusContextChanged();
}

void ImageToolsPage::addFiles(bool folder)
{
    bool dir = false;
    QString target = selectedPath(&dir);
    if (!target.isEmpty() && !dir)
        target = target.contains(u'/') ? target.section(u'/', 0, -2) : QString();
    if (folder) {
        const QString d = QFileDialog::getExistingDirectory(window(), FW_TR("Choose a folder to add"));
        if (d.isEmpty())
            return;
        stage({QStringLiteral("add"), target.isEmpty() ? QFileInfo(d).fileName() : target + u'/' + QFileInfo(d).fileName(), d, {}});
        return;
    }
    // One rebuild of the list for the whole selection, not one per file.
    const QStringList files = QFileDialog::getOpenFileNames(window(), FW_TR("Choose files to add"));
    for (const QString& f : files)
        m_ops.push_back({QStringLiteral("add"), target.isEmpty() ? QFileInfo(f).fileName() : target + u'/' + QFileInfo(f).fileName(), f, {}});
    if (!files.isEmpty())
        rebuildStaged();
}

void ImageToolsPage::replaceSelected()
{
    bool dir = false;
    const QString p = selectedPath(&dir);
    if (p.isEmpty() || dir)
        return;
    const QString f = QFileDialog::getOpenFileName(window(), FW_TR("Choose the new content for %1").arg(p));
    if (!f.isEmpty())
        stage({QStringLiteral("replace"), p, f, {}});
}

void ImageToolsPage::deleteSelected()
{
    const QString p = selectedPath();
    if (!p.isEmpty())
        stage({QStringLiteral("delete"), p, {}, {}});
}

void ImageToolsPage::newFolder()
{
    bool dir = false;
    QString target = selectedPath(&dir);
    if (!target.isEmpty() && !dir)
        target = target.contains(u'/') ? target.section(u'/', 0, -2) : QString();
    auto* s = new Sheet(window(), FW_TR("New folder"), Sheet::Icon::None);
    auto* name = textInput(FW_TR("Folder name"), true, s);
    auto* nameField = field(target.isEmpty() ? FW_TR("Name (in the image root)") : FW_TR("Name (in /%1)").arg(target), name, {}, s);
    s->body()->addWidget(nameField);
    const QString n = askName(s, name);
    if (!n.isEmpty())
        stage({QStringLiteral("mkdir"), target.isEmpty() ? n : target + u'/' + n, {}, {}});
}

// Runs a sheet that asks for a name in the image: the field has the focus, and
// "Stage" keeps the sheet open with the reason while the name cannot be used.
// Returns the name, or "" when the user cancelled.
QString ImageToolsPage::askName(Sheet* s, QLineEdit* name)
{
    auto* why = new MessageRow(s);
    s->body()->addWidget(why);
    s->addButton(FW_TR("Cancel"), Sheet::Role::Safe);
    QPushButton* stageButton = s->addButton(FW_TR("Stage change"), Sheet::Role::Primary);
    const QString format = m_tree.format;
    const auto check = [name, why, format] {
        const QString problem = imageNameProblem(name->text().trimmed(), format);
        why->set(problem, Tone::Err);
        setInputError(name, !problem.isEmpty());
        return problem.isEmpty();
    };
    QObject::disconnect(stageButton, &QPushButton::clicked, s, nullptr); // an unusable name keeps the sheet open
    connect(stageButton, &QPushButton::clicked, s, [s, name, check] {
        if (check())
            s->close(1);
        else
            name->setFocus();
    });
    connect(name, &QLineEdit::textEdited, why, [name, why] {
        why->clear();
        setInputError(name, false);
    });
    connect(name, &QLineEdit::returnPressed, stageButton, &QPushButton::click);
    // The sheet focuses its primary button when it opens; the field is what the user came for.
    QTimer::singleShot(0, name, [name] {
        name->setFocus();
        name->selectAll();
    });
    if (s->exec() != 1)
        return {};
    const QString n = name->text().trimmed();
    const QString problem = imageNameProblem(n, format);
    if (!problem.isEmpty()) { // closed some other way with a name that cannot be used
        showFormError(FW_TR("The name cannot be used"), problem);
        return {};
    }
    return n;
}

void ImageToolsPage::renameSelected()
{
    const QString p = selectedPath();
    if (p.isEmpty())
        return;
    auto* s = new Sheet(window(), FW_TR("Rename"), Sheet::Icon::None);
    auto* name = textInput({}, true, s);
    name->setText(p.section(u'/', -1));
    s->body()->addWidget(field(FW_TR("New name"), name, {}, s));
    const QString n = askName(s, name);
    if (!n.isEmpty() && n != p.section(u'/', -1))
        stage({QStringLiteral("rename"), p, {}, n});
}

void ImageToolsPage::showTreeMenu(const QPoint& pos)
{
    QTreeWidgetItem* it = m_treeView->itemAt(pos);
    if (!it)
        return;
    m_treeView->setCurrentItem(it);
    const bool dir = it->data(0, Qt::UserRole + 1).toBool();
    const QString path = it->data(0, Qt::UserRole).toString();
    QMenu menu(this);
    QAction* prev = menu.addAction(FW_TR("Preview…"), this, [this, path] { (void)preview(path); });
    prev->setEnabled(!dir);
    menu.addSeparator();
    menu.addAction(FW_TR("Replace…"), this, &ImageToolsPage::replaceSelected)->setEnabled(m_btnReplace->isEnabled());
    menu.addAction(FW_TR("Rename"), this, &ImageToolsPage::renameSelected)->setEnabled(m_btnRename->isEnabled());
    menu.addAction(FW_TR("Delete"), this, &ImageToolsPage::deleteSelected)->setEnabled(m_btnDelete->isEnabled());
    menu.exec(m_treeView->viewport()->mapToGlobal(pos));
}

HexPreviewSheet* ImageToolsPage::preview(const QString& path)
{
    if (m_tree.nodes.empty() || !m_tree.error.isEmpty())
        return nullptr;
    qint64 size = -1;
    bool found = false;
    for (const ImageNode& n : m_tree.nodes)
        if (n.path == path && !n.isDir) {
            size = n.size;
            found = true;
            break;
        }
    if (!found)
        return nullptr;
    auto* s = new HexPreviewSheet(window(), m_tree.image, path, size);
    s->start();
    s->open();
    return s;
}

QJsonArray ImageToolsPage::operationsJson() const
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

// ---- submission -------------------------------------------------------------------------

QString ImageToolsPage::validateForm()
{
    if (m_path.isEmpty())
        return FW_TR("Open an image first.");
    if (!QFileInfo(m_path).isFile())
        return FW_TR("The image does not exist.");
    if (m_summary.format.isEmpty())
        return FW_TR("Choose an .exfat, .ffpkg, .ffpfs, .ffpfsc or .pkg file.");
    // What is inside the image (packs, an inner image) decides which job extracts or converts it.
    if ((m_action == Action::Extract || m_action == Action::Convert) && m_info->isBusy())
        return FW_TR("The image is still being read. Try again in a moment.");
    switch (m_action) {
    case Action::Extract:
        if (!m_destination->path().isEmpty() && isSameOrInside(m_path, m_destination->path()))
            return FW_TR("The destination cannot contain the image.");
        return {};
    case Action::Verify: {
        if (!compareMode())
            return {};
        QString problem;
        return checkCompareInputs(&problem) ? QString() : problem;
    }
    case Action::Convert:
        if (convertTarget().isEmpty())
            return FW_TR("Choose the format to convert to.");
        if (QDir::cleanPath(QFileInfo(convertOutput()).absoluteFilePath()) == QDir::cleanPath(QFileInfo(m_path).absoluteFilePath()))
            return FW_TR("The converted image cannot replace its source.");
        return {};
    case Action::Edit:
        if (!m_tree.error.isEmpty())
            return m_tree.error;
        if (m_ops.empty())
            return FW_TR("Stage at least one change.");
        return {};
    }
    return {};
}

Result<QString> ImageToolsPage::submit(bool)
{
    switch (m_action) {
    case Action::Extract: {
        QString dest = m_destination->path();
        if (dest.isEmpty())
            dest = nextToImage(m_path);
        if (usesGameExtract())
            return jobs::submitGameExtract(m_ctx, m_path, dest, m_summary.packs.isEmpty() || m_unpackPacks->isChecked(),
                                           jobOverrides());
        return jobs::submitExtract(m_ctx, m_path, dest, jobOverrides());
    }
    case Action::Verify: {
        if (!compareMode())
            return jobs::submitVerify(m_ctx, m_path, jobOverrides());
        auto r = jobs::submitCompare(m_ctx, m_path, m_compareB->path(), m_compareDepth->currentValue().toString());
        if (r.ok()) {
            m_compareJobId = r.value();
            onCompareJob(m_compareJobId, false);
        }
        return r;
    }
    case Action::Convert: {
        const QString out = convertOutput();
        bool replace = false;
        if (QFileInfo::exists(out)) {
            if (!Sheet::confirm(window(), FW_TR("Replace the existing image?"),
                                FW_TR("%1 exists. It is set aside while the new image is written and put back if the conversion fails.").arg(Sheet::monoSpan(nativePath(out))),
                                FW_TR("Cancel"), FW_TR("Replace")))
                return Error(ErrorCode::Cancelled, QStringLiteral("Nothing was converted."));
            replace = true;
        }
        return jobs::submitConvert(m_ctx, m_path, convertTarget(), out, jobOverrides(), convertMethod(), replace);
    }
    case Action::Edit: {
        // A sheet does not scroll: the first changes are listed, the rest counted.
        constexpr size_t kListed = 12;
        int deletes = 0;
        QStringList lines;
        for (const auto& o : m_ops) {
            if (o.op == QLatin1String("delete") || o.op == QLatin1String("replace"))
                ++deletes;
            if (size_t(lines.size()) < kListed)
                lines << o.describe();
        }
        QString list = Sheet::monoLines(lines); // escapes the names
        if (m_ops.size() > kListed)
            list += QStringLiteral("<br>") + FW_TR("… and %1 more").arg(m_ops.size() - kListed);
        const QString title = m_ops.size() == 1 ? FW_TR("Apply %1 change to %2?").arg(m_ops.size()).arg(QFileInfo(m_path).fileName())
                                                : FW_TR("Apply %1 changes to %2?").arg(m_ops.size()).arg(QFileInfo(m_path).fileName());
        if (!Sheet::confirm(window(), title,
                            list + (deletes > 0 ? FW_TR("<br><br>Deleted and replaced files cannot be recovered from the image afterwards. The edit is verified before the original is replaced.") : QString()),
                            FW_TR("Keep editing"), FW_TR("Apply"), deletes > 0, deletes > 0 ? Sheet::Icon::Warning : Sheet::Icon::Info))
            return Error(ErrorCode::Cancelled, QStringLiteral("Not applied."));
        auto r = jobs::submitImageEdit(m_ctx, m_path, operationsJson(), jobOverrides());
        if (r.ok()) {
            m_ops.clear();
            rebuildStaged();
        }
        return r;
    }
    }
    return Error(ErrorCode::InvalidArgument, QStringLiteral("Choose an action."));
}

// The overrides of the rows this action shows: a value changed under Convert
// is not sent with a later Extract, where its row is hidden.
QVariantMap ImageToolsPage::jobOverrides() const
{
    QVariantMap out;
    const JobOptionsPanel* panel = const_cast<ImageToolsPage*>(this)->options();
    const QVariantMap all = panel->overrides();
    for (auto it = all.cbegin(); it != all.cend(); ++it)
        if (SettingRow* row = panel->row(it.key()); row && !row->isHidden())
            out.insert(it.key(), it.value());
    return out;
}

// ---- page hooks -------------------------------------------------------------------------

bool ImageToolsPage::acceptsDrop(const QList<QUrl>& urls) const
{
    // Not while the job monitor is up: the drop would change the form hidden behind it.
    if (urls.isEmpty() || isMonitorShown())
        return false;
    const QString f = urls.first().toLocalFile();
    if (!imageFormatOf(f, true).isEmpty() && QFileInfo(f).isFile())
        return true;
    // A folder dropped while comparing becomes B.
    return QFileInfo(f).isDir() && !m_path.isEmpty() && m_action == Action::Verify && compareMode();
}

void ImageToolsPage::dropUrls(const QList<QUrl>& urls)
{
    if (!acceptsDrop(urls))
        return;
    const QString f = urls.first().toLocalFile();
    if (QFileInfo(f).isDir())
        m_compareB->setPath(f);
    else if (m_ops.empty() || f == m_path)
        openImage(f);
    else // the question about the staged changes waits until the drop itself is over
        QTimer::singleShot(0, this, [this, f] { requestImage(f); });
}

void ImageToolsPage::setDragHighlight(bool on)
{
    JobFormPage::setDragHighlight(on);
    m_drop->setHighlighted(on);
}

void ImageToolsPage::navigateWith(const QVariantMap& args)
{
    // Asked to open an image or an action: that is the form, not the monitor of an earlier job.
    if (!args.isEmpty())
        showForm();
    QString image = args.value(QStringLiteral("image")).toString();
    if (image.isEmpty())
        image = args.value(QStringLiteral("source")).toString();
    if (!image.isEmpty()) {
        requestImage(image);
        if (m_path != image) // the user kept the staged changes of the open image
            return;
    }
    const QString a = args.value(QStringLiteral("action")).toString();
    if (a == QLatin1String("extract"))
        setAction(Action::Extract);
    else if (a == QLatin1String("verify") || a == QLatin1String("compare")) {
        setAction(Action::Verify);
        m_verifyMode->setCurrentValue(a == QLatin1String("compare") ? QStringLiteral("compare") : QStringLiteral("self"));
    } else if (a == QLatin1String("convert"))
        setAction(Action::Convert);
    else if (a == QLatin1String("edit"))
        setAction(Action::Edit);
    if (args.contains(QStringLiteral("format")))
        setConvertTarget(args.value(QStringLiteral("format")).toString());
    if (args.contains(QStringLiteral("compareWith"))) {
        setAction(Action::Verify);
        m_verifyMode->setCurrentValue(QStringLiteral("compare"));
        m_compareB->setPath(args.value(QStringLiteral("compareWith")).toString());
    }
}

void ImageToolsPage::refresh()
{
    if (!m_path.isEmpty() && !m_demo)
        openImage(m_path);
}

QString ImageToolsPage::statusContext() const
{
    if (m_path.isEmpty())
        return {};
    const QString name = QFileInfo(m_path).fileName();
    if (m_action == Action::Edit)
        return FW_TR("%1 · %2 staged").arg(name).arg(m_ops.size());
    return m_summary.bytes > 0 ? QStringLiteral("%1 · %2").arg(name, formatBytes(m_summary.bytes)) : name;
}

// ---- screenshots ------------------------------------------------------------------------

QStringList ImageToolsPage::demoStates() const
{
    return {QStringLiteral("extract"), QStringLiteral("verify"), QStringLiteral("compare"), QStringLiteral("compare-result"),
            QStringLiteral("convert"), QStringLiteral("edit"), QStringLiteral("hex-preview"), QStringLiteral("error")};
}

void ImageToolsPage::setDemoImage(const QString& path, qint64 bytes)
{
    m_demo = true;
    m_path = path;
    {
        const QSignalBlocker block(m_imageField);
        m_imageField->setPath(path);
    }
    m_imageMsg->clear();
    m_summary = {};
    m_summary.path = path;
    m_summary.format = imageFormatOf(path, true);
    m_summary.exists = true;
    m_summary.bytes = bytes;
    m_summary.title = QStringLiteral("Astro Bot");
    m_summary.titleId = QStringLiteral("PPSA01325");
    m_summary.version = QStringLiteral("01.002.000");
    m_summary.files = 18442;
    m_summary.fromImage = true;
    setBadge(m_imageBadge, FW_TR("Ready"), Tone::Ok);
    showStrip();
    updateActions();
}

void ImageToolsPage::applyDemoState(const QString& name)
{
    if (name == QLatin1String("error")) {
        setDemoImage(kDemoImage, 48500000000LL);
        setAction(Action::Extract);
        showFormError(QStringLiteral("Not enough space on D:"), QStringLiteral("Extracting needs 48.5 GB; 31.2 GB is free. Free 17.3 GB or choose another folder."));
        return;
    }
    setDemoImage(kDemoImage, 48500000000LL);
    if (name == QLatin1String("extract")) {
        setAction(Action::Extract);
    } else if (name == QLatin1String("verify")) {
        setAction(Action::Verify);
        m_verifyMode->setCurrentValue(QStringLiteral("self"));
    } else if (name == QLatin1String("compare")) {
        setAction(Action::Verify);
        m_verifyMode->setCurrentValue(QStringLiteral("compare"));
        m_compareDemo = true;
        m_compareB->setPath(QStringLiteral("D:/Dumps/PPSA01325-app"));
        m_compareResultRow->show();
        setBadge(m_compareBadge, QStringLiteral("6 differences"), Tone::Warn);
        m_compareSummary->setText(QStringLiteral("PPSA01325 Astro Bot (01.002).exfat ↔ PPSA01325-app\n") + kDemoSummary);
        m_compareView->show();
    } else if (name == QLatin1String("compare-result")) {
        setAction(Action::Verify);
        m_verifyMode->setCurrentValue(QStringLiteral("compare"));
        auto* s = new CompareResultSheet(window(), demoCompareDetails(), kDemoSummary, {QStringLiteral("B: Skipped fakelib/link.sprx: a link or special file.")});
        s->open();
        m_demoSheet = s;
    } else if (name == QLatin1String("convert")) {
        setAction(Action::Convert);
        setConvertTarget(jobs::kFormatFfpfsc);
    } else if (name == QLatin1String("edit") || name == QLatin1String("hex-preview")) {
        setAction(Action::Edit);
        ImageTreeResult t;
        t.image = kDemoImage;
        t.format = QStringLiteral("exfat");
        const auto node = [&t](const QString& path, bool dir, qint64 size, int parent) {
            ImageNode n;
            n.path = path;
            n.name = path.section(u'/', -1);
            n.isDir = dir;
            n.size = size;
            n.parent = parent;
            n.modified = QDateTime(QDate(2026, 10, 14), QTime(21, 4));
            t.nodes.push_back(n);
            return int(t.nodes.size() - 1);
        };
        node({}, true, 0, -1);
        node(QStringLiteral("eboot.bin"), false, 2355200, 0);
        const int sys = node(QStringLiteral("sce_sys"), true, 0, 0);
        node(QStringLiteral("sce_sys/param.json"), false, 2214, sys);
        node(QStringLiteral("sce_sys/icon0.png"), false, 512340, sys);
        node(QStringLiteral("sce_sys/pic0.png"), false, 2048000, sys);
        const int mod = node(QStringLiteral("sce_module"), true, 0, 0);
        node(QStringLiteral("sce_module/libc.prx"), false, 1310720, mod);
        node(QStringLiteral("sce_module/libSceFios2.prx"), false, 262144, mod);
        const int dataDir = node(QStringLiteral("data"), true, 0, 0);
        node(QStringLiteral("data/level1.pak"), false, 21474836480LL, dataDir);
        node(QStringLiteral("data/level2.pak"), false, 17179869184LL, dataDir);
        node(QStringLiteral("data/movies.pak"), false, 9663676416LL, dataDir);
        t.fileCount = 18442;
        t.dirCount = 1203;
        t.totalBytes = 48500000000LL;
        m_tree = t;
        m_treeFor = kDemoImage;
        rebuildTree();
        for (int i = 0; i < m_treeView->topLevelItemCount(); ++i)
            if (auto* top = m_treeView->topLevelItem(i); top->text(0) == QLatin1String("sce_sys"))
                top->setExpanded(true);
        m_ops = {{QStringLiteral("add"), QStringLiteral("sce_sys/about/right.sprx"), QStringLiteral("D:/Patches/right.sprx"), {}},
                 {QStringLiteral("replace"), QStringLiteral("eboot.bin"), QStringLiteral("D:/Patches/eboot.bin"), {}},
                 {QStringLiteral("delete"), QStringLiteral("data/movies.pak"), {}, {}}};
        rebuildStaged();
        if (name == QLatin1String("hex-preview")) {
            // A sample slice (no image is read): an ELF-like header, then mixed bytes and strings.
            QByteArray bytes("\x7f" "ELF\x02\x01\x01\x09", 8);
            bytes += QByteArray(8, '\0');
            bytes += QByteArray::fromHex("0300 3e00 0100 0000 c0a3 0100 0000 0000 4000 0000 0000 0000");
            const QByteArray words = QByteArrayLiteral("sce_module/libc.prx\0libSceFios2.prx\0/app0/sce_sys/param.json\0");
            for (int i = 0; bytes.size() < int(kHexPreviewChunk); ++i)
                bytes += (i % 9 == 4) ? words : QByteArray(1, char((i * 37 + (i >> 3)) & 0xFF));
            bytes.truncate(qsizetype(kHexPreviewChunk));
            auto* s = new HexPreviewSheet(window(), kDemoImage, QStringLiteral("eboot.bin"), 2355200);
            HexSlice slice;
            slice.image = s->image();
            slice.path = s->path();
            slice.offset = 0;
            slice.fileSize = 2355200;
            slice.data = bytes;
            s->showSlice(slice);
            s->open();
            m_demoSheet = s;
        }
    }
}

void ImageToolsPage::clearDemoState()
{
    JobFormPage::clearDemoState();
    if (auto* s = qobject_cast<Sheet*>(m_demoSheet.data()))
        s->close(0);
    m_compareDemo = false;
    m_demo = false;
    m_verifyMode->setCurrentValue(QStringLiteral("self"));
    m_action = Action::Extract;
    openImage({});
    showCompareState();
    scrollArea()->verticalScrollBar()->setValue(0);
}

} // namespace fw::pages
