#include "app/pages/BuildImagePage.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "engines/exfat/Module.h"
#include "engines/fpkg/ContentNaming.h"
#include "engines/fpkg/FpkgBuilder.h"
#include "engines/fpkg/FpkgSettings.h"
#include "engines/fpkg/PubCmd.h"
#include "engines/gameinfo/Module.h"
#include "engines/gameinfo/Naming.h"
#include "engines/general/Module.h"
#include "engines/integration/Module.h"
#include "engines/pfs/Module.h"
#include "engines/ufs2/Module.h"

#include <QActionGroup>
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStorageInfo>
#include <QTextLayout>
#include <QThread>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <functional>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

const QString kShared = QStringLiteral("Verification & after the build");

// Settings every image/package job of that format reads, besides its own
// engine's keys (checked against the *Jobs.cpp / FpkgSettings readers).
const QString kVerify = general::keys::kVerifyAfterBuild;
const QString kVerifyMode = general::keys::kVerifyMode;
const QString kThreads = general::keys::kWorkerThreads;
const QString kKeepTemp = general::keys::kKeepJobTemp;
const QString kAutoUpload = integration::keys::kAutoUpload;

bool isSharedKey(const QString& key)
{
    static const QStringList shared = {kVerify, kVerifyMode, kThreads, kKeepTemp, kAutoUpload,
                                       exfat::keys::kDeleteSourceAfterVerify, ufs2::keys::kDeleteSourceAfterBuild,
                                       fpkg::keys::kComputeSha256, fpkg::keys::kPostVerify};
    return shared.contains(key);
}

QString bucketTitle(const SettingDef& d)
{
    if (isSharedKey(d.key))
        return kShared;
    if (d.section == QLatin1String("PlayGo"))
        return QStringLiteral("PlayGo");
    static const QHash<QString, QString> renamed = {
        {QStringLiteral("exFAT images"), QStringLiteral("Image")},
        {QStringLiteral("exFAT contents"), QStringLiteral("Contents")},
        {QStringLiteral("FFPKG images (UFS2)"), QStringLiteral("Image")},
        {QStringLiteral("Folder images (.ffpfs)"), QStringLiteral("Files inside .ffpfs images")},
    };
    const QString g = d.group.isEmpty() ? d.section : d.group;
    return renamed.value(g, g);
}

// Legacy %TYPE% labels (ui/tab_unibuild.py _TYPE_LABEL).
QString typeLabel(const QString& tile)
{
    if (tile == jobs::kFormatFfpkg)
        return QStringLiteral("ffpkg");
    if (tile == jobs::kFormatFfpfsc)
        return QStringLiteral("PFS");
    if (tile == kFormatFpkg)
        return QStringLiteral("FPKG");
    return QStringLiteral("exFAT");
}

QString sourceKindOf(const QString& path)
{
    if (path.isEmpty())
        return {};
    const QFileInfo fi(path);
    if (fi.isDir())
        return QStringLiteral("folder");
    const QString ext = fi.suffix().toLower();
    if (ext == QLatin1String("exfat"))
        return QStringLiteral("exfat");
    if (ext == QLatin1String("ffpfsc") || ext == QLatin1String("ffpfs"))
        return QStringLiteral("ffpfsc");
    if (ext == QLatin1String("ffpkg"))
        return QStringLiteral("ffpkg");
    if (ext == QLatin1String("gp5"))
        return QStringLiteral("gp5");
    if (ext == QLatin1String("zip"))
        return QStringLiteral("zip"); // a zipped dump (#67): exFAT, FFPKG and PFS unpack it first
    return fi.exists() ? QStringLiteral("file") : QStringLiteral("folder");
}

QString sourceKindLabel(const QString& kind)
{
    if (kind == QLatin1String("exfat"))
        return QStringLiteral("exFAT image");
    if (kind == QLatin1String("ffpfsc"))
        return QStringLiteral("PFS image");
    if (kind == QLatin1String("ffpkg"))
        return QStringLiteral("FFPKG image");
    if (kind == QLatin1String("gp5"))
        return QStringLiteral(".gp5 project");
    if (kind == QLatin1String("zip"))
        return QStringLiteral("ZIP archive");
    return QStringLiteral("Game folder");
}

// ---- format tiles ------------------------------------------------------------------
// Four large choices in one row (two by two when narrow). ChoiceTiles lays four
// tiles out three-plus-one at form width, so the page draws its own row in the
// same visual language (selected: gradient border, tint and check bubble).
constexpr int kTileGap = 12;
constexpr int kTileH = 132;
constexpr int kTileMin4 = 150; // narrowest tile in a row of four
constexpr int kTileMin2 = 190;

QStringList wrapText(const QString& text, const QFont& font, int width, int maxLines)
{
    QStringList out;
    if (text.isEmpty() || width <= 10)
        return out;
    QTextLayout layout(text, font);
    layout.beginLayout();
    while (out.size() < maxLines) {
        QTextLine line = layout.createLine();
        if (!line.isValid())
            break;
        line.setLineWidth(width);
        const int start = line.textStart();
        const int len = line.textLength();
        if (out.size() == maxLines - 1 && start + len < text.size()) {
            out << QFontMetrics(font).elidedText(text.mid(start), Qt::ElideRight, width);
            break;
        }
        out << text.mid(start, len).trimmed();
    }
    layout.endLayout();
    return out;
}

class FormatTiles : public QWidget {
public:
    explicit FormatTiles(QWidget* parent) : QWidget(parent)
    {
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        setMouseTracking(true);
        QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Preferred);
        sp.setHeightForWidth(true);
        setSizePolicy(sp);
        setProperty("fwRadius", radius::md);
    }
    void addTile(const QString& title, const QString& description, const QString& icon, const QString& value,
                 const QString& tag = {})
    {
        m_items.push_back({title, description, icon, tag, value});
        if (m_current < 0)
            m_current = 0;
        updateGeometry();
        update();
    }
    int count() const { return int(m_items.size()); }
    int currentIndex() const { return m_current; }
    QString currentValue() const { return m_current >= 0 ? m_items[size_t(m_current)].value : QString(); }
    QString titleAt(int i) const { return m_items[size_t(i)].title; }
    void setCurrentValue(const QString& v)
    {
        for (int i = 0; i < count(); ++i)
            if (m_items[size_t(i)].value == v)
                setCurrentIndex(i);
    }
    void setCurrentIndex(int i)
    {
        if (i < 0 || i >= count() || i == m_current)
            return;
        m_current = i;
        update();
        if (onChanged)
            onChanged(i);
    }
    std::function<void(int)> onChanged;

    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override
    {
        const int cols = columns(w);
        const int rows = (count() + cols - 1) / cols;
        return rows * kTileH + std::max(0, rows - 1) * kTileGap;
    }
    QSize sizeHint() const override
    {
        const int w = count() * (kTileMin4 + 40);
        return {w, heightForWidth(w)};
    }
    QSize minimumSizeHint() const override { return {kTileMin2, kTileH}; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const qreal dpr = devicePixelRatioF();
        for (int i = 0; i < count(); ++i) {
            const Item& it = m_items[size_t(i)];
            const QRectF r = QRectF(tileRect(i)).adjusted(0.5, 0.5, -0.5, -0.5);
            const bool sel = i == m_current;
            const bool hov = i == m_hover && !sel;
            if (sel) {
                p.setPen(Qt::NoPen);
                p.setBrush(color::bg_2);
                p.drawRoundedRect(r, radius::md, radius::md);
                p.setBrush(accentGradient(r, 34));
                p.drawRoundedRect(r, radius::md, radius::md);
                p.setPen(QPen(QBrush(accentGradient(r, 230)), 1.5));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), radius::md, radius::md);
            } else {
                p.setPen(QPen(hov ? color::border_4 : color::border_3, 1));
                p.setBrush(hov ? color::bg_3 : color::field_bg);
                p.drawRoundedRect(r, radius::md, radius::md);
            }
            // Icon square, top left.
            const QRectF ir(r.left() + 14, r.top() + 14, 32, 32);
            p.setPen(Qt::NoPen);
            p.setBrush(sel ? QBrush(accentGradient(ir)) : QBrush(color::bg_4));
            p.drawRoundedRect(ir, 9, 9);
            p.drawPixmap(QRectF(ir.center().x() - 9, ir.center().y() - 9, 18, 18).toRect(),
                         iconPixmap(it.icon, 18, sel ? color::on_accent : color::fg_2, dpr));
            // Check bubble and tag, top right.
            qreal right = r.right() - 14;
            if (sel) {
                const QRectF cb(right - 20, r.top() + 20, 20, 20);
                p.setPen(Qt::NoPen);
                p.setBrush(accentGradient(cb));
                p.drawEllipse(cb);
                p.drawPixmap(QRectF(cb.center().x() - 6, cb.center().y() - 6, 12, 12).toRect(),
                             iconPixmap(QStringLiteral("check"), 12, color::on_accent, dpr));
                right -= 28;
            }
            if (!it.tag.isEmpty()) {
                const QFontMetrics cfm(font::caption());
                const qreal tw = cfm.horizontalAdvance(it.tag) + 14;
                if (right - tw > ir.right() + 8) {
                    const QRectF tagR(right - tw, r.top() + 21, tw, 18);
                    QColor tint = color::accent;
                    tint.setAlpha(46);
                    p.setPen(Qt::NoPen);
                    p.setBrush(tint);
                    p.drawRoundedRect(tagR, 9, 9);
                    p.setPen(color::accent_hi);
                    p.setFont(font::caption());
                    p.drawText(tagR, Qt::AlignCenter, it.tag);
                }
            }
            // Title and up to three lines of description.
            const qreal tx = r.left() + 14;
            const qreal tw = r.width() - 28;
            p.setFont(font::bodyStrong());
            p.setPen(color::fg_0);
            const QRectF titleR(tx, ir.bottom() + 10, tw, 20);
            p.drawText(titleR, Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(font::bodyStrong()).elidedText(it.title, Qt::ElideRight, int(tw)));
            p.setFont(font::caption());
            p.setPen(color::fg_3);
            qreal y = titleR.bottom() + 2;
            for (const QString& l : wrapText(it.description, font::caption(), int(tw), 3)) {
                p.drawText(QRectF(tx, y, tw, 16), Qt::AlignLeft | Qt::AlignVCenter, l);
                y += 16;
            }
            if (hasFocus() && sel) {
                p.setPen(QPen(color::focus, 2));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(-3, -3, 3, 3), radius::md + 3, radius::md + 3);
            }
        }
    }
    void mousePressEvent(QMouseEvent* e) override
    {
        const int i = at(e->pos());
        if (i >= 0)
            setCurrentIndex(i);
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        const int i = at(e->pos());
        if (i != m_hover) {
            m_hover = i;
            setCursor(i >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
            update();
        }
    }
    void leaveEvent(QEvent*) override
    {
        m_hover = -1;
        update();
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        const int cols = columns(width());
        int next = m_current;
        switch (e->key()) {
        case Qt::Key_Left: next = m_current - 1; break;
        case Qt::Key_Right: next = m_current + 1; break;
        case Qt::Key_Up: next = m_current - cols; break;
        case Qt::Key_Down: next = m_current + cols; break;
        default: QWidget::keyPressEvent(e); return;
        }
        if (next >= 0 && next < count())
            setCurrentIndex(next);
    }
    void focusInEvent(QFocusEvent* e) override
    {
        QWidget::focusInEvent(e);
        update();
    }
    void focusOutEvent(QFocusEvent* e) override
    {
        QWidget::focusOutEvent(e);
        update();
    }

private:
    struct Item {
        QString title, description, icon, tag, value;
    };
    int columns(int w) const
    {
        if (count() <= 1)
            return 1;
        if (w >= count() * kTileMin4 + (count() - 1) * kTileGap)
            return count();
        return w >= 2 * kTileMin2 + kTileGap ? 2 : 1;
    }
    QRect tileRect(int i) const
    {
        const int cols = columns(width());
        const int w = (width() - (cols - 1) * kTileGap) / cols;
        const int row = i / cols, col = i % cols;
        return QRect(col * (w + kTileGap), row * (kTileH + kTileGap), w, kTileH);
    }
    int at(const QPoint& pt) const
    {
        for (int i = 0; i < count(); ++i)
            if (tileRect(i).contains(pt))
                return i;
        return -1;
    }
    std::vector<Item> m_items;
    int m_current = -1;
    int m_hover = -1;
};

FormatTiles* tilesOf(QWidget* w) { return static_cast<FormatTiles*>(w); }

// A label + caption on the left, a control on the right, a divider under it:
// the look of a SettingRow for controls that are not settings.
class DividerRow : public QWidget {
public:
    using QWidget::QWidget;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(QRect(0, height() - 1, width(), 1), color::border_1);
    }
};

QWidget* plainRow(const QString& title, const QString& help, QWidget* control, QWidget* parent, QLabel** helpOut = nullptr)
{
    auto* row = new DividerRow(parent);
    auto* h = hbox(row, space::lg, QMargins(0, 6, 0, 6));
    auto* left = new QWidget(row);
    auto* col = vbox(left, 2);
    col->addWidget(label(title, TextStyle::Body, color::fg_1, left));
    auto* cap = caption(help, left);
    cap->setWordWrap(true);
    cap->setVisible(!help.isEmpty());
    col->addWidget(cap);
    if (helpOut)
        *helpOut = cap;
    h->addWidget(left, 1);
    if (control)
        h->addWidget(control, 0, Qt::AlignRight | Qt::AlignVCenter);
    return row;
}

QWidget* sectionHeading(const QString& text, bool first, QWidget* parent)
{
    auto* w = new QWidget(parent);
    auto* l = vbox(w, 0, QMargins(0, first ? 2 : space::lg, 0, 4));
    auto* lab = label(text.toUpper(), TextStyle::Overline, color::fg_3, w);
    l->addWidget(lab);
    return w;
}

} // namespace

// ---- construction ---------------------------------------------------------------------

BuildImagePage::BuildImagePage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent)
    : JobFormPage(ctx, queueModel, QStringLiteral("build"), QStringLiteral("Build image"),
                  QStringLiteral("Turn a game dump into an exFAT image that ShadowMount+ and PCs can mount."),
                  QStringLiteral("Build image"), QStringLiteral("Image building"), parent)
{
    m_reader = new GameInfoReader(this);
    m_reader->setFindGameRoots(true);
    connect(m_reader, &GameInfoReader::ready, this, &BuildImagePage::onSummary);
    connect(m_reader, &GameInfoReader::progress, this, [this](qint64 files, qint64 bytes) {
        m_sourceBadge->set(QStringLiteral("Scanning\u2026 %1 files \u00b7 %2").arg(formatCount(files), formatBytes(bytes)), Tone::Info);
        m_sourceBadge->setIconName(QStringLiteral("loader"));
    });

    buildSourceCard();
    buildFormatCard();
    buildPackageCard();
    buildSaveCard();
    buildAdvanced();
    buildSummaryCard();

    connect(&ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant&) {
        if (key == general::keys::kDefaultOutputDir)
            updateOutput();
        if (key == gameinfo::keys::kNamePreset || key == gameinfo::keys::kNameTemplate || key == gameinfo::keys::kFirmwareOverride) {
            m_nameEdited = false;
            renderFileName();
        }
        if (key == fpkg::keys::kPubToolsPath)
            m_ctx.checkDependenciesAsync();
        if (key == exfat::keys::kClusterSize)
            updateClusterNote();
    });
    connect(&ctx, &AppContext::dependenciesChecked, this, &BuildImagePage::updateToolsStatus);
    connect(&m_ctx.queue(), &JobQueue::jobChanged, this, [this](const QString& id) {
        auto rec = m_ctx.queue().record(id);
        if (rec && rec->state == JobState::Running && (rec->kind == QLatin1String("fpkg.build") || rec->kind == QLatin1String("fpkg.addon")))
            applyLiveCpuLimit();
    });
    onFormatChanged();
}

void BuildImagePage::buildSourceCard()
{
    auto* source = new Card(this);
    m_sourceCard = source;
    setSourceCard(source);
    m_sourceBadge = new Badge(QStringLiteral("Step 1"), Tone::Neutral, source);
    m_sourceBadge->setIconName(QString());
    source->setTitle(QStringLiteral("Game dump"), m_sourceBadge);
    m_drop = new DropZone(QStringLiteral("folder_open"), QStringLiteral("Drop a game dump folder here"),
                          QStringLiteral("or click to browse \u2014 the folder that holds eboot.bin and sce_sys"), source);
    m_drop->setAccessibleName(QStringLiteral("Choose the source"));
    connect(m_drop, &DropZone::clicked, this, &BuildImagePage::browseSource);
    source->body()->addWidget(m_drop);

    auto* strip = new Card(source, Card::Variant::Inset);
    m_strip = strip;
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
    auto* rescan = new IconButton(QStringLiteral("refresh"), QStringLiteral("Read the source again"), IconButton::Size::Regular, strip);
    connect(rescan, &QToolButton::clicked, this, &BuildImagePage::refresh);
    sh->addWidget(rescan);
    auto* change = button(QStringLiteral("Change\u2026"), ButtonVariant::Secondary, QStringLiteral("folder"), strip);
    connect(change, &QPushButton::clicked, this, &BuildImagePage::browseSource);
    sh->addWidget(change);
    strip->body()->addLayout(sh);
    m_strip->hide();
    source->body()->addWidget(m_strip);

    m_sourceMsg = new MessageRow(source);
    source->body()->addWidget(m_sourceMsg);
    // Several games below the chosen folder: one link per game.
    m_rootsBox = new QWidget(source);
    auto* rl = vbox(m_rootsBox, space::xs);
    m_rootsList = new QVBoxLayout;
    m_rootsList->setSpacing(space::xs);
    rl->addLayout(m_rootsList);
    m_rootsBox->hide();
    source->body()->addWidget(m_rootsBox);
    form()->addWidget(source);
}

void BuildImagePage::buildFormatCard()
{
    auto* card = new Card(this);
    card->setTitle(QStringLiteral("Format"));
    auto* tiles = new FormatTiles(card);
    m_formatTiles = tiles;
    tiles->addTile(QStringLiteral("exFAT"), QStringLiteral("Fastest to build. Mounts with ShadowMount+ and on a PC."),
                   QStringLiteral("drive"), jobs::kFormatExfat, QStringLiteral("Popular"));
    tiles->addTile(QStringLiteral("FFPKG"), QStringLiteral("UFS2 image (.ffpkg) that ShadowMount+ mounts as it is."),
                   QStringLiteral("package"), jobs::kFormatFfpkg);
    tiles->addTile(QStringLiteral("PFS"), QStringLiteral("Compressed .ffpfsc, smallest on disk, or a plain .ffpfs."),
                   QStringLiteral("compress"), jobs::kFormatFfpfsc, QStringLiteral("Smallest"));
    tiles->addTile(QStringLiteral("FPKG"), QStringLiteral("Installable package. Needs your own Sony Publishing Tools."),
                   QStringLiteral("package_check"), kFormatFpkg);
    tiles->setAccessibleName(QStringLiteral("Image format"));
    tiles->onChanged = [this](int) { onFormatChanged(); };
    card->body()->addWidget(tiles);

    m_essentialsHost = new QWidget(card);
    auto* eh = vbox(m_essentialsHost, 0);
    card->body()->addWidget(m_essentialsHost);
    form()->addWidget(card);

    // PFS: compressed container or uncompressed folder image (#36, #19).
    m_pfsKind = new Segmented(card);
    m_pfsKind->addSegment(QStringLiteral("Compressed"), {}, jobs::kFormatFfpfsc);
    m_pfsKind->addSegment(QStringLiteral("Uncompressed"), {}, jobs::kFormatFfpfs);
    m_pfsKind->setAccessibleName(QStringLiteral("PFS image type"));
    m_pfsKind->setToolTip(QStringLiteral("Compressed (.ffpfsc) is smallest on disk. Uncompressed (.ffpfs) is about the size of "
                                         "the dump and the fastest to read."));
    connect(m_pfsKind, &Segmented::currentChanged, this, [this](int) {
        applyRowVisibility();
        updatePfsRows();
        updateAdvancedSummary();
        if (!m_nameEdited)
            renderFileName();
        updateOutput();
    });

    // FPKG: the user's own Sony Publishing Tools, one compact row.
    auto* toolsRight = new QWidget(card);
    auto* trh = hbox(toolsRight, space::md);
    m_toolsBadge = new Badge(QStringLiteral("Checking\u2026"), Tone::Neutral, toolsRight);
    trh->addWidget(m_toolsBadge);
    m_toolsLink = new Link(QStringLiteral("Set up\u2026"), toolsRight);
    trh->addWidget(m_toolsLink);
    connect(m_toolsLink, &Link::activated, this, [this] {
        const DependencyInfo info = m_ctx.dependencies().last(QStringLiteral("sony.pubcmd"));
        emit navigateRequested({QStringLiteral("settings"), {{QStringLiteral("section"), QStringLiteral("Tools")},
                                                             {QStringLiteral("key"), fpkg::keys::kPubToolsPath},
                                                             {QStringLiteral("howToFix"), info.howToFix}}});
    });
    m_toolsRow = plainRow(QStringLiteral("Sony Publishing Tools"), {}, toolsRight, card, &m_toolsDetail);
    m_toolsRow->setAccessibleName(QStringLiteral("Sony Publishing Tools"));
    m_toolsDetail->setToolTip(QStringLiteral("FPKG builds run your own copy of prospero-pub-cmd.exe. Nothing from Sony is included or downloaded."));

    // One options panel per format: only that format's settings.
    const QStringList shared = {kVerify, kVerifyMode, kKeepTemp};
    m_formats.resize(4);
    m_formats[0].tile = jobs::kFormatExfat;
    buildFormatOptions(m_formats[0],
                       {QStringLiteral("Build"), QStringLiteral("Verification"), QStringLiteral("Performance"), QStringLiteral("Advanced"),
                        QStringLiteral("PS5 connection")},
                       QStringList{QStringLiteral("exfat."), kThreads, kAutoUpload} + shared,
                       {exfat::keys::kEditStrategy, exfat::keys::kExtractParallel, exfat::keys::kExtractThreads},
                       {exfat::keys::kClusterSize, exfat::keys::kImageSizeGiB});
    m_formats[1].tile = jobs::kFormatFfpkg;
    buildFormatOptions(m_formats[1],
                       {QStringLiteral("Build"), QStringLiteral("Verification"), QStringLiteral("Advanced"), QStringLiteral("PS5 connection")},
                       QStringList{QStringLiteral("ufs2."), kAutoUpload} + shared, {}, {});
    m_formats[2].tile = jobs::kFormatFfpfsc;
    buildFormatOptions(m_formats[2],
                       {QStringLiteral("PFS"), QStringLiteral("Verification"), QStringLiteral("Performance"), QStringLiteral("Advanced"),
                        QStringLiteral("PS5 connection")},
                       QStringList{QStringLiteral("pfs."), kThreads, kAutoUpload} + shared, {pfs::keys::kDeleteSourceAfterConvert},
                       {pfs::keys::kCompressionLevel, pfs::keys::kCompressFiles});
    m_formats[3].tile = kFormatFpkg;
    buildFormatOptions(m_formats[3],
                       {QStringLiteral("FPKG"), QStringLiteral("PlayGo"), QStringLiteral("Firmware and SDK"), QStringLiteral("Verification"),
                        QStringLiteral("Performance"), QStringLiteral("Advanced")},
                       {QStringLiteral("fpkg."), kVerify, kThreads, kKeepTemp}, {fpkg::keys::kPubToolsPath},
                       {fpkg::keys::kPlayGoChunkCount, fpkg::keys::kPostVerify});
    // The PFS type row leads the PFS essentials; the tools row leads FPKG's.
    // (layout items 0 and 1 are the hairline and the space under it)
    if (auto* l = qobject_cast<QVBoxLayout*>(m_formats[2].essentials->layout())) {
        m_pfsKindRow = plainRow(QStringLiteral("Image type"), QStringLiteral("Compressed (.ffpfsc) is smallest; uncompressed (.ffpfs) is fastest to read."),
                                m_pfsKind, m_formats[2].essentials);
        l->insertWidget(2, m_pfsKindRow);
    }
    if (auto* l = qobject_cast<QVBoxLayout*>(m_formats[3].essentials->layout()))
        l->insertWidget(2, m_toolsRow);
    for (auto& fo : m_formats)
        eh->addWidget(fo.essentials);
}

void BuildImagePage::buildFormatOptions(FormatOptions& fo, const QStringList& sections, const QStringList& keyFilter,
                                        const QStringList& exclude, const QStringList& essentials)
{
    fo.panel = new JobOptionsPanel(m_ctx, sections, exclude + QStringList{general::keys::kDefaultOutputDir}, this);
    fo.panel->setObjectName(QStringLiteral("options-%1").arg(fo.tile));
    fo.panel->setKeyFilter(keyFilter);
    fo.panel->setGroupCards(false);
    fo.panel->build();
    fo.panel->hide(); // its rows are placed below; the panel keeps their state
    QLayout* panelLayout = fo.panel->layout();
    const auto take = [panelLayout](SettingRow* row) {
        if (panelLayout)
            panelLayout->removeWidget(row);
    };

    // Essentials: a hairline under the tiles, then the rows.
    fo.essentials = new QWidget(m_essentialsHost);
    auto* el = vbox(fo.essentials, 0);
    auto* line = hairline(fo.essentials);
    el->addSpacing(space::md);
    el->insertWidget(0, line);
    for (const QString& key : essentials)
        if (SettingRow* row = fo.panel->row(key)) {
            take(row);
            el->addWidget(row);
            fo.essentialKeys << key;
        }
    fo.essentials->setVisible(false);

    // Advanced: the remaining rows by bucket, "Verification & after the build" last.
    QStringList order;
    QHash<QString, QStringList> byBucket;
    for (const QString& key : fo.panel->keys()) {
        if (fo.essentialKeys.contains(key))
            continue;
        const SettingDef* d = m_ctx.schema().find(key);
        if (!d)
            continue;
        const QString b = bucketTitle(*d);
        if (!order.contains(b) && b != kShared)
            order << b;
        byBucket[b] << key;
        fo.bucketOf.insert(key, b);
    }
    if (byBucket.contains(kShared))
        order << kShared;
    fo.advanced = new QWidget(this); // moved into the Advanced section by buildAdvanced()
    auto* al = vbox(fo.advanced, 0);
    for (const QString& b : order) {
        auto* head = sectionHeading(b, b == order.first(), fo.advanced);
        fo.headings.insert(b, head);
        al->addWidget(head);
        for (const QString& key : byBucket.value(b)) {
            SettingRow* row = fo.panel->row(key);
            take(row);
            al->addWidget(row);
            fo.advancedKeys << key;
        }
    }
    fo.advanced->setVisible(false);
    connect(fo.panel, &JobOptionsPanel::overridesChanged, this, [this, tile = fo.tile] {
        if (tile != this->tile())
            return;
        updateAdvancedSummary();
        updateClusterNote();
        updatePfsRows();
    });
}

void BuildImagePage::buildPackageCard()
{
    auto* details = new Card(this);
    m_packageCard = details;
    details->setTitle(QStringLiteral("Package"));
    m_packageType = new Segmented(details);
    m_packageType->addSegment(QStringLiteral("App"), {}, QStringLiteral("app"));
    m_packageType->addSegment(QStringLiteral("Add-on"), {}, QStringLiteral("addon"));
    m_packageType->addSegment(QStringLiteral("Update"), {}, QStringLiteral("update"));
    m_packageType->setAccessibleName(QStringLiteral("Package type"));
    details->body()->addWidget(field(QStringLiteral("Package type"), m_packageType, {}, details));
    connect(m_packageType, &Segmented::currentChanged, this, [this](int) { onPackageTypeChanged(); });

    m_contentId = textInput(QStringLiteral("UPxxxx-PPSAxxxxx_00-xxxxxxxxxxxxxxxx"), true, details);
    m_contentId->setMaxLength(36);
    m_contentId->setAccessibleName(QStringLiteral("Content ID"));
    m_contentId->setToolTip(QStringLiteral("36 characters, read from sce_sys/param.json. Change it only if you know why."));
    auto* grid = new QWidget(details);
    auto* gh = hbox(grid, space::md);
    gh->setAlignment(Qt::AlignTop);
    gh->addWidget(field(QStringLiteral("Content ID"), m_contentId, {}, grid), 3);
    m_version = textInput(QStringLiteral("01.000.000"), true, grid);
    m_version->setAccessibleName(QStringLiteral("Version"));
    gh->addWidget(field(QStringLiteral("Version"), m_version, {}, grid), 1);
    details->body()->addWidget(grid);
    m_title = textInput({}, false, details);
    m_title->setAccessibleName(QStringLiteral("Title"));
    details->body()->addWidget(field(QStringLiteral("Title"), m_title, {}, details));
    connect(m_contentId, &QLineEdit::textChanged, this, [this] { renderFileName(); });
    connect(m_version, &QLineEdit::textChanged, this, [this] { renderFileName(); });

    m_basePackage = new PathField(PathField::Mode::OpenFile, details);
    m_basePackage->setFilter(QStringLiteral("Packages (*.pkg)"));
    m_basePackage->setDialogTitle(QStringLiteral("Choose the base package installed on the console"));
    m_baseField = field(QStringLiteral("Base package"), m_basePackage,
                        QStringLiteral("The .pkg the console already has; updates are built against it."), details);
    m_baseField->hide();
    details->body()->addWidget(m_baseField);

    m_addonField = new QWidget(details);
    auto* al = vbox(m_addonField, space::md);
    auto* arow = new QWidget(m_addonField);
    auto* ah = hbox(arow, space::md);
    ah->setAlignment(Qt::AlignTop);
    m_entitlementKey = textInput(QStringLiteral("32 hexadecimal characters"), true, arow);
    m_entitlementKey->setMaxLength(32);
    m_entitlementKey->setAccessibleName(QStringLiteral("Entitlement key"));
    ah->addWidget(field(QStringLiteral("Entitlement key"), m_entitlementKey, {}, arow), 2);
    m_volumeType = dropdown(arow);
    m_volumeType->addItem(QStringLiteral("prospero_ac \u00b7 add-on with data"), QStringLiteral("prospero_ac"));
    m_volumeType->setToolTip(QStringLiteral("prospero_al (licence only) is not supported."));
    ah->addWidget(field(QStringLiteral("Volume type"), m_volumeType, {}, arow), 1);
    al->addWidget(arow);
    auto* drow = new QWidget(m_addonField);
    auto* dh = hbox(drow, space::sm);
    dh->addWidget(caption(QStringLiteral("The source is the add-on's own folder (addcont0\u2026), not the game."), drow), 1);
    auto* fromIni = button(QStringLiteral("From dlc_emu.ini\u2026"), ButtonVariant::Secondary, QStringLiteral("database"), drow);
    connect(fromIni, &QPushButton::clicked, this, &BuildImagePage::pickFromDlcEmu);
    dh->addWidget(fromIni);
    al->addWidget(drow);
    m_addonField->hide();
    details->body()->addWidget(m_addonField);
    form()->addWidget(details);
}

void BuildImagePage::buildSaveCard()
{
    auto* dest = new Card(this);
    dest->setTitle(QStringLiteral("Save to"));
    auto* grid = new QWidget(dest);
    auto* gh = vbox(grid, space::md); // folder, then the file name: both get the full width
    m_output = new PathField(PathField::Mode::Directory, grid);
    m_output->setDialogTitle(QStringLiteral("Choose the output folder"));
    m_output->edit()->setAccessibleName(QStringLiteral("Output folder"));
    auto* outField = new QWidget(grid);
    auto* ol = vbox(outField, space::xs);
    ol->addWidget(label(QStringLiteral("Folder"), TextStyle::Body, color::fg_1, outField));
    ol->addWidget(m_output);
    m_spaceRow = new MessageRow(outField);
    ol->addWidget(m_spaceRow);
    gh->addWidget(outField, 1);

    auto* nameField = new QWidget(grid);
    auto* nl = vbox(nameField, space::xs);
    auto* labelRow = new QHBoxLayout;
    labelRow->setSpacing(space::sm);
    m_fileLabel = label(QStringLiteral("File name"), TextStyle::Body, color::fg_1, nameField);
    labelRow->addWidget(m_fileLabel);
    labelRow->addStretch(1);
    m_resetName = new Link(QStringLiteral("Reset"), nameField);
    m_resetName->setFont(font::caption());
    m_resetName->setToolTip(QStringLiteral("Use the name from Settings \u203a Build \u203a Output name again."));
    m_resetName->hide();
    connect(m_resetName, &Link::activated, this, [this] {
        m_nameEdited = false;
        renderFileName();
    });
    labelRow->addWidget(m_resetName);
    nl->addLayout(labelRow);
    auto* nameRow = new QHBoxLayout;
    nameRow->setSpacing(space::sm);
    m_fileName = textInput({}, false, nameField);
    m_fileName->setAccessibleName(QStringLiteral("File name"));
    m_fileName->setToolTip(QStringLiteral("Named by Settings \u203a Build \u203a Output name. Type to change it for this job."));
    m_fileLabel->setBuddy(m_fileName);
    nameRow->addWidget(m_fileName, 1);
    m_fileExt = monoLabel(QStringLiteral(".exfat"), nameField, color::fg_3);
    nameRow->addWidget(m_fileExt);
    m_nameMenuButton = new IconButton(QStringLiteral("tag"), QStringLiteral("Naming"), IconButton::Size::Regular, nameField);
    nameRow->addWidget(m_nameMenuButton);
    nl->addLayout(nameRow);
    gh->addWidget(nameField, 1);
    dest->body()->addWidget(grid);
    m_spaceBar = new InfoBar(Tone::Err, dest);
    m_spaceBar->hide();
    dest->body()->addWidget(m_spaceBar);
    form()->addWidget(dest);

    connect(m_output, &PathField::pathChanged, this, [this](const QString&) {
        renderFileName();
        checkSpace();
    });
    connect(m_fileName, &QLineEdit::textEdited, this, [this](const QString& text) {
        m_nameEdited = true;
        m_resetName->show();
        const QString problem = text.trimmed().isEmpty() ? QString() : gameinfo::folderNameProblem(text.trimmed() + m_fileExt->text());
        setInputError(m_fileName, !problem.isEmpty());
        m_fileName->setToolTip(problem.isEmpty() ? QStringLiteral("Named by Settings \u203a Build \u203a Output name. Type to change it for this job.")
                                                 : problem);
    });
    connect(m_nameMenuButton, &QToolButton::clicked, this, [this] {
        QMenu menu(this);
        auto* group = new QActionGroup(&menu);
        const QString current = m_ctx.settings().value(gameinfo::keys::kNamePreset).toString();
        if (const SettingDef* d = m_ctx.schema().find(gameinfo::keys::kNamePreset))
            for (const auto& c : d->choices) {
                auto* a = menu.addAction(c.label);
                a->setCheckable(true);
                a->setChecked(c.value == current);
                a->setToolTip(c.description);
                group->addAction(a);
                const QString value = c.value;
                connect(a, &QAction::triggered, this, [this, value] {
                    (void)m_ctx.settings().setValue(gameinfo::keys::kNamePreset, value);
                    m_nameEdited = false;
                    renderFileName();
                });
            }
        menu.addSeparator();
        menu.addAction(QStringLiteral("Template and firmware\u2026"), this, [this] {
            emit navigateRequested({QStringLiteral("settings"), {{QStringLiteral("section"), QStringLiteral("Build")},
                                                                 {QStringLiteral("key"), gameinfo::keys::kNameTemplate}}});
        });
        menu.exec(m_nameMenuButton->mapToGlobal(QPoint(0, m_nameMenuButton->height())));
    });
}

void BuildImagePage::buildAdvanced()
{
    m_advanced = new Disclosure(QStringLiteral("Advanced options"), {}, this);
    m_advanced->setAccessibleName(QStringLiteral("Advanced options"));
    m_advanced->body()->setSpacing(0);
    m_advancedHost = new QWidget(m_advanced);
    auto* hostLayout = vbox(m_advancedHost, 0);
    for (auto& fo : m_formats)
        hostLayout->addWidget(fo.advanced);
    m_advanced->body()->addWidget(m_advancedHost);
    m_advanced->body()->addSpacing(space::sm);
    m_advanced->body()->addWidget(hairline(m_advanced));
    auto* row = new QWidget(m_advanced);
    auto* h = hbox(row, space::lg, QMargins(0, 8, 0, 6));
    auto* lab = label(QStringLiteral("Open the output folder when done"), TextStyle::Body, color::fg_1, row);
    h->addWidget(lab, 1);
    m_openFolder = new Switch(row);
    m_openFolder->setAccessibleName(lab->text());
    h->addWidget(m_openFolder);
    m_advanced->body()->addWidget(row);
    auto* lrow = new QWidget(m_advanced);
    auto* lh = hbox(lrow, 6, QMargins(0, space::sm, 0, 0));
    lh->addStretch(1);
    m_settingsLink = new Link(QStringLiteral("All build settings"), lrow);
    connect(m_settingsLink, &Link::activated, this, [this] {
        const QString t = tile();
        const QString section = t == jobs::kFormatFfpfsc ? QStringLiteral("PFS") : t == kFormatFpkg ? QStringLiteral("FPKG") : QStringLiteral("Build");
        emit navigateRequested({QStringLiteral("settings"), {{QStringLiteral("section"), section}}});
    });
    lh->addWidget(m_settingsLink);
    m_advanced->body()->addWidget(lrow);
    form()->addWidget(m_advanced);
}

void BuildImagePage::buildSummaryCard()
{
    // FPKG only, above the Jobs dock: size, free space and the live CPU limit.
    auto* summary = new Card(this);
    m_summaryCard = summary;
    summary->setTitle(QStringLiteral("Package build"));
    summary->body()->setSpacing(space::sm);
    auto addStat = [summary](const QString& name, QLabel*& value) {
        auto* row = new QWidget(summary);
        auto* h = hbox(row, space::md);
        h->addWidget(label(name, TextStyle::Body, color::fg_1, row), 1);
        value = monoLabel(QStringLiteral("\u2014"), row, color::fg_0);
        h->addWidget(value);
        summary->body()->addWidget(row);
    };
    addStat(QStringLiteral("Source size"), m_estSize);
    addStat(QStringLiteral("Free on output drive"), m_freeSpace);
    auto* row = new QWidget(summary);
    auto* h = hbox(row, space::md);
    auto* col = new QVBoxLayout;
    col->setSpacing(2);
    col->addWidget(label(QStringLiteral("CPU cores while packing"), TextStyle::Body, color::fg_1, row));
    m_liveNote = caption(QStringLiteral("Lower it any time, even while packing."), row);
    col->addWidget(m_liveNote);
    h->addLayout(col, 1);
    m_liveCores = dropdown(row);
    const int cores = std::max(1, QThread::idealThreadCount());
    m_liveCores->addItem(QStringLiteral("All (%1)").arg(cores), 0);
    for (int c = cores - 1; c >= 1; --c)
        m_liveCores->addItem(QStringLiteral("%1 of %2").arg(c).arg(cores), c);
    m_liveCores->setAccessibleName(QStringLiteral("CPU cores while packing"));
    h->addWidget(m_liveCores);
    summary->body()->addWidget(row);
    connect(m_liveCores, &QComboBox::currentIndexChanged, this, [this](int) { applyLiveCpuLimit(); });
    aside()->insertWidget(0, summary);
}

// ---- format ---------------------------------------------------------------------------

QString BuildImagePage::tile() const { return tilesOf(m_formatTiles)->currentValue(); }

QString BuildImagePage::format() const
{
    const QString t = tile();
    if (t == jobs::kFormatFfpfsc && m_pfsKind && m_pfsKind->currentValue().toString() == jobs::kFormatFfpfs)
        return jobs::kFormatFfpfs;
    return t;
}

void BuildImagePage::setFormat(const QString& f)
{
    if (f == jobs::kFormatFfpfs || f == jobs::kFormatFfpfsc) {
        m_pfsKind->setCurrentValue(f);
        tilesOf(m_formatTiles)->setCurrentValue(jobs::kFormatFfpfsc);
    } else if (f == QLatin1String("pfs")) {
        tilesOf(m_formatTiles)->setCurrentValue(jobs::kFormatFfpfsc);
    } else {
        tilesOf(m_formatTiles)->setCurrentValue(f);
    }
}

BuildImagePage::FormatOptions* BuildImagePage::current()
{
    const QString t = tile();
    for (auto& fo : m_formats)
        if (fo.tile == t)
            return &fo;
    return nullptr;
}

const BuildImagePage::FormatOptions* BuildImagePage::current() const
{
    return const_cast<BuildImagePage*>(this)->current();
}

JobOptionsPanel* BuildImagePage::optionsPanel(const QString& f) const
{
    const QString t = f == jobs::kFormatFfpfs ? jobs::kFormatFfpfsc : f;
    for (const auto& fo : m_formats)
        if (fo.tile == t)
            return fo.panel;
    return nullptr;
}

QStringList BuildImagePage::visibleOptionKeys() const
{
    QStringList out;
    const FormatOptions* fo = current();
    if (!fo)
        return out;
    for (const QString& key : fo->panel->keys())
        if (SettingRow* row = fo->panel->row(key); row && !row->isHidden())
            out << key;
    return out;
}

QStringList BuildImagePage::essentialOptionKeys() const
{
    const FormatOptions* fo = current();
    return fo ? fo->essentialKeys : QStringList();
}

void BuildImagePage::onFormatChanged()
{
    const QString t = tile();
    const bool pkg = t == kFormatFpkg;
    for (auto& fo : m_formats) {
        const bool on = fo.tile == t;
        fo.essentials->setVisible(on && (!fo.essentialKeys.isEmpty() || fo.tile == jobs::kFormatFfpfsc || fo.tile == kFormatFpkg));
        fo.advanced->setVisible(on);
    }
    m_packageCard->setVisible(pkg);
    m_summaryCard->setVisible(pkg);
    if (t == jobs::kFormatExfat)
        header()->setCaption(QStringLiteral("Turn a game dump into an exFAT image that ShadowMount+ and PCs can mount."));
    else if (t == jobs::kFormatFfpkg)
        header()->setCaption(QStringLiteral("Turn a game dump into an FFPKG (UFS2) image that ShadowMount+ mounts as it is."));
    else if (t == jobs::kFormatFfpfsc)
        header()->setCaption(QStringLiteral("Turn a game dump into a PFS image, compressed (.ffpfsc) or not (.ffpfs)."));
    else
        header()->setCaption(QStringLiteral("Package a game folder, an image or a .gp5 project as an installable .pkg."));
    primaryButton()->setText(pkg ? QStringLiteral("Build package") : QStringLiteral("Build image"));
    m_sourceCard->setTitleText(pkg ? QStringLiteral("Source") : QStringLiteral("Game dump"));
    m_drop->setTitle(pkg ? QStringLiteral("Drop a game folder, an image or a .gp5 project here") : QStringLiteral("Drop a game dump folder here"));
    m_drop->setText(pkg ? QStringLiteral("or click to browse \u2014 a folder, an .exfat, .ffpfsc or .ffpkg image, or a .gp5 project")
                        : QStringLiteral("or click to browse \u2014 the folder that holds eboot.bin and sce_sys"));
    if (t == jobs::kFormatExfat)
        m_advanced->setCaption(QStringLiteral("Free space, volume label, contents, verification"));
    else if (t == jobs::kFormatFfpkg)
        m_advanced->setCaption(QStringLiteral("Block and fragment size, inodes, verification"));
    else if (t == jobs::kFormatFfpfsc)
        m_advanced->setCaption(QStringLiteral("Threads, PFS format, source folder, verification"));
    else
        m_advanced->setCaption(QStringLiteral("Packing, param.json, files, PlayGo, verification"));
    m_settingsLink->setText(t == jobs::kFormatExfat ? QStringLiteral("All exFAT settings")
                            : t == jobs::kFormatFfpkg ? QStringLiteral("All FFPKG settings")
                            : t == jobs::kFormatFfpfsc ? QStringLiteral("All PFS settings")
                                                       : QStringLiteral("All FPKG settings"));
    m_fileLabel->setText(pkg ? QStringLiteral("Package name") : QStringLiteral("File name"));
    m_fileName->setReadOnly(pkg);
    m_fileExt->setVisible(!pkg);
    m_nameMenuButton->setVisible(!pkg);
    if (pkg)
        m_resetName->hide();
    else
        m_resetName->setVisible(m_nameEdited);
    setRequiredKinds({pkg ? QStringLiteral("fpkg.build") : jobs::imageBuildKind(format())});
    applyRowVisibility();
    updateAdvancedSummary();
    updateClusterNote();
    updatePfsRows();
    if (pkg) {
        updateToolsStatus();
        if (m_ctx.dependencies().last(QStringLiteral("sony.pubcmd")).id.isEmpty())
            m_ctx.checkDependenciesAsync();
    }
    // The source: images need a folder; FPKG also takes images and .gp5 projects.
    if (!m_sourcePath.isEmpty() && m_summary.exists)
        refreshSourceState();
    updateOutput();
}

void BuildImagePage::applyRowVisibility()
{
    const QString f = format();
    static const QStringList fileOnly = {pfs::keys::kCompressFiles, pfs::keys::kFileMinCompressMiB, pfs::keys::kFileBlockGain,
                                         pfs::keys::kFileMinGain, pfs::keys::kSkipExecutables};
    static const QStringList containerOnly = {pfs::keys::kContainerMinCompressMiB, pfs::keys::kContainerBlockGain};
    for (auto& fo : m_formats) {
        for (const QString& key : fo.panel->keys()) {
            SettingRow* row = fo.panel->row(key);
            if (!row)
                continue;
            bool hide = false;
            if (fo.tile == jobs::kFormatFfpfsc)
                hide = (f == jobs::kFormatFfpfsc && fileOnly.contains(key)) || (f == jobs::kFormatFfpfs && containerOnly.contains(key));
            row->setHidden(hide);
        }
        // Dividers between visible rows; a heading only above a bucket with rows.
        const auto divide = [](const QList<SettingRow*>& rows) {
            for (int i = 0; i < rows.size(); ++i)
                rows[i]->setDivider(i + 1 < rows.size());
        };
        QList<SettingRow*> ess;
        for (const QString& key : fo.essentialKeys)
            if (SettingRow* row = fo.panel->row(key); row && !row->isHidden())
                ess << row;
        divide(ess);
        QHash<QString, QList<SettingRow*>> buckets;
        for (const QString& key : fo.advancedKeys)
            if (SettingRow* row = fo.panel->row(key); row && !row->isHidden())
                buckets[fo.bucketOf.value(key)] << row;
        for (auto it = fo.headings.begin(); it != fo.headings.end(); ++it) {
            it.value()->setVisible(buckets.contains(it.key()));
            divide(buckets.value(it.key()));
        }
    }
}

void BuildImagePage::updateAdvancedSummary()
{
    const FormatOptions* fo = current();
    int n = 0;
    if (fo) {
        const QVariantMap ov = fo->panel->overrides();
        for (const QString& key : fo->advancedKeys)
            if (ov.contains(key))
                if (SettingRow* row = fo->panel->row(key); row && !row->isHidden())
                    ++n;
    }
    m_advanced->setSummary(n > 0 ? QStringLiteral("%1 changed").arg(n) : QStringLiteral("Defaults"), n > 0 ? Tone::Info : Tone::Neutral);
}

void BuildImagePage::updateClusterNote()
{
    // ShadowMount+ 1.7 and later mount exFAT only with 64 KiB clusters.
    JobOptionsPanel* panel = optionsPanel(jobs::kFormatExfat);
    SettingRow* row = panel ? panel->row(exfat::keys::kClusterSize) : nullptr;
    if (!row)
        return;
    const QString v = panel->effectiveValue(exfat::keys::kClusterSize).toString();
    if (v == QLatin1String("auto") || v == QLatin1String("65536") || v.isEmpty())
        row->setMessage({}, Tone::Neutral);
    else
        row->setMessage(QStringLiteral("ShadowMount+ 1.7 and later need 64 KB."), Tone::Warn);
}

void BuildImagePage::updatePfsRows()
{
    JobOptionsPanel* panel = optionsPanel(jobs::kFormatFfpfsc);
    SettingRow* level = panel ? panel->row(pfs::keys::kCompressionLevel) : nullptr;
    if (!level)
        return;
    const bool folderImage = format() == jobs::kFormatFfpfs;
    const bool compressFiles = panel->effectiveValue(pfs::keys::kCompressFiles).toBool();
    level->setDisabledReason(folderImage && !compressFiles ? QStringLiteral("Only used when files inside the .ffpfs are compressed.") : QString());
}

void BuildImagePage::updateToolsStatus()
{
    if (!m_toolsBadge)
        return;
    const DependencyInfo info = m_ctx.dependencies().last(QStringLiteral("sony.pubcmd"));
    m_toolsDetail->show();
    if (info.id.isEmpty()) {
        m_toolsBadge->set(QStringLiteral("Checking\u2026"), Tone::Neutral);
        m_toolsBadge->setIconName(QStringLiteral("loader"));
        m_toolsDetail->setText(QStringLiteral("Your own prospero-pub-cmd.exe builds the package."));
        m_toolsLink->setText(QStringLiteral("Set up\u2026"));
    } else if (info.status == DependencyStatus::Ready) {
        m_toolsBadge->set(info.version.isEmpty() ? QStringLiteral("Found") : QStringLiteral("Found \u00b7 %1").arg(info.version), Tone::Ok);
        m_toolsBadge->setIconName(defaultBadgeIcon(Tone::Ok));
        m_toolsDetail->setText(info.path.isEmpty() ? QStringLiteral("Your own prospero-pub-cmd.exe builds the package.") : nativePath(info.path));
        m_toolsLink->setText(QStringLiteral("Change\u2026"));
    } else {
        const bool notSet = info.status == DependencyStatus::Missing;
        m_toolsBadge->set(notSet ? QStringLiteral("Not set") : dependencyStatusName(info.status), Tone::Warn);
        m_toolsBadge->setIconName(defaultBadgeIcon(Tone::Warn));
        m_toolsDetail->setText(notSet ? QStringLiteral("Needed to build packages. Nothing from Sony is included.")
                                      : (info.detail.isEmpty() ? info.howToFix : info.detail));
        m_toolsLink->setText(QStringLiteral("Set up\u2026"));
    }
}

// ---- source -----------------------------------------------------------------------

QString BuildImagePage::source() const { return m_sourcePath; }

QString BuildImagePage::sourceKind() const { return sourceKindOf(m_sourcePath); }

void BuildImagePage::setDragHighlight(bool on)
{
    JobFormPage::setDragHighlight(on);
    m_drop->setHighlighted(on);
}

bool BuildImagePage::acceptsDrop(const QList<QUrl>& urls) const
{
    if (urls.isEmpty())
        return false;
    const QFileInfo fi(urls.first().toLocalFile());
    if (fi.isDir())
        return true;
    static const QStringList exts = {QStringLiteral("exfat"), QStringLiteral("ffpfsc"), QStringLiteral("ffpfs"),
                                     QStringLiteral("ffpkg"), QStringLiteral("gp5")};
    if (tile() != kFormatFpkg && fi.isFile() && fi.suffix().compare(QLatin1String("zip"), Qt::CaseInsensitive) == 0)
        return true; // a zipped dump
    return tile() == kFormatFpkg && fi.isFile() && exts.contains(fi.suffix().toLower());
}

void BuildImagePage::dropUrls(const QList<QUrl>& urls)
{
    if (acceptsDrop(urls))
        setSource(urls.first().toLocalFile());
}

void BuildImagePage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("format")))
        setFormat(args.value(QStringLiteral("format")).toString());
    if (args.contains(QStringLiteral("source"))) {
        const QString src = args.value(QStringLiteral("source")).toString();
        // Images and .gp5 projects can only be packaged.
        if (!src.isEmpty() && QFileInfo(src).isFile())
            setFormat(kFormatFpkg);
        setSource(src);
    }
}

void BuildImagePage::onShown()
{
    if (isFpkg())
        updateToolsStatus();
}

void BuildImagePage::refresh()
{
    if (!m_sourcePath.isEmpty())
        setSource(m_sourcePath);
    if (isFpkg())
        m_ctx.checkDependenciesAsync();
}

void BuildImagePage::browseSource()
{
    const QString start = m_ctx.settings().value(general::keys::kLastSourceDir).toString();
    QString picked;
    if (!isFpkg()) {
        QMenu menu(this);
        auto* folder = menu.addAction(iconPixmap(QStringLiteral("folder"), 16, color::fg_2, devicePixelRatioF()), QStringLiteral("Game folder\u2026"));
        auto* zip = menu.addAction(iconPixmap(QStringLiteral("archive"), 16, color::fg_2, devicePixelRatioF()), QStringLiteral("Zipped dump (.zip)\u2026"));
        QAction* chosen = menu.exec(QCursor::pos());
        if (chosen == folder)
            picked = QFileDialog::getExistingDirectory(window(), QStringLiteral("Choose the game dump folder"), start);
        else if (chosen == zip)
            picked = QFileDialog::getOpenFileName(window(), QStringLiteral("Choose the zipped dump"), start,
                                                  QStringLiteral("ZIP archives (*.zip)"));
    } else {
        QMenu menu(this);
        auto* folder = menu.addAction(iconPixmap(QStringLiteral("folder"), 16, color::fg_2, devicePixelRatioF()), QStringLiteral("Game folder\u2026"));
        auto* file = menu.addAction(iconPixmap(QStringLiteral("file"), 16, color::fg_2, devicePixelRatioF()), QStringLiteral("Image or .gp5 project\u2026"));
        QAction* chosen = menu.exec(QCursor::pos());
        if (chosen == folder)
            picked = QFileDialog::getExistingDirectory(window(), QStringLiteral("Choose the game folder"), start);
        else if (chosen == file)
            picked = QFileDialog::getOpenFileName(window(), QStringLiteral("Choose the image or project"), start,
                                                  QStringLiteral("Images and projects (*.exfat *.ffpfsc *.ffpfs *.ffpkg *.gp5)"));
    }
    if (!picked.isEmpty())
        setSource(picked);
}

void BuildImagePage::setSource(const QString& path)
{
    m_sourcePath = QDir::fromNativeSeparators(path);
    clearFormError();
    m_sourceMsg->clear();
    m_rootsBox->hide();
    m_rootChoices.clear();
    m_pendingRootNote.clear();
    if (path.isEmpty()) {
        m_reader->cancel();
        m_strip->hide();
        m_drop->show();
        m_sourceBadge->set(QStringLiteral("Step 1"), Tone::Neutral);
        m_sourceBadge->setIconName(QString());
        m_summary = {};
        m_nameEdited = false;
        updateOutput();
        renderFileName();
        emit statusContextChanged();
        return;
    }
    // A new source gets its own name again (#16).
    m_nameEdited = false;
    const QFileInfo fi(path);
    if (fi.isDir()) {
        m_sourceBadge->set(QStringLiteral("Scanning\u2026"), Tone::Info);
        m_sourceBadge->setIconName(QStringLiteral("loader"));
        m_summary = {}; // nothing stale while the new folder is read
        renderFileName();
        m_reader->summarize(m_sourcePath, true);
        (void)m_ctx.settings().setValue(general::keys::kLastSourceDir, fi.absolutePath());
        return;
    }
    m_reader->cancel();
    GameSummary s;
    s.folder = m_sourcePath;
    s.exists = fi.exists();
    s.isGame = fi.exists();
    s.bytes = fi.size();
    s.title = fi.completeBaseName();
    static const QRegularExpression re(QStringLiteral("(CUSA|PPSA|PPLH)[-_ ]?(\\d{5})"), QRegularExpression::CaseInsensitiveOption);
    if (const auto m = re.match(fi.fileName()); m.hasMatch())
        s.titleId = (m.captured(1) + m.captured(2)).toUpper();
    if (fi.exists())
        (void)m_ctx.settings().setValue(general::keys::kLastSourceDir, fi.absolutePath());
    onSummary(s);
}

void BuildImagePage::onSummary(const GameSummary& s)
{
    // A wrapper folder holding exactly one game: use that game (#84, #53).
    if (s.exists && !s.isGame && s.gameRoots.size() == 1 && !m_demo) {
        const QString root = s.gameRoots.first();
        const QString note = QDir::toNativeSeparators(QDir(s.folder).relativeFilePath(root));
        setSource(root);
        m_pendingRootNote = note; // shown once the game folder is read
        return;
    }
    m_summary = s;
    refreshSourceState();
    // Package details come from param.json (or the file name for images).
    if (s.exists && m_contentId) {
        if (!s.contentId.isEmpty())
            m_contentId->setText(s.contentId);
        if (!s.title.isEmpty())
            m_title->setText(s.title);
        if (!s.version.isEmpty())
            m_version->setText(s.version);
    }
    if (m_estSize)
        m_estSize->setText(s.bytes > 0 ? formatBytes(s.bytes) : QStringLiteral("—"));
    renderFileName();
    updateOutput();
    emit statusContextChanged();
}

void BuildImagePage::refreshSourceState()
{
    const GameSummary& s = m_summary;
    if (m_sourcePath.isEmpty())
        return;
    const QString kind = sourceKind();
    const bool file = kind != QLatin1String("folder") && !kind.isEmpty();
    m_stripPath->setFullText(QDir::toNativeSeparators(s.folder.isEmpty() ? m_sourcePath : s.folder));
    m_rootsBox->hide();
    m_rootChoices.clear();
    if (!s.exists) {
        m_strip->hide();
        m_drop->show();
        m_sourceMsg->set(file ? QStringLiteral("This file does not exist. Choose another or check the drive.")
                              : QStringLiteral("This folder does not exist. Choose another or check the drive."), Tone::Err);
        m_sourceBadge->set(QStringLiteral("Not found"), Tone::Err);
        m_sourceBadge->setIconName(defaultBadgeIcon(Tone::Err));
    } else {
        m_drop->hide();
        m_strip->show();
        m_stripTitle->setFullText(s.title);
        if (file) {
            m_stripTile->setIconName(kind == QLatin1String("zip") ? QStringLiteral("archive")
                                     : kind == QLatin1String("gp5") ? QStringLiteral("file_cog")
                                     : kind == QLatin1String("ffpfsc") ? QStringLiteral("compress")
                                     : kind == QLatin1String("ffpkg") ? QStringLiteral("package") : QStringLiteral("drive"));
            QStringList meta{sourceKindLabel(kind)};
            if (!s.titleId.isEmpty())
                meta << s.titleId;
            if (s.bytes > 0)
                meta << formatBytes(s.bytes);
            m_stripMeta->setFullText(meta.join(QStringLiteral(" \u00b7 ")));
        } else {
            const QString line = s.metaLine();
            if (s.isGame) {
                m_stripTile->setIconName(QStringLiteral("game"));
                m_stripMeta->setFullText(line.isEmpty() ? sourceKindLabel(kind) : sourceKindLabel(kind) + QStringLiteral(" \u00b7 ") + line);
            } else {
                m_stripTile->setIconName(QStringLiteral("folder"));
                m_stripMeta->setFullText(s.gameRoots.size() > 1 ? QStringLiteral("Folder \u00b7 %1 games inside").arg(s.gameRoots.size())
                                                                : QStringLiteral("Folder without a game"));
            }
        }
        if (kind == QLatin1String("zip") && isFpkg()) {
            m_stripTile->setTone(Tone::Warn);
            m_sourceMsg->set(QStringLiteral("FPKG packages are made from a folder, an image or a .gp5 project. Unpack the ZIP, or pick exFAT, FFPKG or PFS."), Tone::Warn);
            m_sourceBadge->set(QStringLiteral("Needs a folder"), Tone::Warn);
            m_sourceBadge->setIconName(defaultBadgeIcon(Tone::Warn));
        } else if (kind == QLatin1String("zip") && (!s.isGame || !s.error.isEmpty())) {
            m_stripTile->setTone(Tone::Warn);
            m_sourceMsg->set(s.error.isEmpty() ? QStringLiteral("This ZIP holds no game (no eboot.bin or sce_sys/param.json).") : s.error, Tone::Warn);
            m_sourceBadge->set(QStringLiteral("Not a game"), Tone::Warn);
            m_sourceBadge->setIconName(defaultBadgeIcon(Tone::Warn));
        } else if (kind == QLatin1String("zip")) {
            m_stripTile->setTone(Tone::Ok);
            m_sourceMsg->set(QStringLiteral("The ZIP is unpacked to a temporary folder when the build starts (it needs %1 of free space there).")
                                 .arg(formatBytes(s.bytes)),
                             Tone::Neutral);
            m_sourceBadge->set(QStringLiteral("Ready"), Tone::Ok);
            m_sourceBadge->setIconName(defaultBadgeIcon(Tone::Ok));
        } else if (file && !isFpkg()) {
            m_stripTile->setTone(Tone::Warn);
            m_sourceMsg->set(QStringLiteral("Only FPKG can use an image or a .gp5 project. Choose the game folder for this format."), Tone::Warn);
            m_sourceBadge->set(QStringLiteral("Needs a folder"), Tone::Warn);
            m_sourceBadge->setIconName(defaultBadgeIcon(Tone::Warn));
        } else if (!s.isGame) {
            m_stripTile->setTone(Tone::Warn);
            showGameRoots(s);
        } else {
            m_stripTile->setTone(Tone::Ok);
            if (!m_pendingRootNote.isEmpty())
                m_sourceMsg->set(QStringLiteral("Found the game in %1.").arg(m_pendingRootNote), Tone::Info);
            else
                m_sourceMsg->clear();
            m_sourceBadge->set(QStringLiteral("Ready"), Tone::Ok);
            m_sourceBadge->setIconName(defaultBadgeIcon(Tone::Ok));
        }
    }
}

void BuildImagePage::showGameRoots(const GameSummary& s)
{
    while (QLayoutItem* it = m_rootsList->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    m_rootChoices = s.gameRoots;
    if (s.gameRoots.isEmpty()) {
        QString text = QStringLiteral("No game here. Choose the folder that holds eboot.bin and sce_sys.");
        if (s.rootsTruncated)
            text = QStringLiteral("No game found in the first 4,000 folders. Choose the folder that holds eboot.bin and sce_sys.");
        m_sourceMsg->set(text, Tone::Warn);
        m_sourceBadge->set(QStringLiteral("Not a game dump"), Tone::Warn);
        m_sourceBadge->setIconName(defaultBadgeIcon(Tone::Warn));
        return;
    }
    m_sourceMsg->set(QStringLiteral("This folder holds %1 games. Choose one:").arg(s.gameRoots.size()), Tone::Info);
    m_sourceBadge->set(QStringLiteral("Choose a game"), Tone::Warn);
    m_sourceBadge->setIconName(defaultBadgeIcon(Tone::Warn));
    const int shown = std::min<int>(int(s.gameRoots.size()), 8);
    for (int i = 0; i < shown; ++i) {
        const QString root = s.gameRoots[i];
        auto* pick = button(QDir::toNativeSeparators(QDir(s.folder).relativeFilePath(root)), ButtonVariant::Subtle,
                            QStringLiteral("game"), m_rootsBox);
        setButtonSmall(pick, true);
        pick->setAccessibleName(QStringLiteral("Use %1").arg(QFileInfo(root).fileName()));
        pick->setToolTip(nativePath(root));
        connect(pick, &QPushButton::clicked, this, [this, root] { setSource(root); });
        m_rootsList->addWidget(pick, 0, Qt::AlignLeft);
    }
    if (s.gameRoots.size() > shown || s.rootsTruncated)
        m_rootsList->addWidget(caption(s.rootsTruncated ? QStringLiteral("The search stopped after 4,000 folders.")
                                                        : QStringLiteral("and %1 more").arg(s.gameRoots.size() - shown), m_rootsBox));
    m_rootsBox->show();
}

QString BuildImagePage::sourceMessage() const { return m_sourceMsg->text(); }

// ---- package (FPKG) ---------------------------------------------------------------

void BuildImagePage::onPackageTypeChanged()
{
    const QString t = m_packageType->currentValue().toString();
    m_baseField->setVisible(t == QLatin1String("update"));
    m_addonField->setVisible(t == QLatin1String("addon"));
    renderFileName();
}

void BuildImagePage::pickFromDlcEmu()
{
    QString game = m_summary.exists && QFileInfo(m_summary.folder).isDir() ? m_summary.folder : QString();
    if (game.isEmpty() || !QFile::exists(QDir(game).filePath(QStringLiteral("dlc_emu.ini"))))
        game = QFileDialog::getExistingDirectory(window(), QStringLiteral("Choose the game folder that holds dlc_emu.ini"));
    if (game.isEmpty())
        return;
    const auto candidates = fpkg::addonsFromDlcEmu(game);
    if (candidates.empty()) {
        Sheet::message(window(), QStringLiteral("No add-ons found"),
                       QStringLiteral("%1 has no dlc_emu.ini entries with a content ID.").arg(Sheet::monoSpan(nativePath(game))), Sheet::Icon::Info);
        return;
    }
    auto* s = new Sheet(window(), QStringLiteral("Add-ons in dlc_emu.ini"), Sheet::Icon::None, size::dialogWide);
    auto* combo = dropdown(s);
    for (const auto& c : candidates)
        combo->addItem(QStringLiteral("%1 \u00b7 %2%3")
                           .arg(c.contentId, c.payloadFolder.isEmpty() ? QStringLiteral("no local payload folder") : nativePath(c.payloadFolder),
                                c.supported ? QString() : QStringLiteral(" \u00b7 ") + c.reason),
                       int(&c - &candidates[0]));
    combo->setMinimumWidth(520);
    s->body()->addWidget(field(QStringLiteral("Add-on"), combo, QStringLiteral("Only entries with a payload folder and a supported volume type can be packaged."), s));
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Use this add-on"), Sheet::Role::Primary);
    if (s->exec() != 1)
        return;
    const auto& c = candidates[size_t(combo->currentData().toInt())];
    m_contentId->setText(c.contentId);
    m_entitlementKey->setText(c.entitlementKey);
    if (!c.payloadFolder.isEmpty())
        setSource(c.payloadFolder);
    if (!c.supported)
        showFormError(QStringLiteral("This add-on cannot be packaged"), c.reason);
}

QString BuildImagePage::expectedPackagePath() const
{
    const QString cid = m_contentId->text().trimmed();
    const auto ver = fpkg::ContentVersion::parse(m_version->text().trimmed().isEmpty() ? QStringLiteral("01.000.000") : m_version->text().trimmed());
    if (!fpkg::isValidContentId(cid) || !ver)
        return {};
    const QString stem = fpkg::packageStem(cid, *ver);
    const QString name = m_packageType->currentValue().toString() == QLatin1String("update") ? fpkg::updatePackageName(stem) : fpkg::fullPackageName(stem);
    const QString dir = outputFolder();
    return dir.isEmpty() ? name : QDir(dir).filePath(name);
}

void BuildImagePage::applyLiveCpuLimit()
{
    const int cores = m_liveCores->currentData().toInt();
    int applied = 0;
    for (const auto& rec : m_ctx.queue().records())
        if (rec.state == JobState::Running && (rec.kind == QLatin1String("fpkg.build") || rec.kind == QLatin1String("fpkg.addon"))) {
            fpkg::setLiveCpuLimit(rec.id, cores);
            ++applied;
        }
    m_liveNote->setText(applied > 0 ? QStringLiteral("Applied to %1 running package build%2.").arg(applied).arg(applied == 1 ? QString() : QStringLiteral("s"))
                                    : QStringLiteral("Lower it any time, even while packing."));
}

// ---- output -----------------------------------------------------------------------

QString BuildImagePage::outputFolder() const
{
    if (!m_output->path().isEmpty())
        return m_output->path();
    const QString def = m_ctx.settings().value(general::keys::kDefaultOutputDir).toString();
    if (isFpkg())
        return m_summary.exists ? fpkg::defaultOutputFolder(m_summary.folder, def) : def;
    if (!def.isEmpty())
        return def;
    return m_summary.exists ? QFileInfo(m_summary.folder).absolutePath() : QString();
}

void BuildImagePage::setOutputFolder(const QString& dir) { m_output->setPath(dir); }

void BuildImagePage::setFileStem(const QString& stem)
{
    m_fileName->setText(stem);
    emit m_fileName->textEdited(stem);
}

QString BuildImagePage::fileName() const
{
    if (isFpkg())
        return QFileInfo(expectedPackagePath()).fileName();
    const QString stem = m_fileName->text().trimmed();
    return stem.isEmpty() ? QString() : stem + jobs::outputExtension(format());
}

void BuildImagePage::renderFileName()
{
    const QString ext = jobs::outputExtension(format());
    m_fileExt->setText(ext);
    if (isFpkg()) {
        const QString name = QFileInfo(expectedPackagePath()).fileName();
        m_fileName->setText(name);
        m_fileName->setPlaceholderText(QStringLiteral("Named from the content ID"));
        m_fileName->setCursorPosition(0);
        setInputError(m_fileName, false);
        return;
    }
    m_fileName->setPlaceholderText(m_summary.exists ? QString() : QStringLiteral("From the game's title and version"));
    if (m_nameEdited) {
        m_resetName->show();
        return;
    }
    m_resetName->hide();
    setInputError(m_fileName, false);
    if (!m_summary.exists || (sourceKind() != QLatin1String("folder") && sourceKind() != QLatin1String("zip"))) {
        m_fileName->clear();
        return;
    }
    const auto& st = m_ctx.settings();
    const auto preset = gameinfo::imageNamePresetFromKey(st.value(gameinfo::keys::kNamePreset).toString())
                            .value_or(gameinfo::ImageNamePreset::TitleIdTitleVersion);
    gameinfo::ImageNameFields f;
    f.titleId = m_summary.titleId;
    f.title = m_summary.title == QFileInfo(m_summary.folder).fileName() && !m_summary.isGame ? QString() : m_summary.title;
    f.version = m_summary.version;
    const QString fwOverride = st.value(gameinfo::keys::kFirmwareOverride).toString();
    f.firmware = fwOverride.isEmpty() || fwOverride == QLatin1String("auto") ? gameinfo::firmwareMajor(m_summary.requiredFw) : fwOverride;
    f.sdk = m_summary.sdk;
    f.type = typeLabel(tile());
    f.folderName = sourceKind() == QLatin1String("zip") ? QFileInfo(m_summary.folder).completeBaseName() : QFileInfo(m_summary.folder).fileName();
    m_fileName->setText(gameinfo::renderImageName(preset, st.value(gameinfo::keys::kNameTemplate).toString(), f));
    m_fileName->setCursorPosition(0);
}

qint64 BuildImagePage::neededBytes() const
{
    if (m_summary.bytes <= 0)
        return 0;
    const QString f = format();
    if (f == jobs::kFormatFfpfsc || f == kFormatFpkg)
        return m_summary.bytes; // compressed: never more than the source
    return qint64(double(m_summary.bytes) * 1.006) + 64LL * 1024 * 1024;
}

void BuildImagePage::updateOutput()
{
    if (!m_nameEdited || isFpkg())
        renderFileName();
    else
        m_fileExt->setText(jobs::outputExtension(format()));
    QString placeholder;
    const QString def = m_ctx.settings().value(general::keys::kDefaultOutputDir).toString();
    if (isFpkg())
        placeholder = m_summary.exists ? nativePath(fpkg::defaultOutputFolder(m_summary.folder, def))
                                       : (def.isEmpty() ? QStringLiteral("Next to the source, in a \u2026-pkg folder") : nativePath(def));
    else
        placeholder = def.isEmpty() ? QStringLiteral("Next to the source folder") : nativePath(def);
    m_output->setPlaceholder(placeholder);
    checkSpace();
}

void BuildImagePage::checkSpace()
{
    QString folder = outputFolder();
    m_spaceBar->hide();
    m_spaceOk = true;
    m_spaceProblem.clear();
    if (m_demo) {
        ++m_spaceGen; // drop results of checks still running
        m_spaceRow->set(isFpkg() ? QStringLiteral("412 GB free on D:. The package needs up to 31.6 GB.")
                                 : QStringLiteral("412 GB free on D:. This image needs about 31.8 GB."), Tone::Ok);
        if (m_freeSpace)
            m_freeSpace->setText(QStringLiteral("412 GB"));
        return;
    }
    if (folder.isEmpty()) {
        m_spaceRow->clear();
        if (m_freeSpace)
            m_freeSpace->setText(QStringLiteral("\u2014"));
        return;
    }
    const qint64 needed = neededBytes();
    const bool pkg = isFpkg();
    const quint64 gen = ++m_spaceGen;
    QPointer<BuildImagePage> self(this);
    (void)QtConcurrent::run([self, folder, needed, gen, pkg] {
        // A folder created by the build: measure the drive it will be on.
        QString probe = folder;
        while (!probe.isEmpty() && !QFileInfo::exists(probe))
            probe = QFileInfo(probe).absolutePath() == probe ? QString() : QFileInfo(probe).absolutePath();
        const bool exists = QFileInfo::exists(folder);
        auto ds = diskSpace(probe.isEmpty() ? folder : probe);
        auto vol = checkOutputVolume(probe.isEmpty() ? folder : probe, needed);
        if (!self)
            return;
        QMetaObject::invokeMethod(self.data(), [self, ds, vol, folder, needed, gen, pkg, exists] {
            if (!self || gen != self->m_spaceGen)
                return;
            self->m_output->setError({});
            if (!ds.ok()) {
                self->m_spaceRow->set(QStringLiteral("The output drive is not available."), Tone::Warn);
                if (self->m_freeSpace)
                    self->m_freeSpace->setText(QStringLiteral("\u2014"));
                return;
            }
            if (self->m_freeSpace)
                self->m_freeSpace->setText(formatBytes(ds->available));
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
            QString text = QStringLiteral("%1 free on %2.").arg(formatBytes(ds->available), drive);
            if (needed > 0)
                text += pkg ? QStringLiteral(" The package needs up to %1.").arg(formatBytes(needed))
                            : QStringLiteral(" This image needs about %1.").arg(formatBytes(needed));
            if (!exists)
                text += QStringLiteral(" The folder is created when the build starts.");
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
    m_spaceBar->setMessage(QStringLiteral("This %1 needs %2; %3 is free. Free %4 or choose another output folder.")
                               .arg(isFpkg() ? QStringLiteral("package") : QStringLiteral("image"), formatBytes(needed), formatBytes(freeBytes),
                                    formatBytes(std::max<qint64>(0, needed - freeBytes))));
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

// ---- validation and submit ----------------------------------------------------------------

QString BuildImagePage::validateForm()
{
    const FormatOptions* fo = current();
    if (m_sourcePath.isEmpty())
        return isFpkg() ? QStringLiteral("Choose the source first.") : QStringLiteral("Choose the game dump folder first.");
    if (m_reader->isBusy())
        return QStringLiteral("The source is still being read. Try again in a moment.");
    if (!m_summary.exists)
        return QStringLiteral("The source does not exist.");
    const QString kind = sourceKind();
    if (!isFpkg()) {
        if (kind == QLatin1String("zip")) {
            if (!m_summary.error.isEmpty())
                return m_summary.error;
            if (!m_summary.isGame)
                return QStringLiteral("This ZIP holds no game (no eboot.bin or sce_sys/param.json).");
        } else if (kind != QLatin1String("folder"))
            return QStringLiteral("exFAT, FFPKG and PFS images are built from a game folder or a zipped dump. Choose the dump, or choose FPKG to package this file.");
        if (kind == QLatin1String("folder") && !m_summary.isGame)
            return m_summary.gameRoots.size() > 1 ? QStringLiteral("This folder holds several games. Choose one of them.")
                                                  : QStringLiteral("The source folder is not a game dump: sce_sys\\param.json or eboot.bin is missing.");
        const QString stem = m_fileName->text().trimmed();
        if (stem.isEmpty())
            return QStringLiteral("Enter a file name.");
        if (const QString why = gameinfo::folderNameProblem(fileName()); !why.isEmpty()) {
            setInputError(m_fileName, true);
            return QStringLiteral("The file name cannot be used: %1").arg(why);
        }
    } else {
        if (kind == QLatin1String("zip"))
            return QStringLiteral("FPKG packages are made from a folder, an image or a .gp5 project. Unpack the ZIP first.");
        if (kind == QLatin1String("folder") && !m_summary.isGame)
            return m_summary.gameRoots.size() > 1 ? QStringLiteral("This folder holds several games. Choose one of them.")
                                                  : QStringLiteral("The folder is not a game dump: sce_sys\\param.json is missing.");
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
        const QString type = m_packageType->currentValue().toString();
        if (type == QLatin1String("update") && m_basePackage->path().isEmpty())
            return QStringLiteral("An update needs the base package that is installed on the console.");
        if (type == QLatin1String("addon")) {
            static const QRegularExpression hex32(QStringLiteral("^[0-9A-Fa-f]{32}$"));
            setInputError(m_entitlementKey, !hex32.match(m_entitlementKey->text().trimmed()).hasMatch());
            if (!hex32.match(m_entitlementKey->text().trimmed()).hasMatch())
                return QStringLiteral("The entitlement key must be 32 hexadecimal characters.");
            if (c.isEmpty())
                return QStringLiteral("An add-on needs its Content ID.");
        }
    }
    if (!m_spaceOk)
        return m_spaceProblem;
    if (fo && fo->panel->hasErrors())
        return fo->panel->errorMessages().join(u' ');
    return {};
}

Result<QString> BuildImagePage::submit(bool)
{
    const FormatOptions* fo = current();
    QVariantMap overrides = fo ? fo->panel->overrides() : QVariantMap();
    Result<QString> r = Error(ErrorCode::Internal, QStringLiteral("Nothing to build."));
    if (!isFpkg()) {
        QString out = m_output->path();
        if (out.isEmpty())
            out = outputFolder();
        QJsonObject extra{{QStringLiteral("title"), m_summary.title}, {QStringLiteral("titleId"), m_summary.titleId},
                          {QStringLiteral("version"), m_summary.version}, {QStringLiteral("sourceBytes"), m_summary.bytes}};
        r = jobs::submitImageBuild(m_ctx, format(), m_summary.folder, out, fileName(), overrides, extra);
    } else {
        QJsonObject details;
        if (!m_contentId->text().trimmed().isEmpty())
            details.insert(QStringLiteral("contentId"), m_contentId->text().trimmed());
        if (!m_title->text().trimmed().isEmpty())
            details.insert(QStringLiteral("title"), m_title->text().trimmed());
        if (!m_version->text().trimmed().isEmpty())
            details.insert(QStringLiteral("contentVersion"), m_version->text().trimmed());
        const QString type = m_packageType->currentValue().toString();
        details.insert(QStringLiteral("packageType"), type);
        if (type == QLatin1String("update"))
            details.insert(QStringLiteral("basePackage"), m_basePackage->path());
        if (type == QLatin1String("addon")) {
            details.insert(QStringLiteral("entitlementKey"), m_entitlementKey->text().trimmed());
            details.insert(QStringLiteral("volumeType"), m_volumeType->currentData().toString());
        }
        details.insert(QStringLiteral("sourceBytes"), m_summary.bytes);
        // An existing package with the expected name: ask now, since a queued job cannot.
        const QString expected = expectedPackagePath();
        if (!expected.isEmpty() && QFileInfo::exists(expected) && m_ctx.schema().contains(fpkg::keys::kOutputConflict)) {
            auto* s = new Sheet(window(), QStringLiteral("A package with this name exists"), Sheet::Icon::Warning);
            s->addText(QStringLiteral("%1 is already in the output folder. Keep both renames the existing file to \"name (1).pkg\"; overwrite sets it aside and restores it if the build fails.")
                           .arg(Sheet::monoSpan(nativePath(expected))));
            s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
            s->addButton(QStringLiteral("Keep both"), Sheet::Role::Primary);
            s->addButton(QStringLiteral("Overwrite"), Sheet::Role::Destructive);
            const int res = s->exec();
            if (res != 1 && res != 2)
                return Error(ErrorCode::Cancelled, QStringLiteral("Not started."));
            overrides.insert(fpkg::keys::kOutputConflict, res == 1 ? QStringLiteral("keepBoth") : QStringLiteral("overwrite"));
        }
        r = jobs::submitFpkgBuild(m_ctx, sourceKind(), m_summary.folder, m_output->path(), details, overrides);
    }
    // JobFormPage reports "open the folder when done" from its own switch,
    // which this page does not use; report ours for the new job.
    if (r.ok() && m_openFolder->isChecked())
        emit jobSubmitted(r.value(), true);
    return r;
}

QString BuildImagePage::statusContext() const
{
    if (!m_summary.exists)
        return {};
    if (sourceKind() != QLatin1String("folder"))
        return QStringLiteral("%1 \u00b7 %2").arg(m_summary.title, formatBytes(m_summary.bytes));
    return m_summary.isGame ? QStringLiteral("%1 \u00b7 %2").arg(m_summary.title, formatBytes(m_summary.bytes)) : QString();
}

// ---- screenshots ------------------------------------------------------------------------

QStringList BuildImagePage::demoStates() const
{
    return {QStringLiteral("space-error"), QStringLiteral("format-exfat"), QStringLiteral("format-ffpkg"), QStringLiteral("format-pfs"),
            QStringLiteral("format-fpkg"), QStringLiteral("advanced-exfat"), QStringLiteral("advanced-fpkg"), QStringLiteral("several-games")};
}

void BuildImagePage::applyDemoState(const QString& name)
{
    if (name == QLatin1String("space-error")) {
        showSpaceError(48500000000LL, 34100000000LL, QStringLiteral("C:"));
        return;
    }
    m_demo = true;
    GameSummary s;
    s.folder = QStringLiteral("D:/PS5/Dumps/PPSA01342 Demon's Souls");
    s.title = QStringLiteral("Demon's Souls");
    s.titleId = QStringLiteral("PPSA01342");
    s.contentId = QStringLiteral("UP9000-PPSA01342_00-DEMONSSOULS00000");
    s.version = QStringLiteral("01.004.000");
    s.sdk = QStringLiteral("2.00");
    s.requiredFw = QStringLiteral("2.00");
    s.bytes = 31600000000LL;
    s.files = 18442;
    s.isGame = true;
    s.exists = true;
    QString fmt = jobs::kFormatExfat;
    if (name.endsWith(QLatin1String("ffpkg")))
        fmt = jobs::kFormatFfpkg;
    else if (name.endsWith(QLatin1String("pfs")))
        fmt = jobs::kFormatFfpfsc;
    else if (name.endsWith(QLatin1String("fpkg")))
        fmt = kFormatFpkg;
    setFormat(fmt);
    if (name == QLatin1String("several-games")) {
        s.folder = QStringLiteral("D:/PS5/Dumps");
        s.title = QStringLiteral("Dumps");
        s.titleId.clear();
        s.version.clear();
        s.sdk.clear();
        s.isGame = false;
        s.bytes = 0;
        s.files = 0;
        s.gameRoots = {QStringLiteral("D:/PS5/Dumps/PPSA01342 Demon's Souls"), QStringLiteral("D:/PS5/Dumps/PPSA03016 Returnal"),
                       QStringLiteral("D:/PS5/Dumps/Spider-Man 2/PPSA03420-app")};
    }
    m_sourcePath = s.folder;
    onSummary(s);
    if (name.startsWith(QLatin1String("advanced-"))) {
        m_advanced->setExpanded(true);
        QTimer::singleShot(0, this, [this] {
            scrollArea()->verticalScrollBar()->setValue(m_advanced->mapTo(scrollArea()->widget(), QPoint(0, 0)).y() - space::xl);
        });
    }
}

void BuildImagePage::clearDemoState()
{
    m_spaceBar->hide();
    m_output->setError({});
    m_spaceOk = true;
    if (m_demo) {
        m_demo = false;
        m_advanced->setExpanded(false);
        setFormat(jobs::kFormatExfat);
        m_pfsKind->setCurrentValue(jobs::kFormatFfpfsc);
        setSource({});
        scrollArea()->verticalScrollBar()->setValue(0);
    }
    JobFormPage::clearDemoState();
}

} // namespace fw::pages
