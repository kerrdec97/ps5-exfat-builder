#include "app/pages/BackportPage.h"

#include "app/AppContext.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Tr.h"
#include "engines/fpkg/BackportUpdate.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineEdit>
#include <QListWidget>
#include <QShortcut>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSpinBox>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QTextEdit>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

// ---- texts (PS5 Backport GUI 0.3.4, gui.py / pssr2_text.py) ------------------------------

const QString kSdkTip =
    FW_TR_NOOP("The SDK version written into the game's executables. SDK 4 loads on firmware 4.00 and later; an SDK N build only "
               "loads on firmware N.00 and later. It cannot be higher than the game's own SDK. An SDK N build is planned for "
               "firmware N.00+: FW4-only fixes that the native firmware no longer needs (for example the FW4 save-data "
               "workarounds) are left out, so the game keeps its native behaviour there.");
const QString kProFixTip =
    FW_TR_NOOP("PS5 Pro fix (the Assassin's Creed Black Flag V16 fix set): the Quality60 AGCDriver FW<12 hook and the shader-assert "
               "trap fix. Use it when a backport shows a black screen or GPU hang on a PS5 Pro. The Pro identity gate and PSML Pro "
               "overlay are added automatically where the game needs them.");
const QString kApplyTip =
    FW_TR_NOOP("After the backport is built and verified and the originals are backed up and verified, copy only the backport files "
               "into the selected game folders. You will be asked to confirm first.");
const QString kFakelibTip =
    FW_TR_NOOP("Replace the AMPR / PlayGo libraries in the ticked games' backports (or in a backport ZIP / folder you choose) with "
               "known-good ones. For dumps whose own libraries stop the game right after launch (0xa0020104) or leave it without "
               "content or audio. The executables are not touched.");
const QString kBlackScreenNotice =
    FW_TR_NOOP("Set Target SDK to 5 (or higher) and backport the unmodified game again. This fixes most cases. Keep the FW 4xx ZIP "
               "for FW 4.xx consoles. A right-click rebuild of an old ZIP keeps its FW 4.xx save fixes.");
const QString kSdkRebuildCaveat =
    FW_TR_NOOP("Rebuilt from an existing FW 4xx ZIP, so every FW 4.xx fix in it is kept, including save fixes that can break saving "
               "on newer firmware. If saving fails, backport the unmodified game again with Target SDK set.");
const QString kPssr2Tip =
    FW_TR_NOOP("PS5 Pro build. For a game that declares PS5 Pro support, the backport ZIP is built as a Pro-only ZIP named '... PS5 "
               "Pro backport files.zip': the game's own Pro checks (IsTrinityMode, GetOperationMode, sceAgcGetIsTrinityMode) answer "
               "Pro on every Pro firmware from 9.00, and a game that uses PSSR gets the Pro libScePsml.sprx with the model the game "
               "declares. A game without PSSR gets only the eboot.bin change. A game that declares no PS5 Pro support keeps the "
               "normal ZIP with a note: a PS5 Pro runs it in base mode, so there is nothing to force. Install only on a PS5 Pro with "
               "firmware 9.00 or later; base PS5 consoles use the normal FW 4xx ZIP. The same changes are confirmed working on Black "
               "Flag, UFC 6, F1 25 and Ghost of Yotei (Pro 13.20); every other game needs a console test. In every test so far the "
               "game kept its original PSSR model and testers saw no image change from the PSSR part.");

QString tip(const QString& t)
{
    return QStringLiteral("<p style='white-space:normal'>%1</p>").arg(i18n::translate(t).toHtmlEscaped());
}

QString nameOr(const BackportRow& r)
{
    return r.game.title.isEmpty() ? r.game.path : r.game.title;
}
// "1 game" / "3 games".
QString counted(qsizetype n, const QString& one, const QString& many)
{
    return QStringLiteral("%1 %2").arg(n).arg(n == 1 ? one : many);
}

QString idOr(const BackportRow& r, const QString& fallback = QStringLiteral("no PPSA"))
{
    return r.game.titleId.isEmpty() ? fallback : r.game.titleId;
}

// `shown`: for the page (translated); otherwise for the activity log (English).
QString modeNote(const QJsonObject& pssr2, bool shown = false)
{
    const auto t = [shown](const QString& s) { return shown ? i18n::translate(s) : s; };
    const QString mode = pssr2.value(QStringLiteral("mode")).toString();
    if (mode == QLatin1String("identity_only"))
        return t(FW_TR_NOOP("no PSSR in this game: only the eboot's own Pro checks are changed"));
    const QString v = pssr2.value(QStringLiteral("mfsr_version")).toString();
    const QString model = pssr2.value(QStringLiteral("selected_model")).toString();
    const QString from = v.isEmpty() ? t(FW_TR_NOOP("not declared")) : v;
    const QString to = model.isEmpty() ? t(FW_TR_NOOP("default model")) : model;
    if (model.isEmpty() || model.startsWith(QLatin1String("r40")))
        return t(FW_TR_NOOP("mfsrVersion %1 -> %2 (the game's original PSSR model; this build cannot switch it to the newer PSSR 2 "
                            "models yet)"))
            .arg(from, to);
    return t(FW_TR_NOOP("mfsrVersion %1 -> %2")).arg(from, to);
}

// Covers drawn so far, by icon path (or title initials) and size. A rescan
// clears it (clearCoverCache), so paint never asks the file system for the
// icon's modification time.
QHash<QString, QPixmap>& coverCache()
{
    static QHash<QString, QPixmap> cache;
    return cache;
}

void clearCoverCache() { coverCache().clear(); }

QPixmap coverFor(const BackportRow& r, int px, qreal dpr)
{
    QHash<QString, QPixmap>& cache = coverCache();
    const QString title = r.game.title.isEmpty() ? QFileInfo(r.game.path).fileName() : r.game.title;
    QString key;
    if (!r.game.icon.isEmpty())
        key = r.game.icon + u'|' + QString::number(px);
    else
        key = QStringLiteral("initials|") + title + u'|' + r.game.titleId + u'|' + QString::number(px);
    auto it = cache.find(key);
    if (it != cache.end())
        return *it;
    const int dpx = int(px * dpr);
    QPixmap out(dpx, dpx);
    out.fill(Qt::transparent);
    {
        QPainter p(&out);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(0, 0, dpx, dpx), std::max(3.0, dpx / 6.0), std::max(3.0, dpx / 6.0));
        p.setClipPath(clip);
        QPixmap src;
        if (!r.game.icon.isEmpty())
            src.load(r.game.icon);
        if (!src.isNull()) {
            p.drawPixmap(QRect(0, 0, dpx, dpx), src.scaled(dpx, dpx, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
        } else {
            // The same generated cover as in the Library (the clip above rounds it).
            paintCoverPlaceholder(p, QRectF(0, 0, dpx, dpx), title, r.game.titleId, 0);
        }
    }
    out.setDevicePixelRatio(dpr);
    cache.insert(key, out);
    return out;
}

// ---- delegate ---------------------------------------------------------------------------

constexpr int kStatusPadLeft = 8, kStatusPadRight = 10; // around the Status badge in its cell

class GameDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& o, const QModelIndex& i) const override
    {
        return {QStyledItemDelegate::sizeHint(o, i).width(), 58};
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        const auto* row = static_cast<const BackportRow*>(idx.data(BackportGameModel::RowRole).value<void*>());
        if (!row)
            return;
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRect r = opt.rect;
        // Selection (with its accent bar) and hover are the style's, painted
        // before this; the row only adds its hairline.
        p->fillRect(QRect(r.left(), r.bottom(), r.width(), 1), color::border_1);
        const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
        switch (idx.column()) {
        case BackportGameModel::SelectCol: {
            const QRectF b(r.center().x() - 9.5, r.center().y() - 9.5, 19, 19);
            if (row->checked) {
                p->setPen(Qt::NoPen);
                p->setBrush(accentFillGradient(b)); // white sits on it
                p->drawRoundedRect(b, 5, 5);
                p->drawPixmap(QRectF(b.center().x() - 7, b.center().y() - 7, 14, 14).toRect(),
                              iconPixmap(QStringLiteral("check"), 14, color::on_accent, dpr));
            } else {
                p->setPen(QPen(color::border_control, 1.3));
                p->setBrush(color::field_bg);
                p->drawRoundedRect(b.adjusted(0.5, 0.5, -0.5, -0.5), 5, 5);
            }
            break;
        }
        case BackportGameModel::CoverCol: {
            const int px = 40;
            p->drawPixmap(QRect(r.left() + (r.width() - px) / 2, r.top() + (r.height() - px) / 2, px, px), coverFor(*row, px, dpr));
            break;
        }
        case BackportGameModel::TitleCol: {
            const QRect tr = r.adjusted(4, 9, -8, -9);
            QFont tf = font::bodyStrong();
            p->setFont(tf);
            p->setPen(row->game.title.isEmpty() ? color::fg_4 : color::fg_0);
            const QString title = row->game.title.isEmpty() ? FW_TR("Unknown title") : row->game.title;
            p->drawText(QRect(tr.left(), tr.top(), tr.width(), 20), Qt::AlignLeft | Qt::AlignVCenter,
                        QFontMetrics(tf).elidedText(title, Qt::ElideRight, tr.width()));
            p->setFont(font::caption());
            p->setPen(color::fg_4);
            const QString sub = row->game.path;
            p->drawText(QRect(tr.left(), tr.top() + 21, tr.width(), 18), Qt::AlignLeft | Qt::AlignVCenter,
                        QFontMetrics(font::caption()).elidedText(sub, Qt::ElideMiddle, tr.width()));
            break;
        }
        case BackportGameModel::PpsaCol: {
            const QRect tr = r.adjusted(8, 9, -6, -9);
            const QString id = row->game.titleId;
            p->setFont(id.isEmpty() ? font::caption() : font::mono());
            p->setPen(id.isEmpty() ? color::fg_5 : color::fg_1);
            p->drawText(QRect(tr.left(), tr.top(), tr.width(), 20), Qt::AlignLeft | Qt::AlignVCenter, id.isEmpty() ? FW_TR("Unknown") : id);
            p->setFont(font::caption());
            p->setPen(color::fg_4);
            p->drawText(QRect(tr.left(), tr.top() + 21, tr.width(), 18), Qt::AlignLeft | Qt::AlignVCenter,
                        row->game.version.isEmpty() ? FW_TR("version unknown") : QStringLiteral("v%1").arg(row->game.version));
            break;
        }
        case BackportGameModel::SdkCol:
        case BackportGameModel::FwCol: {
            const QString v = idx.data(Qt::DisplayRole).toString();
            p->setFont(v.isEmpty() ? font::caption() : font::mono());
            p->setPen(v.isEmpty() ? color::fg_5 : color::fg_1);
            p->drawText(r.adjusted(8, 0, -6, 0), Qt::AlignLeft | Qt::AlignVCenter, v.isEmpty() ? FW_TR("Unknown") : v);
            break;
        }
        case BackportGameModel::StatusCol: {
            // The library's badge. The column is as wide as its widest state
            // (fitStatusColumn); a state longer than the column's limit is
            // cut here, and the row's tooltip always has it whole.
            const QRect sr = r.adjusted(kStatusPadLeft, 0, -kStatusPadRight, 0);
            const Tone tone = BackportGameModel::statusTone(*row);
            const QString icon = tone == Tone::Neutral ? QString() : defaultBadgeIcon(tone);
            const QString text = BackportGameModel::statusText(*row);
            const int full = badgeWidth(text, !icon.isEmpty());
            const int w = std::min(full, sr.width());
            QString shown = text;
            if (w < full) {
                // Cut the words, keep the percentage.
                const QFontMetrics fm(font::chip());
                const QString status = i18n::translate(row->status);
                const QString pct = text.mid(status.size());
                shown = fm.elidedText(status, Qt::ElideRight, w - (full - fm.horizontalAdvance(text)) - fm.horizontalAdvance(pct)) + pct;
            }
            paintBadge(*p, QRectF(sr.left(), r.center().y() - size::badge / 2.0, w, size::badge), shown, tone, icon);
            break;
        }
        default:
            break;
        }
        p->restore();
    }
};

} // namespace

// ---- model --------------------------------------------------------------------------------

BackportGameModel::BackportGameModel(QObject* parent) : QAbstractTableModel(parent) {}

QString BackportGameModel::statusText(const BackportRow& row)
{
    // The status is also the row's state (compared in English); it is shown translated.
    const QString status = i18n::translate(row.status);
    return row.tone == QLatin1String("busy") && row.pct > 0 ? QStringLiteral("%1 %2%").arg(status).arg(row.pct) : status;
}

Tone BackportGameModel::statusTone(const BackportRow& row)
{
    if (row.tone == QLatin1String("idle") || row.tone == QLatin1String("good"))
        return Tone::Ok;
    if (row.tone == QLatin1String("warn"))
        return Tone::Warn;
    if (row.tone == QLatin1String("bad"))
        return Tone::Err;
    if (row.tone == QLatin1String("busy"))
        return Tone::Info;
    return Tone::Neutral; // info, muted
}

int BackportGameModel::statusWidth(const BackportRow& row)
{
    const bool icon = statusTone(row) != Tone::Neutral;
    // A running game: as if it were at 100 % already.
    const QString status = i18n::translate(row.status);
    const QString text = row.tone == QLatin1String("busy") ? QStringLiteral("%1 100%").arg(status) : status;
    return kStatusPadLeft + badgeWidth(text, icon) + kStatusPadRight;
}

int BackportGameModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(m_rows.size()); }
int BackportGameModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : ColumnCount; }

QVariant BackportGameModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= int(m_rows.size()))
        return {};
    const BackportRow& r = m_rows[size_t(index.row())];
    if (role == RowRole)
        return QVariant::fromValue(static_cast<void*>(const_cast<BackportRow*>(&r)));
    if (role == FilterRole)
        return QStringList{r.game.title, r.game.titleId, r.game.path}.join(u'\n');
    if (role == SortRole) {
        switch (index.column()) {
        case TitleCol: return (r.game.title.isEmpty() ? r.game.path : r.game.title).toLower();
        case PpsaCol: return r.game.titleId;
        case SdkCol: return r.game.sdk.isEmpty() ? QStringLiteral("~") : r.game.sdk;
        case FwCol: return r.game.fw.isEmpty() ? QStringLiteral("~") : r.game.fw;
        case StatusCol: return r.status;
        default: return index.row();
        }
    }
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case TitleCol: return r.game.title;
        case PpsaCol: return r.game.titleId;
        case SdkCol: return r.game.sdk;
        case FwCol: return r.game.fw;
        case StatusCol: return i18n::translate(r.status);
        default: return {};
        }
    }
    if (role == Qt::CheckStateRole && index.column() == SelectCol)
        return r.checked ? Qt::Checked : Qt::Unchecked;
    if (role == Qt::ToolTipRole) {
        QStringList t;
        t << (r.game.title.isEmpty() ? FW_TR("Unknown title") : r.game.title);
        t << FW_TR("Source folder: %1").arg(r.game.path);
        t << FW_TR("PPSA: %1   Version: %2").arg(r.game.titleId.isEmpty() ? FW_TR("Unknown") : r.game.titleId,
                                                         r.game.version.isEmpty() ? FW_TR("Unknown") : r.game.version);
        t << FW_TR("Source SDK: %1   Required FW: %2").arg(r.game.sdk.isEmpty() ? FW_TR("Unknown") : r.game.sdk,
                                                                   r.game.fw.isEmpty() ? FW_TR("Unknown") : r.game.fw);
        for (const QString& n : r.game.notes)
            t << FW_TR("Note: %1").arg(n);
        if (!r.game.error.isEmpty())
            t << FW_TR("Metadata: %1").arg(r.game.error);
        // Always: the Status column may cut a long state.
        t << (r.detail.isEmpty() ? FW_TR("Status: %1").arg(statusText(r))
                                 : FW_TR("Status: %1 — %2").arg(statusText(r), r.detail));
        t << FW_TR("Double-click for details, right-click for actions.");
        // Titles and notes come from the dump: shown as text, never as markup.
        return QStringLiteral("<p style='white-space:pre-wrap'>%1</p>").arg(t.join(u'\n').toHtmlEscaped());
    }
    return {};
}

QVariant BackportGameModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole)
        return {};
    switch (section) {
    case SelectCol: return QString();
    case CoverCol: return QString();
    case TitleCol: return FW_TR("Game title");
    case PpsaCol: return QStringLiteral("PPSA");
    case SdkCol: return QStringLiteral("SDK");
    case FwCol: return FW_TR("Required FW");
    case StatusCol: return FW_TR("Status");
    default: return {};
    }
}

Qt::ItemFlags BackportGameModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

int BackportGameModel::indexOfKey(const QString& key) const
{
    for (size_t i = 0; i < m_rows.size(); ++i)
        if (m_rows[i].game.key == key)
            return int(i);
    return -1;
}

BackportRow* BackportGameModel::rowByKey(const QString& key)
{
    const int i = indexOfKey(key);
    return i < 0 ? nullptr : &m_rows[size_t(i)];
}

void BackportGameModel::clear()
{
    beginResetModel();
    m_rows.clear();
    endResetModel();
    emit checkedChanged();
}

void BackportGameModel::append(BackportRow row)
{
    beginInsertRows({}, int(m_rows.size()), int(m_rows.size()));
    m_rows.push_back(std::move(row));
    endInsertRows();
    emit checkedChanged();
}

void BackportGameModel::removeKey(const QString& key)
{
    const int i = indexOfKey(key);
    if (i < 0)
        return;
    beginRemoveRows({}, i, i);
    m_rows.erase(m_rows.begin() + i);
    endRemoveRows();
    emit checkedChanged();
}

void BackportGameModel::rowChanged(const QString& key)
{
    const int i = indexOfKey(key);
    if (i >= 0)
        emit dataChanged(index(i, 0), index(i, ColumnCount - 1));
}

void BackportGameModel::allChanged()
{
    if (!m_rows.empty())
        emit dataChanged(index(0, 0), index(int(m_rows.size()) - 1, ColumnCount - 1));
    emit checkedChanged();
}

void BackportFilterModel::setSearch(const QString& text)
{
    beginFilterChange();
    m_search = text.trimmed();
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool BackportFilterModel::filterAcceptsRow(int row, const QModelIndex& parent) const
{
    if (m_search.isEmpty())
        return true;
    const QString hay = sourceModel()->index(row, 0, parent).data(BackportGameModel::FilterRole).toString();
    return hay.contains(m_search, Qt::CaseInsensitive);
}

bool BackportFilterModel::lessThan(const QModelIndex& a, const QModelIndex& b) const
{
    const QVariant va = a.data(BackportGameModel::SortRole), vb = b.data(BackportGameModel::SortRole);
    if (va.typeId() == QMetaType::Int)
        return va.toInt() < vb.toInt();
    return va.toString() < vb.toString(); // string sorts, as the original
}

// ---- page ----------------------------------------------------------------------------------

BackportPage::BackportPage(AppContext& ctx, JobQueueModel&, LogModel&, LibraryStore& library, QWidget* parent)
    : Page(ctx, QStringLiteral("backport"), FW_TR("Backport"),
           FW_TR("Make games run on older PS5 firmware. The originals are always backed up first."),
           parent),
      m_library(library)
{
    bp::SettingsFile settingsState = bp::SettingsFile::Missing;
    QString settingsError;
    if (mayReadSharedFiles())
        m_settings = bp::loadSettings(&settingsState, &settingsError);
    m_settingsMissing = settingsState == bp::SettingsFile::Missing;
    m_model = new BackportGameModel(this);
    m_proxy = new BackportFilterModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setDynamicSortFilter(false);

    m_switchBtn = button(FW_TR("Switch AMPR / PlayGo…"), ButtonVariant::Secondary, QStringLiteral("replace"), this);
    m_switchBtn->setToolTip(tip(kFakelibTip));
    m_start = button(FW_TR("Start Backport"), ButtonVariant::Primary, QStringLiteral("play"), this);
    header()->addAction(m_switchBtn);
    header()->addAction(m_start);
    connect(m_start, &QPushButton::clicked, this, &BackportPage::startOrCancel);
    connect(m_switchBtn, &QPushButton::clicked, this, [this] { switchFakelib(checkedKeys()); });

    m_engineBar = new InfoBar(Tone::Warn, this);
    m_engineBar->hide();
    content()->addWidget(m_engineBar);
    m_settingsBar = new InfoBar(Tone::Warn, this);
    m_settingsBar->hide();
    content()->addWidget(m_settingsBar);
    if (settingsState == bp::SettingsFile::Unreadable)
        showSettingsProblem(FW_TR("The PS5 Backport settings file cannot be read"),
                            FW_TR("%1 cannot be read (%2).")
                                .arg(QDir::toNativeSeparators(bp::folders().settingsPath), settingsError));

    auto* columns = new QWidget(this);
    auto* layout = new TwoColumnLayout(columns, 348, 4000);
    auto* left = new QWidget(columns);
    auto* ll = vbox(left, space::lg);
    auto* right = new QWidget(columns);
    auto* rl = vbox(right, space::lg);
    layout->setLeft(left);
    layout->setRight(right);
    content()->addWidget(columns);

    buildLibraryCard();
    ll->addWidget(m_libraryCard, 1);
    buildProgressCard();
    ll->addWidget(m_progressCard);
    ll->addWidget(m_activityCard);
    buildSideColumn(rl);
    rl->addStretch(1);

    connect(m_model, &BackportGameModel::checkedChanged, this, [this] {
        updateStats();
        updateLibraryState();
    });
    updateModeLine();
    updateButtons();
    updateLibraryState();
    updateStats();

    // An apply interrupted by a crash or power loss: say so even before the
    // page is opened (the restore itself is offered on the page).
    if (!qEnvironmentVariableIsSet("FW_OFFLINE")) {
        QTimer::singleShot(1500, this, [this] {
            const auto rows = bp::interruptedApplies();
            if (!rows.empty())
                emit toastRequested(Tone::Warn, FW_TR("A backport apply was interrupted"),
                                    FW_TR("Open Backport to restore the original files from the verified backup."));
        });
    }
}

BackportPage::~BackportPage()
{
    if (m_scanCancel)
        m_scanCancel->store(true);
}

void BackportPage::buildLibraryCard()
{
    m_libraryCard = new Card(this);
    auto* tools = new QWidget(m_libraryCard);
    auto* th = hbox(tools, space::sm);
    m_search = new QLineEdit(tools);
    m_search->setPlaceholderText(FW_TR("Search title or PPSA"));
    m_search->setAccessibleName(FW_TR("Search games"));
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(200);
    m_search->setMaximumWidth(320);
    m_search->addAction(icon(QStringLiteral("search"), color::fg_4), QLineEdit::LeadingPosition);
    m_rootsBtn = button(FW_TR("Locations (0)"), ButtonVariant::Secondary, QStringLiteral("folders"), tools);
    m_rootsBtn->setToolTip(FW_TR("Show, add or remove the folders that are scanned"));
    th->addWidget(m_rootsBtn);
    m_rescanBtn = new IconButton(QStringLiteral("refresh"), FW_TR("Rescan"), IconButton::Size::Regular, tools);
    th->addWidget(m_rescanBtn);
    m_addBtn = button(FW_TR("Add folder…"), ButtonVariant::Secondary, QStringLiteral("folder_plus"), tools);
    m_addBtn->setToolTip(FW_TR("Add a folder to scan (a game folder, or a folder that contains games). "
                                        "Subfolders are scanned. You can also drag one or more folders onto this window."));
    th->addWidget(m_addBtn);
    auto* titleRow = m_libraryCard->setTitle(FW_TR("Game library"), tools);
    m_countBadge = new Badge(FW_TR("No games yet"), Tone::Neutral, m_libraryCard);
    m_countBadge->setIconName(QString());
    titleRow->insertWidget(1, m_countBadge);

    auto* sub = new QWidget(m_libraryCard);
    auto* sh = hbox(sub, space::md);
    m_search->setParent(sub);
    sh->addWidget(m_search, 1);
    // The three list actions are one group next to the search; the time of
    // the last scan is meta text at the far end.
    auto* links = new QWidget(sub);
    auto* lh = hbox(links, space::md);
    auto* all = new Link(FW_TR("Select all"), links);
    auto* none = new Link(FW_TR("Select none"), links);
    auto* clear = new Link(FW_TR("Clear list"), links);
    clear->setToolTip(FW_TR("Remove every game and scan location from the list. Nothing is deleted from disk."));
    lh->addWidget(all);
    lh->addWidget(none);
    lh->addWidget(clear);
    sh->addWidget(links);
    sh->addStretch(1);
    m_scanLabel = label({}, TextStyle::Caption, color::fg_4, sub);
    m_scanLabel->setTextFormat(Qt::PlainText);
    m_scanLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    sh->addWidget(m_scanLabel);
    m_libraryCard->body()->addWidget(sub);

    m_table = new QTableView(m_libraryCard);
    m_table->setModel(m_proxy);
    m_table->setItemDelegate(new GameDelegate(m_table));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setShowGrid(false);
    m_table->setMouseTracking(true);
    m_table->setWordWrap(false);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(58);
    m_table->setContextMenuPolicy(Qt::CustomContextMenu);
    m_table->setSortingEnabled(false);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_table->setMinimumHeight(380);
    m_table->setAccessibleName(FW_TR("Games"));
    auto* hh = m_table->horizontalHeader();
    hh->setSectionResizeMode(QHeaderView::Fixed);
    hh->setSectionResizeMode(BackportGameModel::TitleCol, QHeaderView::Stretch);
    hh->resizeSection(BackportGameModel::SelectCol, 40);
    hh->resizeSection(BackportGameModel::CoverCol, 52);
    hh->resizeSection(BackportGameModel::PpsaCol, 112);
    hh->resizeSection(BackportGameModel::SdkCol, 60);
    hh->resizeSection(BackportGameModel::FwCol, 96);
    hh->resizeSection(BackportGameModel::StatusCol, BackportGameModel::kStatusColMin);
    // The Status column follows its widest state.
    connect(m_model, &QAbstractItemModel::rowsInserted, this, &BackportPage::fitStatusColumn);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, &BackportPage::fitStatusColumn);
    connect(m_model, &QAbstractItemModel::modelReset, this, &BackportPage::fitStatusColumn);
    connect(m_model, &QAbstractItemModel::dataChanged, this, &BackportPage::fitStatusColumn);
    m_table->viewport()->installEventFilter(this);
    hh->setSectionsClickable(true);
    hh->setHighlightSections(false);
    hh->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_libraryCard->body()->addWidget(m_table, 1);

    m_emptyDrop = new DropZone(QStringLiteral("folder_plus"), FW_TR("Add the folders that contain your extracted games"),
                               FW_TR("Click to browse, or drag folders here. A game folder is one with eboot.bin at its root."),
                               m_libraryCard);
    m_libraryCard->body()->addWidget(m_emptyDrop);
    m_emptyText = label({}, TextStyle::Body, color::fg_3, m_libraryCard);
    m_emptyText->setAlignment(Qt::AlignCenter);
    m_emptyText->setMinimumHeight(120);
    m_libraryCard->body()->addWidget(m_emptyText);

    connect(m_search, &QLineEdit::textChanged, this, [this](const QString& t) {
        m_proxy->setSearch(t);
        updateLibraryState();
    });
    connect(m_addBtn, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(this, FW_TR("Add a folder to scan"));
        if (!d.isEmpty())
            addRoots({d});
    });
    connect(m_emptyDrop, &DropZone::clicked, m_addBtn, &QPushButton::click);
    connect(m_rescanBtn, &QToolButton::clicked, this, &BackportPage::rescan);
    connect(m_rootsBtn, &QPushButton::clicked, this, &BackportPage::editScanLocations);
    connect(clear, &Link::activated, this, &BackportPage::clearList);
    auto setVisible = [this](bool on) {
        if (!isIdle())
            return;
        for (int i = 0; i < m_proxy->rowCount(); ++i)
            m_model->rows()[size_t(m_proxy->mapToSource(m_proxy->index(i, 0)).row())].checked = on;
        m_model->allChanged();
    };
    connect(all, &Link::activated, this, [setVisible] { setVisible(true); });
    connect(none, &Link::activated, this, [setVisible] { setVisible(false); });
    connect(hh, &QHeaderView::sectionClicked, this, [this, setVisible](int section) {
        if (section == BackportGameModel::SelectCol) {
            bool allOn = m_proxy->rowCount() > 0;
            for (int i = 0; i < m_proxy->rowCount(); ++i)
                allOn = allOn && m_model->rows()[size_t(m_proxy->mapToSource(m_proxy->index(i, 0)).row())].checked;
            setVisible(!allOn);
            return;
        }
        if (section == BackportGameModel::CoverCol)
            return;
        const bool same = m_proxy->sortColumn() == section;
        const Qt::SortOrder order = same && m_proxy->sortOrder() == Qt::AscendingOrder ? Qt::DescendingOrder : Qt::AscendingOrder;
        m_proxy->sort(section, order);
        m_table->horizontalHeader()->setSortIndicatorShown(true);
        m_table->horizontalHeader()->setSortIndicator(section, order);
    });
    connect(m_table, &QTableView::clicked, this, [this](const QModelIndex& i) {
        if (i.column() == BackportGameModel::SelectCol && isIdle()) {
            auto& r = m_model->rows()[size_t(m_proxy->mapToSource(i).row())];
            r.checked = !r.checked;
            m_model->rowChanged(r.game.key);
            emit m_model->checkedChanged();
        }
    });
    connect(m_table, &QTableView::doubleClicked, this, [this](const QModelIndex& i) {
        showDetails(m_model->rows()[size_t(m_proxy->mapToSource(i).row())].game.key);
    });
    connect(m_table, &QWidget::customContextMenuRequested, this, [this](const QPoint& p) {
        const QModelIndex i = m_table->indexAt(p);
        if (!i.isValid())
            return;
        if (!m_table->selectionModel()->isRowSelected(i.row(), {}))
            m_table->selectRow(i.row());
        showRowMenu(m_table->viewport()->mapToGlobal(p));
    });
    auto* space = new QShortcut(QKeySequence(Qt::Key_Space), m_table, nullptr, nullptr, Qt::WidgetShortcut);
    connect(space, &QShortcut::activated, this, [this] {
        if (!isIdle())
            return;
        for (const QString& k : selectedKeys())
            if (auto* r = m_model->rowByKey(k)) {
                r->checked = !r->checked;
                m_model->rowChanged(k);
            }
        emit m_model->checkedChanged();
    });
}

void BackportPage::buildProgressCard()
{
    // Progress
    auto* card = new Card(this);
    auto* chipsWrap = new QWidget(card);
    auto* ch = hbox(chipsWrap, 6);
    for (const QString& name : {FW_TR_NOOP("Games"), FW_TR_NOOP("Selected"), FW_TR_NOOP("Ready"), FW_TR_NOOP("Running"),
                                FW_TR_NOOP("Done"), FW_TR_NOOP("Failed / partial")}) {
        // The library's neutral badge: one chip look in the app.
        auto* chip = new Badge(i18n::translate(name), Tone::Neutral, chipsWrap);
        chip->setIconName(QString());
        chip->setProperty("chipName", name);
        m_chips.insert(name, chip);
        ch->addWidget(chip);
    }
    card->setTitle(FW_TR("Progress"), chipsWrap);
    auto* row = new QWidget(card);
    auto* rh = hbox(row, space::md);
    m_bar = new ProgressBar(row);
    m_percent = monoLabel(QStringLiteral("0%"), row, color::fg_0);
    m_percent->setMinimumWidth(QFontMetrics(font::mono()).horizontalAdvance(QStringLiteral("100%")) + 4);
    m_percent->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    rh->addWidget(m_bar, 1);
    rh->addWidget(m_percent);
    card->body()->addWidget(row);
    auto* labels = new QWidget(card);
    auto* lh = hbox(labels, space::md);
    // Game titles and the engine's stage names: plain text, whatever they hold.
    m_current = label(FW_TR("Idle"), TextStyle::Body, color::fg_1, labels);
    m_current->setTextFormat(Qt::PlainText);
    m_stage = label({}, TextStyle::Caption, color::accent_hi, labels);
    m_stage->setTextFormat(Qt::PlainText);
    m_stage->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    lh->addWidget(m_current, 1);
    lh->addWidget(m_stage);
    card->body()->addWidget(labels);

    // Activity
    auto* act = new Card(this);
    auto* actTools = new QWidget(act);
    auto* ah = hbox(actTools, space::sm);
    m_openOutput = button(FW_TR("Open output folder"), ButtonVariant::Subtle, QStringLiteral("folder_open"), actTools);
    m_retry = button(FW_TR("Retry failed"), ButtonVariant::Subtle, QStringLiteral("repeat"), actTools);
    m_retry->setToolTip(FW_TR("Select only the games that failed or were cancelled and run again"));
    auto* clear = button(FW_TR("Clear"), ButtonVariant::Subtle, QStringLiteral("trash"), actTools);
    clear->setToolTip(FW_TR("Clear this view (the log file keeps everything)"));
    setButtonSmall(m_openOutput, true);
    setButtonSmall(m_retry, true);
    setButtonSmall(clear, true);
    ah->addWidget(m_openOutput);
    ah->addWidget(m_retry);
    ah->addWidget(clear);
    act->setTitle(FW_TR("Activity"), actTools);
    m_modeLine = label({}, TextStyle::Caption, color::fg_4, act);
    act->body()->addWidget(m_modeLine);
    m_logBox = new QTextEdit(act);
    m_logBox->setAccessibleName(FW_TR("Activity"));
    m_logBox->setReadOnly(true);
    m_logBox->setMinimumHeight(190);
    m_logBox->setFont(font::monoSmall());
    m_logBox->document()->setMaximumBlockCount(3000);
    m_logBox->setLineWrapMode(QTextEdit::WidgetWidth);
    m_logBox->setStyleSheet(QStringLiteral("QTextEdit{background:%1;border:1px solid %2;border-radius:10px;padding:6px 8px;}")
                                .arg(hex(color::field_bg), hex(color::border_2)));
    act->body()->addWidget(m_logBox);
    m_openOutput->setEnabled(false);
    m_retry->setEnabled(false);
    connect(clear, &QPushButton::clicked, m_logBox, &QTextEdit::clear);
    connect(m_retry, &QPushButton::clicked, this, &BackportPage::retryFailed);
    connect(m_openOutput, &QPushButton::clicked, this, [this] { openPath(m_lastOutputRoot); });
    m_progressCard = card;
    m_activityCard = act;
}

void BackportPage::buildSideColumn(QVBoxLayout* into)
{
    // ---- Save to
    auto* save = new Card(this);
    save->setTitle(FW_TR("Save to"));
    m_outField = new PathField(PathField::Mode::Directory, save);
    m_outField->setDialogTitle(FW_TR("Choose the folder for Backport ZIPs"));
    m_outField->setPlaceholder(FW_TR("Choose a folder…"));
    m_outField->edit()->setToolTip(FW_TR("Where each game's backport ZIP is saved. Click Browse… to choose the folder."));
    m_outField->edit()->setAccessibleName(FW_TR("Folder for backport ZIPs"));
    m_suppressSave = true;
    m_outField->setPath(m_settings.outputDir);
    m_outField->edit()->setCursorPosition(m_settings.outputDir.size());
    auto* outLabel = label(FW_TR("Backport ZIPs"), TextStyle::Body, color::fg_1, save);
    outLabel->setToolTip(FW_TR("Each game's 'FW 4xx <PPSA> <Title> (<version>) backport files.zip' is saved directly in this "
                                        "folder. Each run's loose files and reports are kept in its _backport_details subfolder."));
    m_bakField = new PathField(PathField::Mode::Directory, save);
    m_bakField->setDialogTitle(FW_TR("Choose the folder for Originals backup ZIPs"));
    m_bakField->setPlaceholder(FW_TR("Choose a folder…"));
    m_bakField->edit()->setToolTip(FW_TR("Where the untouched originals of the replaced files are saved. Click Browse… to choose the folder."));
    m_bakField->edit()->setAccessibleName(FW_TR("Folder for originals backup ZIPs"));
    m_bakField->setPath(m_settings.backupDir);
    m_bakField->edit()->setCursorPosition(m_settings.backupDir.size());
    m_suppressSave = false;
    auto* bakLabel = label(FW_TR("Originals backup ZIPs"), TextStyle::Body, color::fg_1, save);
    bakLabel->setToolTip(FW_TR("Each game's '<PPSA> <Title> (<version>) original files.zip' (untouched copies of only the "
                                        "game files the backport replaces) is saved directly in this folder. Never full games."));
    save->body()->addWidget(outLabel);
    save->body()->addWidget(m_outField);
    save->body()->addSpacing(2);
    save->body()->addWidget(bakLabel);
    save->body()->addWidget(m_bakField);
    connect(m_outField, &PathField::pathChanged, this, [this](const QString&) { saveDestinations(); });
    connect(m_bakField, &PathField::pathChanged, this, [this](const QString&) { saveDestinations(); });
    into->addWidget(save);

    // ---- Options (never persisted: every start is SDK 4, everything off)
    auto* opts = new Card(this);
    opts->setTitle(FW_TR("Options"));
    auto* sdkLabel = label(FW_TR("Target SDK"), TextStyle::Body, color::fg_1, opts);
    sdkLabel->setToolTip(tip(kSdkTip));
    m_sdk = new QComboBox(opts);
    m_sdk->addItem(FW_TR("SDK 4  \u00b7  loads on FW 4.00+  (default)"), 4);
    for (int n = 5; n <= 13; ++n)
        m_sdk->addItem(FW_TR("SDK %1  \u00b7  loads on FW %1.00+").arg(n), n);
    m_sdk->setToolTip(tip(kSdkTip));
    m_sdk->setAccessibleName(FW_TR("Target SDK"));
    m_sdkCaption = caption(FW_TR("An SDK N build only loads on firmware N.00 and later."), opts);
    m_sdkCaption->setToolTip(tip(kSdkTip));
    opts->body()->addWidget(sdkLabel);
    opts->body()->addWidget(m_sdk);
    opts->body()->addWidget(m_sdkCaption);
    opts->body()->addWidget(hairline(opts));
    auto addSwitch = [opts](const QString& text, const QString& desc, const QString& tooltip, Switch*& sw) {
        auto* row = new QWidget(opts);
        auto* h = hbox(row, space::md);
        auto* col = new QVBoxLayout;
        col->setSpacing(1);
        auto* t = label(text, TextStyle::Body, color::fg_1, row);
        auto* d = label(desc, TextStyle::Caption, color::fg_3, row);
        keepWrappedHeight(d); // a long translation wraps to more lines
        t->setToolTip(tip(tooltip));
        d->setToolTip(tip(tooltip));
        col->addWidget(t);
        col->addWidget(d);
        h->addLayout(col, 1);
        sw = new Switch(row);
        sw->setAccessibleName(text);
        sw->setToolTip(tip(tooltip));
        h->addWidget(sw, 0, Qt::AlignVCenter);
        opts->body()->addWidget(row);
    };
    addSwitch(FW_TR("Apply to game folders"), FW_TR("Copy the files into the game folders, after a backup."), kApplyTip, m_apply);
    addSwitch(FW_TR("PS5 Pro fix"), FW_TR("For a black screen or GPU hang on a PS5 Pro."), kProFixTip, m_proFix);
    addSwitch(FW_TR("PS5 Pro build (Pro FW 9.00+)"),
              FW_TR("Pro-only ZIP: the game's own Pro checks, and its PSSR, answer Pro on every Pro firmware."), kPssr2Tip, m_proBuild);
    connect(m_sdk, &QComboBox::currentIndexChanged, this, [this](int) { updateModeLine(); });
    for (Switch* s : {m_apply, m_proFix, m_proBuild})
        connect(s, &QAbstractButton::toggled, this, [this](bool) { updateModeLine(); });
    into->addWidget(opts);

    // ---- Black screen tip
    auto* notice = new InfoBar(Tone::Warn, this);
    notice->setTitle(FW_TR("Black screen on FW 5.00+?"));
    notice->setMessage(i18n::translate(kBlackScreenNotice));
    notice->setToolTip(tip(kSdkTip));
    into->addWidget(notice);

    // ---- Backups & support
    auto* sup = new Card(this);
    m_engineBadge = new Badge(FW_TR("Not checked"), Tone::Neutral, sup);
    m_engineBadge->setIconName(QString());
    sup->setTitle(FW_TR("Backups & support"), m_engineBadge);
    m_engineLine = label({}, TextStyle::Caption, color::fg_4, sup);
    m_engineLine->setTextFormat(Qt::PlainText);
    m_engineLine->setWordWrap(true);
    sup->body()->addWidget(m_engineLine);
    auto* restore = button(FW_TR("Restore originals from a backup\u2026"), ButtonVariant::Secondary, QStringLiteral("undo"), sup);
    restore->setToolTip(FW_TR("Puts back the original files of a game folder this program applied a backport to, and removes "
                                       "the files it added (only if they are unchanged)."));
    sup->body()->addWidget(restore);
    auto* grid = new QWidget(sup);
    auto* gh = hbox(grid, space::sm);
    auto* exportBtn = button(FW_TR("Export logs"), ButtonVariant::Secondary, QStringLiteral("archive"), grid);
    exportBtn->setToolTip(FW_TR("Save this program's logs (each run's activity log, engine plan, build log and errors) as one "
                                         "ZIP on your Desktop to send with a bug report. No game files are included."));
    auto* report = button(FW_TR("Report a problem"), ButtonVariant::Secondary, QStringLiteral("bug"), grid);
    report->setToolTip(FW_TR("Open a new issue on the PS5 Backport GUI GitHub page (you can save a logs ZIP to attach first)."));
    gh->addWidget(exportBtn, 1);
    gh->addWidget(report, 1);
    sup->body()->addWidget(grid);
    auto* more = new Disclosure(FW_TR("More"), FW_TR("Scan depth, logs folder"), sup);
    auto* depthRow = new QWidget(more);
    auto* dh = hbox(depthRow, space::md);
    auto* dl = label(FW_TR("Subfolder depth"), TextStyle::Body, color::fg_1, depthRow);
    dl->setToolTip(FW_TR("How many folder levels below each scan location are searched (1-12)."));
    dh->addWidget(dl, 1);
    m_depth = new QSpinBox(depthRow);
    m_depth->setRange(1, 12);
    m_depth->setValue(m_settings.scanDepth);
    m_depth->setFixedWidth(72);
    m_depth->setAccessibleName(FW_TR("Subfolder depth"));
    m_depth->setToolTip(dl->toolTip());
    dh->addWidget(m_depth);
    more->body()->addWidget(depthRow);
    auto* logs = button(FW_TR("Open logs folder"), ButtonVariant::Subtle, QStringLiteral("folder_open"), more);
    more->body()->addWidget(logs, 0, Qt::AlignLeft);
    sup->body()->addWidget(more);
    into->addWidget(sup);

    connect(restore, &QPushButton::clicked, this, &BackportPage::pickRestore);
    connect(exportBtn, &QPushButton::clicked, this, [this] { exportLogs(QStringLiteral("export logs"), true); });
    connect(report, &QPushButton::clicked, this, &BackportPage::reportProblem);
    connect(logs, &QPushButton::clicked, this, [this] {
        QDir().mkpath(bp::folders().logDir);
        openPath(bp::folders().logDir);
    });
    connect(m_depth, &QSpinBox::valueChanged, this, [this](int v) {
        if (v == m_settings.scanDepth)
            return;
        reloadSettings(bp::ScanDepthKey);
        m_settings.scanDepth = v;
        persistSettings(bp::ScanDepthKey);
    });
}

// ---- helpers ---------------------------------------------------------------------------------

// Screenshot runs and automated tests never touch the user's PS5 Backport
// files (settings.json, the activity log) and never start a scan or the
// engine on their own.
bool BackportPage::testMode()
{
    return qApp->property("fwScreenshotMode").toBool() || qEnvironmentVariableIsSet("FW_OFFLINE");
}

bool BackportPage::persistAllowed() const { return !m_demo && !testMode(); }

// An automated test that did not give the PS5 Backport files a folder of its
// own does not even read the user's.
bool BackportPage::mayReadSharedFiles()
{
    return !qEnvironmentVariableIsSet("FW_OFFLINE") || qEnvironmentVariableIsSet("D97BETA_CONFIG_DIR");
}

// settings.json is shared with the standalone PS5 Backport GUI. Before a
// change is applied, what that program wrote since is taken over (except the
// keys in `changing`, which the user is editing right now), so a save never
// rolls its values back. True when the scan locations changed.
bool BackportPage::reloadSettings(int changing)
{
    if (!persistAllowed())
        return false;
    bp::SettingsFile state = bp::SettingsFile::Missing;
    const bp::Settings disk = bp::loadSettings(&state);
    m_settingsMissing = state == bp::SettingsFile::Missing;
    if (state != bp::SettingsFile::Ok)
        return false; // nothing to take over; an unreadable file is reported by the save
    const bool rootsChanged = !(changing & bp::ScanRootsKey) && disk.scanRoots != m_settings.scanRoots;
    if (rootsChanged)
        m_settings.scanRoots = disk.scanRoots;
    m_suppressSave = true;
    if (!(changing & bp::OutputDirKey) && disk.outputDir != m_settings.outputDir) {
        m_settings.outputDir = disk.outputDir;
        m_outField->setPath(disk.outputDir);
    }
    if (!(changing & bp::BackupDirKey) && disk.backupDir != m_settings.backupDir) {
        m_settings.backupDir = disk.backupDir;
        m_bakField->setPath(disk.backupDir);
    }
    m_suppressSave = false;
    if (!(changing & bp::ScanDepthKey) && disk.scanDepth != m_settings.scanDepth) {
        m_settings.scanDepth = disk.scanDepth;
        const QSignalBlocker blocker(m_depth);
        m_depth->setValue(disk.scanDepth);
    }
    if (rootsChanged)
        updateButtons();
    return rootsChanged;
}

void BackportPage::persistSettings(int keys)
{
    if (!persistAllowed())
        return;
    QString error;
    // A file that did not exist is written whole, so it holds what this page shows.
    if (bp::saveSettings(m_settings, m_settingsMissing ? int(bp::AllSettingsKeys) : keys, &error)) {
        m_settingsMissing = false;
        m_settingsBar->hide();
        return;
    }
    log(QStringLiteral("WARN"), QStringLiteral("PS5 Backport settings not saved: %1").arg(error), false);
    showSettingsProblem(FW_TR("The PS5 Backport settings were not saved"), error + u'.');
}

void BackportPage::showSettingsProblem(const QString& title, const QString& message)
{
    m_settingsBar->setTitle(title);
    m_settingsBar->setMessage(message + u' '
                              + FW_TR("Scan locations and folders you change here are kept for this session only. If PS5 "
                                      "Backport GUI is open, close it; if the file is damaged, delete it to start with "
                                      "fresh settings."));
    m_settingsBar->show();
}

void BackportPage::log(const QString& level, const QString& message, bool toBox)
{
    if (persistAllowed())
        bp::appendAppLog(level, message);
    Log lg(QStringLiteral("backport"));
    if (level == QLatin1String("ERROR"))
        lg.error(message);
    else if (level == QLatin1String("WARN"))
        lg.warn(message);
    else
        lg.info(message);
    if (!toBox)
        return;
    QColor lc = color::accent_hi, mc = color::fg_1;
    if (level == QLatin1String("OK")) {
        lc = color::success;
        mc = color::success_hi;
    } else if (level == QLatin1String("WARN")) {
        lc = color::warn;
        mc = color::warn_hi;
    } else if (level == QLatin1String("ERROR")) {
        lc = color::danger;
        mc = color::danger_hi;
    }
    QScrollBar* sb = m_logBox->verticalScrollBar();
    const bool atEnd = sb->value() >= sb->maximum() - 4;
    const QString html = QStringLiteral("<span style='color:%1'>[%2]</span> <span style='color:%3'>[%4]</span> "
                                        "<span style='color:%5;white-space:pre-wrap'>%6</span>")
                             .arg(hex(color::fg_5), QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), hex(lc), level,
                                  hex(mc), message.toHtmlEscaped());
    m_logBox->append(html);
    if (atEnd)
        sb->setValue(sb->maximum());
}

void BackportPage::setState(const QString& text, Tone tone)
{
    m_engineBadge->set(text, tone);
    emit statusContextChanged();
}

int BackportPage::targetSdk() const { return m_sdk->currentData().toInt(); }

QString BackportPage::outputRoot() const { return bp::nativeFolder(m_outField->path()); }

QStringList BackportPage::checkedKeys() const
{
    QStringList out;
    for (const auto& r : m_model->rows())
        if (r.checked)
            out << r.game.key;
    return out;
}

QStringList BackportPage::selectedKeys() const
{
    QStringList out;
    for (const QModelIndex& i : m_table->selectionModel()->selectedRows())
        out << m_model->rows()[size_t(m_proxy->mapToSource(i).row())].game.key;
    return out;
}

BackportRow* BackportPage::currentRow()
{
    const QModelIndex i = m_table->currentIndex();
    if (!i.isValid())
        return nullptr;
    return &m_model->rows()[size_t(m_proxy->mapToSource(i).row())];
}

void BackportPage::saveDestinations()
{
    if (m_suppressSave || m_demo)
        return;
    // Native separators, as the standalone program stores and shows them; the
    // engine job and every text built from these values use the same form.
    const QString out = bp::nativeFolder(m_outField->path());
    const QString bak = bp::nativeFolder(m_bakField->path());
    int keys = 0;
    if (out != m_settings.outputDir)
        keys |= bp::OutputDirKey;
    if (bak != m_settings.backupDir)
        keys |= bp::BackupDirKey;
    if (!keys)
        return; // nothing the user changed: the shared file is left alone
    reloadSettings(keys);
    m_settings.outputDir = out;
    m_settings.backupDir = bak;
    persistSettings(keys);
}

void BackportPage::openPath(const QString& path)
{
    if (path.isEmpty())
        return;
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        Sheet::message(window(), FW_TR("Could not open"), FW_TR("Could not open %1").arg(QDir::toNativeSeparators(path)),
                       Sheet::Icon::Error);
}

void BackportPage::updateModeLine()
{
    QStringList parts{FW_TR("backport + originals ZIPs"),
                      m_apply->isChecked() ? FW_TR("apply to game folders") : FW_TR("game folders unchanged")};
    const int n = targetSdk();
    if (n > 4)
        parts << FW_TR("SDK %1 (FW %1.00+)").arg(n);
    if (m_proFix->isChecked())
        parts << FW_TR("PS5 Pro fix");
    if (m_proBuild->isChecked())
        parts << FW_TR("PS5 Pro build (Pro only)");
    m_modeLine->setText(FW_TR("Next run: %1").arg(parts.join(QStringLiteral(" \u00b7 "))));
}

bool BackportPage::setRunState(RunState state)
{
    if (m_runState == state)
        return true;
    const bool workerAlive = m_worker && m_worker->isRunning();
    bool allowed = false;
    switch (state) {
    case RunState::Idle:
        allowed = !workerAlive; // the page is never idle next to a live worker
        break;
    case RunState::Preflight:
        allowed = m_runState == RunState::Idle;
        break;
    case RunState::Running:
        allowed = (m_runState == RunState::Idle || m_runState == RunState::Preflight) && !workerAlive;
        break;
    case RunState::Cancelling:
        allowed = m_runState == RunState::Running;
        break;
    }
    if (!allowed)
        return false;
    m_runState = state;
    updateButtons();
    updateLibraryState();
    return true;
}

void BackportPage::setScanning(bool on)
{
    if (m_scanning == on)
        return;
    m_scanning = on;
    updateButtons();
    updateLibraryState();
}

// Every action that would start a run, a batch or a scan asks here first.
bool BackportPage::idleForNewWork()
{
    if (isIdle())
        return true;
    Sheet::message(window(), QStringLiteral("Backport"),
                   isPreflight() ? FW_TR("The run you started is still checking its destinations. Wait for it to start or stop.")
                                 : FW_TR("Wait for the current run to finish."));
    return false;
}

// The one gate into Running: from Idle, or from the destination check of this
// very run, and never while a worker is alive. One worker at a time.
bool BackportPage::enterRunning(Kind kind)
{
    if (isRunning() || (m_worker && m_worker->isRunning()))
        return false;
    if (isPreflight() && kind != Kind::Backport)
        return false;
    const Kind before = m_kind;
    m_kind = kind; // updateButtons() reads it
    if (setRunState(RunState::Running))
        return true;
    m_kind = before;
    return false;
}

void BackportPage::updateButtons()
{
    const bool running = isRunning();
    const bool busy = !isIdle(); // a run, or the check before one
    if (running) {
        m_start->setText(isCancelling() ? FW_TR("Cancelling\u2026") : FW_TR("Cancel"));
        setButtonVariant(m_start, ButtonVariant::Danger);
        setButtonIcon(m_start, QStringLiteral("stop"));
    } else {
        m_start->setText(FW_TR("Start Backport"));
        setButtonVariant(m_start, ButtonVariant::Primary);
        setButtonIcon(m_start, QStringLiteral("play"));
    }
    // Checking the destinations, cancelling already, or a restore (which must
    // finish): nothing to press.
    m_start->setEnabled(!(isPreflight() || (running && (isCancelling() || m_kind == Kind::Restore))));
    m_switchBtn->setEnabled(!busy && !isScanning());
    m_rescanBtn->setEnabled(!busy && !isScanning());
    m_addBtn->setEnabled(!busy);
    m_rootsBtn->setEnabled(!busy);
    for (QWidget* w : std::initializer_list<QWidget*>{m_sdk, m_apply, m_proFix, m_proBuild})
        w->setEnabled(!busy);
    m_rootsBtn->setText(FW_TR("Locations (%1)").arg(m_settings.scanRoots.size()));
    updateStartTip();
}

// The Start button says what a click would ask for; it stays enabled, and the
// click still explains it.
void BackportPage::updateStartTip()
{
    m_start->setToolTip(isIdle() && m_selected == 0 ? FW_TR("Select at least one game in the Game library.") : QString());
}

void BackportPage::updateStats()
{
    int games = 0, sel = 0, ready = 0, running = 0, done = 0, failed = 0;
    for (const auto& r : m_model->rows()) {
        ++games;
        if (r.checked)
            ++sel;
        if (r.checked && !r.game.alreadyBackported && r.game.appliedRecord.isEmpty() && r.tone != QLatin1String("busy"))
            ++ready;
        if (r.tone == QLatin1String("busy"))
            ++running;
        if (r.status == QLatin1String("Done") || r.status == QLatin1String("No change needed"))
            ++done;
        if (r.status == QLatin1String("Failed") || r.status == QLatin1String("Partial") || r.status == QLatin1String("Cancelled"))
            ++failed;
    }
    auto set = [this](const QString& name, int n) {
        if (auto* c = m_chips.value(name))
            c->setText(QStringLiteral("%1  %2").arg(i18n::translate(name)).arg(n));
    };
    set(QStringLiteral("Games"), games);
    set(QStringLiteral("Selected"), sel);
    set(QStringLiteral("Ready"), ready);
    set(QStringLiteral("Running"), running);
    set(QStringLiteral("Done"), done);
    set(QStringLiteral("Failed / partial"), failed);
    m_selected = sel;
    updateStartTip();
    m_countBadge->set(games == 0 ? FW_TR("No games yet")
                                 : games == 1 ? FW_TR("1 game found") : FW_TR("%1 games found").arg(games),
                      games ? Tone::Info : Tone::Neutral);
    m_countBadge->setIconName(QString());
}

void BackportPage::updateLibraryState()
{
    const bool noRoots = m_settings.scanRoots.isEmpty() && m_model->rowCount() == 0;
    const bool empty = m_proxy->rowCount() == 0;
    m_emptyDrop->setVisible(noRoots && !isScanning());
    m_table->setVisible(!empty);
    QString text;
    if (empty && !noRoots) {
        if (m_model->rowCount() > 0)
            text = FW_TR("No game matches the search.");
        else if (isScanning())
            text = FW_TR("Scanning\u2026");
        else
            text = FW_TR("No game folders found in the scan locations.");
    } else if (empty && isScanning()) {
        text = FW_TR("Scanning\u2026");
    }
    m_emptyText->setText(text);
    m_emptyText->setVisible(!text.isEmpty());
}

QString BackportPage::statusContext() const
{
    if (isRunning())
        return m_current->text();
    return QStringLiteral("PS5 Backport %1 \u00b7 %2").arg(QLatin1String(bp::kEngineVersion), m_engineBadge->text());
}

// ---- engine -----------------------------------------------------------------------------------

void BackportPage::onShown()
{
    if (m_offerPending && m_engine == Engine::Ready)
        QTimer::singleShot(0, this, [this] {
            if (m_offerPending && isVisible())
                offerInterruptedRestores();
        });
    if (m_shownOnce || testMode())
        return;
    m_shownOnce = true;
    log(QStringLiteral("INFO"), QStringLiteral("PS5 Backport engine %1 by Deckerr97 started").arg(QLatin1String(bp::kEngineVersion)));
    log(QStringLiteral("INFO"), QStringLiteral("Builds are verified offline; test them on a console and report problems with Report a problem"));
    if (!m_settings.scanRoots.isEmpty() && m_model->rowCount() == 0 && !isScanning())
        rescan();
    prepareEngine();
}

void BackportPage::prepareEngine()
{
    if (m_engine == Engine::Preparing || m_engine == Engine::Ready || m_demo)
        return;
    const QString exe = bp::engineExecutable();
    if (exe.isEmpty()) {
        m_engine = Engine::Missing;
        setState(FW_TR("Engine missing"), Tone::Err);
        m_engineLine->setText(FW_TR("PS5 Backport engine %1 is not part of this installation.").arg(QLatin1String(bp::kEngineVersion)));
        m_engineBar->setTone(Tone::Err);
        m_engineBar->setTitle(FW_TR("The backport engine is missing"));
        m_engineBar->setMessage(FW_TR("This copy of the app has no engines\\backport\\%1. Download the app again from the "
                                               "official GitHub release page.")
                                    .arg(QLatin1String(bp::kEngineExeName)));
        m_engineBar->show();
        return;
    }
    if (qEnvironmentVariableIsSet("FW_OFFLINE") && !qEnvironmentVariableIsSet("FW_BACKPORT_ENGINE"))
        return; // tests and screenshot runs never start the engine on their own
    m_engine = Engine::Preparing;
    setState(FW_TR("Preparing engine\u2026"), Tone::Info);
    m_engineLine->setText(FW_TR("PS5 Backport engine %1 by Deckerr97 (bundled)").arg(QLatin1String(bp::kEngineVersion)));
    auto* w = new bp::Worker(this);
    connect(w, &bp::Worker::progress, this, [this](const QJsonObject& ev) {
        const QString d = ev.value(QStringLiteral("detail")).toString();
        if (!d.isEmpty())
            log(QStringLiteral("INFO"), d);
    });
    connect(w, &bp::Worker::finished, this, [this, w](const QJsonObject& r) {
        w->deleteLater();
        if (r.value(QStringLiteral("status")).toString() == QLatin1String("DONE")) {
            m_engineRevision = r.value(QStringLiteral("revision")).toString();
            engineReady();
        } else {
            m_engine = Engine::Failed;
            m_engineError = r.value(QStringLiteral("headline")).toString();
            const QString tail = r.value(QStringLiteral("stderr")).toString();
            log(QStringLiteral("ERROR"), QStringLiteral("Engine preparation failed: %1").arg(m_engineError), false);
            if (!tail.isEmpty())
                log(QStringLiteral("ERROR"), tail.right(600), false);
            log(QStringLiteral("ERROR"), QStringLiteral("The backport engine could not be set up"));
            setState(FW_TR("Engine unavailable"), Tone::Err);
            m_engineBar->setTone(Tone::Err);
            m_engineBar->setTitle(FW_TR("The backport engine could not be set up"));
            m_engineBar->setMessage(FW_TR("Scanning still works; backporting is unavailable. %1").arg(m_engineError));
            m_engineBar->show();
        }
    });
    QString err;
    if (!w->start(QJsonObject{{QStringLiteral("cmd"), QStringLiteral("selftest")}}, &err)) {
        w->deleteLater();
        m_engine = Engine::Failed;
        m_engineError = err;
        setState(FW_TR("Engine unavailable"), Tone::Err);
        log(QStringLiteral("ERROR"), err);
    }
}

void BackportPage::engineReady()
{
    m_engine = Engine::Ready;
    m_engineBar->hide();
    setState(FW_TR("Ready"), Tone::Ok);
    m_engineLine->setText(FW_TR("PS5 Backport engine %1 by Deckerr97  \u00b7  backend %2")
                              .arg(QLatin1String(bp::kEngineVersion), m_engineRevision));
    log(QStringLiteral("INFO"), QStringLiteral("Backport engine ready (backend %1)").arg(m_engineRevision));
    // The question belongs to this page: it waits until the page is in front.
    m_offerPending = true;
    if (isVisible())
        offerInterruptedRestores();
}

// The engine check shared by every action that starts a worker.
bool BackportPage::engineUsable()
{
    switch (m_engine) {
    case Engine::Missing:
        Sheet::message(window(), FW_TR("The backport engine is missing"),
                       FW_TR("This copy of the app has no backport engine. Download the app again from the official GitHub release page."),
                       Sheet::Icon::Error);
        return false;
    case Engine::Preparing:
        Sheet::message(window(), QStringLiteral("Backport"), FW_TR("The backport engine is not ready yet."), Sheet::Icon::Warning);
        return false;
    case Engine::Failed:
        Sheet::message(window(), FW_TR("The backport engine could not be set up"), m_engineError, Sheet::Icon::Error);
        return false;
    case Engine::Unknown:
    case Engine::Ready:
        break;
    }
    return true;
}

// The badge between runs.
void BackportPage::showEngineState()
{
    setState(m_engine == Engine::Ready ? FW_TR("Ready") : FW_TR("Not checked"), m_engine == Engine::Ready ? Tone::Ok : Tone::Neutral);
}

void BackportPage::offerInterruptedRestores()
{
    m_offerPending = false;
    const auto rows = bp::interruptedApplies();
    QPointer<BackportPage> self(this);
    for (const QJsonObject& row : rows) {
        if (!isIdle())
            return;
        const QString game = QDir::toNativeSeparators(row.value(QStringLiteral("game_dir")).toString());
        const QString backup = QDir::toNativeSeparators(row.value(QStringLiteral("backup_dir")).toString());
        const bool yes = Sheet::confirm(window(), FW_TR("Unfinished apply"),
                                        FW_TR("Applying a backport to this game folder may have been interrupted:\n\n%1\n\nIf PS5 Backport "
                                                       "GUI is still working on this folder, choose Not now. Otherwise, restore its original "
                                                       "files now from the verified backup?\n%2")
                                            .arg(game, backup),
                                        FW_TR("Not now"), FW_TR("Restore originals"), false, Sheet::Icon::Warning);
        if (!self)
            return;
        if (yes) {
            // The question may have been open for a while: look again.
            if (bp::foreignEngineRunning()) {
                Sheet::message(window(), FW_TR("Restore"),
                               FW_TR("PS5 Backport GUI is running. Close it, or let it finish, before restoring."), Sheet::Icon::Warning);
                return;
            }
            // The backup run is the folder that holds the journal, as for a
            // restore chosen by hand.
            const QString journal = row.value(QStringLiteral("journal")).toString();
            runRestore(QFileInfo(journal).isFile() ? QDir::toNativeSeparators(QFileInfo(journal).absolutePath())
                                                   : row.value(QStringLiteral("backup_dir")).toString());
            return; // one at a time; the rest are offered again next time
        }
    }
}

// ---- scanning ---------------------------------------------------------------------------------

bool BackportPage::acceptsDrop(const QList<QUrl>& urls) const
{
    for (const QUrl& u : urls)
        if (u.isLocalFile() && QFileInfo(u.toLocalFile()).isDir())
            return true;
    return false;
}

void BackportPage::dropUrls(const QList<QUrl>& urls)
{
    QStringList dirs;
    int ignored = 0;
    for (const QUrl& u : urls) {
        if (u.isLocalFile() && QFileInfo(u.toLocalFile()).isDir())
            dirs << u.toLocalFile();
        else
            ++ignored;
    }
    if (ignored)
        log(QStringLiteral("WARN"), QStringLiteral("%1 ignored: only folders can be added").arg(counted(ignored, QStringLiteral("dropped item"), QStringLiteral("dropped items"))));
    if (!dirs.isEmpty())
        QTimer::singleShot(0, this, [this, dirs] { addRoots(dirs); }); // never inside the drop handler (#25)
}

void BackportPage::setDragHighlight(bool on)
{
    m_libraryCard->setDropHighlight(on);
    m_emptyDrop->setHighlighted(on);
}

void BackportPage::navigateWith(const QVariantMap& args)
{
    // One request for any number of folders: one write of the shared settings
    // and one scan.
    QStringList dirs = args.value(QStringLiteral("folders")).toStringList();
    if (args.contains(QStringLiteral("folder")))
        dirs << args.value(QStringLiteral("folder")).toString();
    dirs.removeAll(QString());
    if (!dirs.isEmpty())
        addRoots(dirs);
}

void BackportPage::addRoots(const QStringList& dirs)
{
    if (!idleForNewWork())
        return;
    bool changed = reloadSettings();
    QStringList keys;
    for (const QString& r : m_settings.scanRoots)
        keys << bp::normKey(r);
    bool added = false;
    for (const QString& d : dirs) {
        if (d.trimmed().isEmpty())
            continue;
        const QString abs = QDir::toNativeSeparators(QFileInfo(d).absoluteFilePath());
        if (keys.contains(bp::normKey(abs)))
            continue;
        keys << bp::normKey(abs);
        m_settings.scanRoots << abs;
        added = true;
        log(QStringLiteral("INFO"), QStringLiteral("Scan location added: %1").arg(abs));
    }
    if (added)
        persistSettings(bp::ScanRootsKey);
    changed = changed || added;
    updateButtons();
    // A location that is already known is not scanned again; a list that was
    // never filled still needs its first scan.
    if (changed || (m_model->rowCount() == 0 && !isScanning() && !m_settings.scanRoots.isEmpty()))
        rescan();
}

void BackportPage::rescan()
{
    if (!idleForNewWork())
        return;
    if (m_demo)
        return;
    const quint64 gen = ++m_scanGen;
    if (m_scanCancel)
        m_scanCancel->store(true);
    m_scanCancel = std::make_shared<std::atomic_bool>(false);
    // Keep each row's tick, result and status across the rescan (by key). A
    // scan that is restarted has only listed some of the games again, so the
    // rows are merged into the memory, never taken as the whole of it.
    for (const auto& r : m_model->rows())
        m_rowMemory.insert(r.game.key, r);
    m_model->clear();
    clearCoverCache(); // icons may have changed on disk since the last scan
    setScanning(true);
    m_scanLabel->setText(FW_TR("Scanning\u2026"));
    updateLibraryState();
    const QStringList roots = m_settings.scanRoots;
    if (roots.isEmpty()) {
        onScanFinished(gen, 0);
        return;
    }
    log(QStringLiteral("INFO"), roots.size() == 1 ? QStringLiteral("Scanning 1 location…")
                                                  : QStringLiteral("Scanning %1 locations…").arg(roots.size()));
    const QStringList excludes = {m_outField->path(), m_bakField->path(), bp::engineHome(), bp::folders().dataDir, bp::folders().configDir};
    const int depth = m_settings.scanDepth;
    auto cancel = m_scanCancel;
    QPointer<BackportPage> self(this);
    auto prev = std::make_shared<const QHash<QString, BackportRow>>(m_rowMemory);
    // The results are posted to the application object and the page is looked
    // at only there, on the GUI thread: the scan thread never touches it.
    const bool readLedger = mayReadSharedFiles();
    (void)QtConcurrent::run([self, gen, roots, excludes, depth, cancel, prev, readLedger] {
        const QStringList games = bp::discoverGames(roots, depth, excludes, cancel.get());
        QMetaObject::invokeMethod(qApp, [self, gen, n = games.size()] {
            if (self && gen == self->m_scanGen)
                self->m_scanLabel->setText(n == 1 ? FW_TR("Reading 1 game folder…")
                                                  : FW_TR("Reading %1 game folders…").arg(n));
        });
        int count = 0;
        const QJsonArray ledger = readLedger ? bp::readLedger() : QJsonArray();
        for (const QString& g : games) {
            if (cancel->load())
                return;
            bp::Game game = bp::readGame(g, ledger);
            ++count;
            QMetaObject::invokeMethod(qApp, [self, gen, game, prev] {
                if (!self || gen != self->m_scanGen)
                    return;
                BackportRow row;
                row.game = game;
                if (const auto it = prev->constFind(game.key); it != prev->constEnd()) {
                    const BackportRow& p = it.value();
                    row.checked = p.checked;
                    row.result = p.result;
                    // "Backport applied" and "Already backported" describe the
                    // folder as it was then; they are decided again below.
                    if (p.status != QLatin1String("Backport applied") && p.status != QLatin1String("Already backported")) {
                        row.status = p.status;
                        row.tone = p.tone;
                        row.detail = p.detail;
                    }
                }
                if (!game.appliedRecord.isEmpty()) {
                    row.status = FW_TR_NOOP("Backport applied");
                    row.tone = QStringLiteral("info");
                    row.checked = false;
                    row.detail = FW_TR("applied %1; restore originals from Backups & support")
                                     .arg(game.appliedRecord.value(QStringLiteral("applied_at")).toString());
                } else if (game.alreadyBackported && row.result.isEmpty()) {
                    row.status = FW_TR_NOOP("Already backported");
                    row.tone = QStringLiteral("warn");
                    row.checked = false;
                    row.detail = FW_TR("this folder already contains backport fakelibs; use an unmodified game folder");
                }
                self->m_model->append(row);
                self->updateLibraryState();
            });
        }
        QMetaObject::invokeMethod(qApp, [self, gen, count] {
            if (self)
                self->onScanFinished(gen, count);
        });
    });
}

void BackportPage::onScanFinished(quint64 gen, int count)
{
    if (gen != m_scanGen)
        return;
    // Only the scan ends here: a run that was started meanwhile goes on.
    setScanning(false);
    m_scanLabel->setText(FW_TR("Last scan %1").arg(QTime::currentTime().toString(QStringLiteral("HH:mm"))));
    if (!m_settings.scanRoots.isEmpty())
        log(QStringLiteral("INFO"), count == 1 ? QStringLiteral("Found 1 game folder") : QStringLiteral("Found %1 game folders").arg(count));
    if (m_proxy->sortColumn() >= 0)
        m_proxy->sort(m_proxy->sortColumn(), m_proxy->sortOrder());
    updateButtons();
    updateLibraryState();
    updateStats();
    syncLibraryStatus();
}

// The Library marks the games that carry a backport: it hears here what the
// scan and the runs found for the folders both pages know.
void BackportPage::syncLibraryStatus()
{
    if (m_demo || m_library.games().empty() || m_model->rows().empty())
        return;
    QHash<QString, bool> backported;
    for (const auto& r : m_model->rows())
        backported.insert(r.game.key, r.game.alreadyBackported || !r.game.appliedRecord.isEmpty());
    std::vector<LibraryGame> games = m_library.games();
    bool changed = false;
    const QString mark = QStringLiteral("Backported");
    for (LibraryGame& g : games) {
        const auto it = backported.constFind(bp::normKey(g.info.folder));
        if (it == backported.constEnd())
            continue;
        const QString status = it.value() ? mark : (g.status == mark ? QString() : g.status);
        if (status != g.status) {
            g.status = status;
            changed = true;
        }
    }
    if (changed)
        m_library.setGames(std::move(games));
}

void BackportPage::editScanLocations()
{
    auto* s = new Sheet(window(), FW_TR("Scan locations"), Sheet::Icon::None, size::dialogWide);
    s->addText(FW_TR("Folders that are scanned for games (subfolders included). Removing one does not delete anything."));
    auto* list = new QListWidget(s);
    list->setAccessibleName(FW_TR("Scan locations"));
    list->setMinimumHeight(200);
    auto fill = [this, list] {
        list->clear();
        for (const QString& r : m_settings.scanRoots)
            list->addItem(QFileInfo(r).isDir() ? r : FW_TR("%1   (not found)").arg(r));
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setData(Qt::UserRole, m_settings.scanRoots.value(i));
    };
    fill();
    s->body()->addWidget(list);
    auto* row = new QWidget(s);
    auto* rh = hbox(row, space::sm);
    auto* add = button(FW_TR("Add folder\u2026"), ButtonVariant::Secondary, QStringLiteral("folder_plus"), row);
    auto* remove = button(FW_TR("Remove selected"), ButtonVariant::Secondary, QStringLiteral("trash"), row);
    rh->addWidget(add);
    rh->addWidget(remove);
    rh->addStretch(1);
    s->body()->addWidget(row);
    bool changed = false;
    connect(add, &QPushButton::clicked, s, [this, s, fill, &changed] {
        const QString d = QFileDialog::getExistingDirectory(s, QString());
        if (d.isEmpty())
            return;
        const QString abs = QDir::toNativeSeparators(QFileInfo(d).absoluteFilePath());
        if (reloadSettings())
            changed = true;
        for (const QString& r : m_settings.scanRoots)
            if (bp::normKey(r) == bp::normKey(abs)) {
                fill();
                return;
            }
        m_settings.scanRoots << abs;
        log(QStringLiteral("INFO"), QStringLiteral("Scan location added: %1").arg(abs));
        persistSettings(bp::ScanRootsKey);
        changed = true;
        fill();
    });
    connect(remove, &QPushButton::clicked, s, [this, list, fill, &changed] {
        auto* it = list->currentItem();
        if (!it)
            return;
        const QString root = it->data(Qt::UserRole).toString();
        if (reloadSettings())
            changed = true;
        if (m_settings.scanRoots.removeAll(root) > 0) {
            log(QStringLiteral("INFO"), QStringLiteral("Scan location removed: %1").arg(root));
            persistSettings(bp::ScanRootsKey);
            changed = true;
        }
        fill();
    });
    s->addButton(FW_TR("Close"), Sheet::Role::Safe);
    s->addButton(FW_TR("Close and rescan"), Sheet::Role::Primary);
    const int r = s->exec();
    updateButtons();
    if (r == 1 || changed)
        rescan();
}

void BackportPage::clearList()
{
    if (!isIdle())
        return;
    // The question guards the scan locations as much as the rows: an empty
    // list (a scan that just started, a drive that is not connected) still has
    // locations to lose.
    if (m_model->rowCount() == 0 && m_settings.scanRoots.isEmpty())
        return;
    QPointer<BackportPage> self(this);
    if (!Sheet::confirm(window(), FW_TR("Clear the game list?"),
                        FW_TR("Clear the game list and forget all scan locations?\nNothing is deleted from disk."),
                        FW_TR("Keep"), FW_TR("Clear list"), true, Sheet::Icon::Warning))
        return;
    if (!self || !isIdle())
        return;
    // A scan still in flight would report to a list that no longer exists.
    ++m_scanGen;
    if (m_scanCancel)
        m_scanCancel->store(true);
    if (isScanning()) {
        setScanning(false);
        m_scanLabel->clear();
    }
    reloadSettings(bp::ScanRootsKey);
    m_settings.scanRoots.clear();
    persistSettings(bp::ScanRootsKey);
    m_rowMemory.clear();
    m_model->clear();
    updateButtons();
    updateLibraryState();
    updateStats();
    log(QStringLiteral("INFO"), QStringLiteral("Game list cleared"));
}

// ---- Start Backport ---------------------------------------------------------------------------

void BackportPage::startOrCancel()
{
    if (isRunning()) {
        requestCancel();
        return;
    }
    startRun(checkedKeys());
}

// The keyboard shortcut only ever starts: a second press out of impatience
// must not abort a run that takes hours (Cancel is the button's job).
void BackportPage::primaryAction()
{
    if (isIdle())
        startRun(checkedKeys());
}

void BackportPage::startRun(const QStringList& keysIn)
{
    if (isPreflight())
        return; // this very run is still checking its destinations
    if (!idleForNewWork() || !engineUsable())
        return;
    if (keysIn.isEmpty()) {
        Sheet::message(window(), QStringLiteral("Backport"), FW_TR("Select at least one game in the Game library."));
        return;
    }
    QStringList keys;
    int skipped = 0;
    for (const QString& k : keysIn) {
        const BackportRow* r = m_model->rowByKey(k);
        if (!r)
            continue;
        if (!r->game.appliedRecord.isEmpty() || r->game.alreadyBackported) {
            ++skipped;
            continue;
        }
        keys << k;
    }
    if (skipped)
        log(QStringLiteral("WARN"), skipped == 1 ? QStringLiteral("1 selected folder already carries backport files and is skipped")
                                                 : QStringLiteral("%1 selected folders already carry backport files and are skipped").arg(skipped));
    if (keys.isEmpty()) {
        Sheet::message(window(), QStringLiteral("Backport"),
                       FW_TR("The selected folders already contain backport files. Use unmodified game folders, or restore their "
                                      "originals first (Backups & support)."));
        return;
    }
    saveDestinations();
    // From here to beginRun() the page is in Preflight: the check runs on a
    // pool thread and may take a while, and nothing else may start meanwhile.
    if (!setRunState(RunState::Preflight))
        return;
    setState(FW_TR("Checking destinations\u2026"), Tone::Info);
    QStringList dirs;
    for (const QString& k : keys)
        dirs << m_model->rowByKey(k)->game.path;
    const QString out = m_settings.outputDir, bak = m_settings.backupDir;
    QPointer<BackportPage> self(this);
    (void)QtConcurrent::run([self, out, bak, dirs, keys] {
        const bp::Preflight pf = bp::preflight(out, bak, dirs);
        QMetaObject::invokeMethod(qApp, [self, pf, keys, dirs] {
            if (self)
                self->preflightDone(pf, keys, dirs);
        });
    });
}

void BackportPage::preflightDone(const bp::Preflight& pf, const QStringList& keys, const QStringList& dirs)
{
    if (!isPreflight())
        return;
    showEngineState();
    // The questions below run their own event loops; the page stays in
    // Preflight under them and may be gone when one returns.
    QPointer<BackportPage> self(this);
    if (!pf.errors.isEmpty()) {
        setRunState(RunState::Idle);
        for (const QString& e : pf.errors)
            log(QStringLiteral("ERROR"), e);
        QStringList shown = pf.errors.mid(0, 10);
        if (pf.errors.size() > shown.size())
            shown << FW_TR("… and %1 more (see Activity)").arg(pf.errors.size() - shown.size());
        Sheet::message(window(), FW_TR("The run cannot start"),
                       FW_TR("The run cannot start:\n\n• %1").arg(shown.join(QStringLiteral("\n• "))), Sheet::Icon::Error);
        return;
    }
    if (!pf.warnings.isEmpty()) {
        for (const QString& w : pf.warnings)
            log(QStringLiteral("WARN"), w);
        const bool go = Sheet::confirm(window(), FW_TR("Continue anyway?"), pf.warnings.join(u'\n') + QStringLiteral("\n\n") + FW_TR("Continue anyway?"),
                                       FW_TR("No"), FW_TR("Continue"), false, Sheet::Icon::Warning);
        if (!self)
            return;
        if (!go) {
            setRunState(RunState::Idle);
            return;
        }
    }
    if (m_apply->isChecked()) {
        QStringList shown;
        for (int i = 0; i < dirs.size() && i < 12; ++i)
            shown << QStringLiteral("• %1").arg(dirs[i]);
        if (dirs.size() > 12)
            shown << FW_TR("• … and %1 more").arg(dirs.size() - 12);
        QString text = FW_TR("These game folders will be changed after each backport is built and verified:\n\n%1\n\n"
                                      "Before anything is written, the original of every file that will be replaced is copied to its "
                                      "own folder under:\n%2\nand verified. Only backport files are written into the game folders; "
                                      "nothing else in them is touched.")
                           .arg(shown.join(u'\n'), m_settings.backupDir);
        if (m_proBuild->isChecked())
            text += QStringLiteral("\n\n")
                + FW_TR("PS5 Pro build is ON: games that declare PS5 Pro support get Pro-only files (eboot.bin, and for "
                        "games with PSSR a different libScePsml.sprx). Apply only to games you play on a PS5 Pro with "
                        "firmware 9.00 or later; a base PS5 should use the normal backport.");
        text += QStringLiteral("\n\n") + FW_TR("Apply to these game folders?");
        const bool go = Sheet::confirm(window(), FW_TR("Apply backport to game folders"), text, FW_TR("No"),
                                       FW_TR("Apply"), true, Sheet::Icon::Warning);
        if (!self)
            return;
        if (!go) {
            setRunState(RunState::Idle);
            log(QStringLiteral("INFO"), QStringLiteral("Run not started: apply was not confirmed"));
            return;
        }
    }
    beginRun(keys);
}

void BackportPage::beginRun(const QStringList& keys)
{
    if (!enterRunning(Kind::Backport)) {
        if (isPreflight())
            setRunState(RunState::Idle);
        return;
    }
    m_runOptions = {{QStringLiteral("apply"), m_apply->isChecked()},
                    {QStringLiteral("zip"), true},
                    {QStringLiteral("target_sdk"), targetSdk()},
                    {QStringLiteral("pro_fix"), m_proFix->isChecked()},
                    {QStringLiteral("pssr2"), m_proBuild->isChecked()}};
    m_runKeys = keys;
    m_runIndex = 0;
    for (const QString& k : keys)
        if (auto* r = m_model->rowByKey(k)) {
            r->status = FW_TR_NOOP("Queued");
            r->tone = QStringLiteral("muted");
            r->detail.clear();
            r->pct = 0;
        }
    m_model->allChanged();
    m_bar->setState(ProgressBar::State::Determinate);
    m_bar->setFraction(0);
    m_percent->setText(QStringLiteral("0%"));
    updateButtons();
    setState(FW_TR("Running"), Tone::Info);
    QString msg = QStringLiteral("Starting %1: SDK %2 (FW %2.00+)").arg(counted(keys.size(), QStringLiteral("game"), QStringLiteral("games"))).arg(targetSdk());
    if (m_proFix->isChecked())
        msg += QStringLiteral(", PS5 Pro fix");
    if (m_proBuild->isChecked())
        msg += QStringLiteral(", PS5 Pro build (Pro only)");
    if (m_apply->isChecked())
        msg += QStringLiteral(", apply to game folders");
    log(QStringLiteral("INFO"), msg);
    m_lastOutputRoot = m_settings.outputDir;
    runNext();
}

void BackportPage::requestCancel()
{
    if (m_runState != RunState::Running)
        return;
    setRunState(RunState::Cancelling);
    setState(FW_TR("Cancelling\u2026"), Tone::Warn);
    log(QStringLiteral("WARN"), QStringLiteral("Cancelling: stopping at the next safe point"));
    if (m_worker)
        m_worker->cancel();
}

void BackportPage::runNext()
{
    const int total = m_kind == Kind::Backport ? int(m_runKeys.size()) : int(m_items.size());
    if (m_runIndex >= total || (isCancelling() && m_kind != Kind::Backport)) {
        if (m_kind == Kind::Backport)
            runFinished();
        else
            batchFinished();
        return;
    }
    QJsonObject job;
    if (m_kind == Kind::Backport) {
        const QString key = m_runKeys[m_runIndex];
        BackportRow* r = m_model->rowByKey(key);
        if (isCancelling()) {
            for (int i = m_runIndex; i < total; ++i)
                if (auto* rr = m_model->rowByKey(m_runKeys[i])) {
                    rr->status = FW_TR_NOOP("Cancelled");
                    rr->tone = QStringLiteral("warn");
                    rr->detail = FW_TR("not started");
                }
            m_model->allChanged();
            runFinished();
            return;
        }
        if (!r) {
            ++m_runIndex;
            runNext();
            return;
        }
        m_runRowKey = key;
        r->status = FW_TR_NOOP("Processing");
        r->tone = QStringLiteral("busy");
        m_model->rowChanged(key);
        m_current->setText(FW_TR("Processing: %1 (%2)  \u2014  game %3 of %4").arg(nameOr(*r), idOr(*r, FW_TR("no PPSA"))).arg(m_runIndex + 1).arg(total));
        job = {{QStringLiteral("cmd"), QStringLiteral("backport")},
               {QStringLiteral("game"), QJsonObject{{QStringLiteral("path"), r->game.path},
                                                     {QStringLiteral("title_id"), r->game.titleId},
                                                     {QStringLiteral("title"), r->game.title},
                                                     {QStringLiteral("version"), r->game.version},
                                                     {QStringLiteral("sdk"), r->game.sdk},
                                                     {QStringLiteral("fw"), r->game.fw}}},
               {QStringLiteral("output_root"), m_settings.outputDir},
               {QStringLiteral("backup_root"), m_settings.backupDir},
               {QStringLiteral("options"), m_runOptions}};
    } else {
        const Item& it = m_items[size_t(m_runIndex)];
        job = it.job;
        m_runRowKey = it.rowKey;
        static const QHash<int, QString> kinds = {{int(Kind::Sdk), FW_TR_NOOP("SDK %1 build")},
                                                  {int(Kind::ProBuild), FW_TR_NOOP("PS5 Pro build")},
                                                  {int(Kind::Fakelib), FW_TR_NOOP("Switch AMPR / PlayGo")},
                                                  {int(Kind::Restore), FW_TR_NOOP("Restore")}};
        QString kind = i18n::translate(kinds.value(int(m_kind)));
        if (m_kind == Kind::Sdk)
            kind = kind.arg(m_sdkPair);
        m_current->setText(FW_TR("%1: %2  \u2014  %3 of %4").arg(kind, it.name).arg(m_runIndex + 1).arg(total));
        if (auto* r = m_model->rowByKey(it.rowKey)) {
            m_rowSavedStatus = r->status;
            m_rowSavedTone = r->tone;
            m_rowSavedDetail = r->detail;
        }
    }
    if (m_worker && m_worker->isRunning())
        return; // one worker at a time; its end continues the run
    m_lastLabel.clear();
    auto* w = new bp::Worker(this);
    m_worker = w;
    connect(w, &bp::Worker::progress, this, &BackportPage::onProgress);
    connect(w, &bp::Worker::finished, this, [this, w](const QJsonObject& r) {
        w->deleteLater();
        onFinished(r);
    });
    QString err;
    if (!w->start(job, &err)) {
        w->deleteLater();
        onFinished({{QStringLiteral("status"), QStringLiteral("FAILED")}, {QStringLiteral("headline"), err}});
        return;
    }
}

void BackportPage::onProgress(const QJsonObject& ev)
{
    const QString stage = ev.value(QStringLiteral("stage")).toString();
    QString label = ev.value(QStringLiteral("label")).toString();
    if (label.isEmpty())
        label = stage;
    const QString detail = ev.value(QStringLiteral("detail")).toString();
    const int pct = ev.value(QStringLiteral("pct")).toInt();
    const int total = m_kind == Kind::Backport ? int(m_runKeys.size()) : int(m_items.size());
    if (auto* r = m_model->rowByKey(m_runRowKey)) {
        r->pct = pct;
        r->status = label.isEmpty() ? FW_TR_NOOP("Processing") : label;
        r->tone = QStringLiteral("busy");
        m_model->rowChanged(m_runRowKey);
    }
    const double frac = total > 0 ? (m_runIndex + pct / 100.0) / total : 0;
    m_bar->setFraction(frac);
    m_percent->setText(QStringLiteral("%1%").arg(int(frac * 100)));
    m_stage->setText(label + QStringLiteral("\u2026"));
    QString who;
    if (const BackportRow* r = m_model->rowByKey(m_runRowKey))
        who = r->game.titleId.isEmpty() ? r->game.title : r->game.titleId;
    else if (m_runIndex < int(m_items.size()) && m_kind != Kind::Backport)
        who = m_items[size_t(m_runIndex)].name;
    if (label != m_lastLabel) {
        m_lastLabel = label;
        log(QStringLiteral("INFO"), who.isEmpty() ? label : QStringLiteral("%1: %2").arg(who, label));
    }
    if (!detail.isEmpty() && stage != QLatin1String("PLAN")) {
        static const QRegularExpression perItem(QStringLiteral("\\d+/\\d+|^Backing up \\d|^Re-checking|^Applying \\d"));
        const bool box = (stage == QLatin1String("EXPORT") || stage == QLatin1String("BACKUP") || stage == QLatin1String("APPLY")
                          || stage == QLatin1String("SDK5"))
            && !perItem.match(detail).hasMatch();
        log(QStringLiteral("INFO"), QStringLiteral("    ") + detail, box);
    }
    updateStats();
    emit statusContextChanged();
}

void BackportPage::onFinished(const QJsonObject& result)
{
    m_worker.clear();
    const QString trace = result.value(QStringLiteral("trace")).toString();
    const QString err = trace.isEmpty() ? result.value(QStringLiteral("stderr")).toString() : trace;
    if (!err.isEmpty())
        log(QStringLiteral("ERROR"), err.right(600), false);
    if (m_kind == Kind::Backport) {
        if (auto* r = m_model->rowByKey(m_runRowKey))
            rowResult(*r, result);
        ++m_runIndex;
        runNext();
        return;
    }
    if (auto* r = m_model->rowByKey(m_runRowKey)) {
        r->status = m_rowSavedStatus;
        r->tone = m_rowSavedTone;
        r->detail = m_rowSavedDetail;
        r->pct = 0;
        const QString status = result.value(QStringLiteral("status")).toString();
        if (status == QLatin1String("DONE")) {
            const char* mergeKey = m_kind == Kind::Sdk ? "sdk_rebuild" : m_kind == Kind::ProBuild ? "pssr2_test" : "fakelib_swap";
            if (m_kind != Kind::Restore)
                r->result.insert(QLatin1String(mergeKey), result);
        }
        m_model->rowChanged(m_runRowKey);
    }
    m_batchResults.emplace_back(m_items[size_t(m_runIndex)], result);
    ++m_runIndex;
    runNext();
}

void BackportPage::rowResult(BackportRow& row, const QJsonObject& r)
{
    const QString status = r.value(QStringLiteral("status")).toString();
    QString text, tone, level;
    if (status == QLatin1String("DONE")) {
        text = FW_TR_NOOP("Done");
        tone = QStringLiteral("good");
        level = QStringLiteral("OK");
    } else if (status == QLatin1String("PARTIAL")) {
        text = FW_TR_NOOP("Partial");
        tone = QStringLiteral("warn");
        level = QStringLiteral("WARN");
    } else if (status == QLatin1String("CANCELLED")) {
        text = FW_TR_NOOP("Cancelled");
        tone = QStringLiteral("warn");
        level = QStringLiteral("WARN");
    } else if (status == QLatin1String("NO_CHANGE")) {
        text = FW_TR_NOOP("No change needed");
        tone = QStringLiteral("info");
        level = QStringLiteral("INFO");
    } else {
        text = FW_TR_NOOP("Failed");
        tone = QStringLiteral("bad");
        level = QStringLiteral("ERROR");
    }
    const QString headline = r.value(QStringLiteral("headline")).toString();
    row.status = text;
    row.tone = tone;
    row.detail = headline;
    row.result = r;
    row.pct = 0;
    m_model->rowChanged(row.game.key);
    log(level, QStringLiteral("%1 (%2): %3 \u2014 %4").arg(nameOr(row), idOr(row), text, headline));
    const QJsonObject sdk = r.value(QStringLiteral("sdk_change")).toObject();
    if (sdk.value(QStringLiteral("applied")).toBool()) {
        const int n = sdk.value(QStringLiteral("target")).toInt();
        log(QStringLiteral("INFO"), QStringLiteral("    SDK %1 build: loads on firmware %1.00 and later only").arg(n));
    }
    const QJsonObject band = r.value(QStringLiteral("engine")).toObject().value(QStringLiteral("target_band")).toObject();
    if (band.contains(QStringLiteral("target_fw"))) {
        const QJsonArray na = band.value(QStringLiteral("not_applied")).toArray();
        QStringList names;
        for (const QJsonValue& v : na)
            names << v.toString();
        const QString what = names.isEmpty() ? QStringLiteral("every fix still applies there")
                                             : QStringLiteral("%1 FW4-only fix(es) left out: %2").arg(names.size()).arg(names.join(QStringLiteral(", ")));
        log(QStringLiteral("INFO"), (QStringLiteral("    FW %1+ band: ").arg(band.value(QStringLiteral("target_fw")).toString()) + what).left(600));
    }
    const QJsonObject stages = r.value(QStringLiteral("stages")).toObject();
    for (auto it = stages.begin(); it != stages.end(); ++it) {
        const QJsonObject st = it.value().toObject();
        const QString s = st.value(QStringLiteral("state")).toString();
        if (s == QLatin1String("FAILED") || s == QLatin1String("CANCELLED"))
            log(s == QLatin1String("FAILED") ? QStringLiteral("ERROR") : QStringLiteral("WARN"),
                (QStringLiteral("    %1: %2").arg(it.key(), st.value(QStringLiteral("detail")).toString())).left(600));
    }
    const QJsonObject pro = r.value(QStringLiteral("pro_fix")).toObject();
    if (pro.value(QStringLiteral("requested")).toBool()) {
        QStringList fam;
        for (const QJsonValue& v : pro.value(QStringLiteral("families")).toArray())
            fam << v.toString();
        log(QStringLiteral("INFO"), QStringLiteral("    PS5 Pro fix: %1").arg(fam.isEmpty() ? QStringLiteral("already present") : fam.join(QStringLiteral(", "))));
    }
    const QJsonObject dump = r.value(QStringLiteral("dump")).toObject();
    if (dump.contains(QStringLiteral("note")))
        log(QStringLiteral("INFO"), (QStringLiteral("    ") + dump.value(QStringLiteral("note")).toString()).left(600));
    if (dump.contains(QStringLiteral("sdk_note")))
        log(QStringLiteral("WARN"), (QStringLiteral("    ") + dump.value(QStringLiteral("sdk_note")).toString()).left(600));
    const QJsonObject pssr2 = r.value(QStringLiteral("pssr2")).toObject();
    if (pssr2.value(QStringLiteral("applied")).toBool())
        log(QStringLiteral("INFO"), QStringLiteral("    PS5 Pro build: PS5 Pro FW 9.00+ only; %1").arg(modeNote(pssr2)));
    else if (pssr2.value(QStringLiteral("requested")).toBool())
        log(QStringLiteral("WARN"), (QStringLiteral("    PS5 Pro build not made: ") + pssr2.value(QStringLiteral("reason")).toString()).left(600));
    const QString zip = r.value(QStringLiteral("zip")).toObject().value(QStringLiteral("path")).toString();
    const QString ozip = r.value(QStringLiteral("originals_zip")).toObject().value(QStringLiteral("path")).toString();
    if (!zip.isEmpty())
        log(QStringLiteral("OK"), QStringLiteral("    Backport ZIP: %1").arg(zip));
    if (!ozip.isEmpty())
        log(QStringLiteral("OK"), QStringLiteral("    Originals ZIP: %1").arg(ozip));
    if (r.contains(QStringLiteral("output_dir")))
        log(QStringLiteral("INFO"), QStringLiteral("    Run details: %1").arg(r.value(QStringLiteral("output_dir")).toString()), false);
    if (status != QLatin1String("DONE") && status != QLatin1String("NO_CHANGE") && r.contains(QStringLiteral("log_dir")))
        log(QStringLiteral("INFO"), QStringLiteral("    Full log: %1 (Export logs collects it)").arg(r.value(QStringLiteral("log_dir")).toString()));
    updateStats();
}

void BackportPage::runFinished()
{
    const bool cancelled = isCancelling();
    setRunState(RunState::Idle);
    m_worker.clear();
    if (!cancelled) {
        m_bar->setFraction(1);
        m_percent->setText(QStringLiteral("100%"));
    }
    m_current->setText(cancelled ? FW_TR("Run cancelled") : FW_TR("Run complete"));
    m_stage->clear();
    updateButtons();
    showEngineState();
    int done = 0;
    QStringList failedNotes;
    bool anyRetry = false;
    const QJsonArray ledger = mayReadSharedFiles() ? bp::readLedger() : QJsonArray();
    for (auto& r : m_model->rows()) {
        if (!m_runKeys.contains(r.game.key))
            continue;
        if (r.status == QLatin1String("Done"))
            ++done;
        if (r.status == QLatin1String("Failed") || r.status == QLatin1String("Partial")) {
            if (!r.result.isEmpty())
                failedNotes << QStringLiteral("%1: %2").arg(r.game.titleId.isEmpty() ? r.game.title : r.game.titleId, r.detail).left(200);
        }
        r.game.appliedRecord = bp::activeLedgerRecord(r.game.path, ledger);
    }
    syncLibraryStatus();
    for (const auto& r : m_model->rows())
        anyRetry = anyRetry || r.status == QLatin1String("Failed") || r.status == QLatin1String("Partial") || r.status == QLatin1String("Cancelled");
    m_retry->setEnabled(anyRetry);
    m_openOutput->setEnabled(!m_lastOutputRoot.isEmpty() && QFileInfo(m_lastOutputRoot).isDir());
    if (cancelled)
        log(QStringLiteral("WARN"), QStringLiteral("Run cancelled: %1 done. Builds are verified offline only; test them on a console.").arg(done));
    else
        log(QStringLiteral("OK"), QStringLiteral("Run finished: %1 done. Builds are verified offline only; test them on a console.").arg(done));
    updateStats();
    emit toastRequested(cancelled ? Tone::Warn : (failedNotes.isEmpty() ? Tone::Ok : Tone::Warn),
                        cancelled ? FW_TR("Backport cancelled") : FW_TR("Backport finished"),
                        FW_TR("%1 done. Test the result on a console.").arg(done));
    if (!failedNotes.isEmpty() && !cancelled) {
        if (Sheet::confirm(window(), FW_TR("Some games did not complete"),
                           (failedNotes.size() == 1 ? FW_TR("1 game failed or was only partly done.")
                                                    : FW_TR("%1 games failed or were only partly done.").arg(failedNotes.size()))
                               + QStringLiteral("\n\n") + FW_TR("Save a ZIP with the full logs to send with your report?"),
                           FW_TR("No"), FW_TR("Save logs ZIP"), false, Sheet::Icon::Warning))
            exportLogs(QStringLiteral("after run: ") + failedNotes.join(QStringLiteral("; ")), true);
    }
}

void BackportPage::retryFailed()
{
    if (!isIdle())
        return;
    QStringList keys;
    for (auto& r : m_model->rows()) {
        r.checked = r.status == QLatin1String("Failed") || r.status == QLatin1String("Partial") || r.status == QLatin1String("Cancelled");
        if (r.checked)
            keys << r.game.key;
    }
    m_model->allChanged();
    startRun(keys);
}

// ---- row-menu jobs ------------------------------------------------------------------------------

void BackportPage::startBatch(Kind kind, std::vector<Item> items)
{
    if (items.empty() || !enterRunning(kind))
        return;
    m_items = std::move(items);
    m_batchResults.clear();
    m_runIndex = 0;
    m_runKeys.clear();
    m_bar->setState(ProgressBar::State::Determinate);
    m_bar->setFraction(0);
    m_percent->setText(QStringLiteral("0%"));
    updateButtons();
    switch (kind) {
    case Kind::Sdk:
        setState(FW_TR("Building SDK %1 versions\u2026").arg(m_sdkPair), Tone::Info);
        log(QStringLiteral("INFO"), QStringLiteral("Rebuild with SDK %1: %2").arg(m_sdkPair).arg(counted(qsizetype(m_items.size()), QStringLiteral("backport ZIP"), QStringLiteral("backport ZIPs"))));
        break;
    case Kind::ProBuild:
        setState(FW_TR("Building PS5 Pro builds\u2026"), Tone::Info);
        log(QStringLiteral("INFO"), QStringLiteral("PS5 Pro build: %1").arg(counted(qsizetype(m_items.size()), QStringLiteral("backport"), QStringLiteral("backports"))));
        break;
    case Kind::Fakelib:
        setState(FW_TR("Switching AMPR / PlayGo\u2026"), Tone::Info);
        break;
    case Kind::Restore:
        setState(FW_TR("Restoring original files\u2026"), Tone::Info);
        break;
    case Kind::Backport:
        break;
    }
    runNext();
}

void BackportPage::batchFinished()
{
    const Kind kind = m_kind;
    const bool cancelled = isCancelling() && kind != Kind::Restore;
    setRunState(RunState::Idle);
    m_kind = Kind::Backport;
    updateButtons();
    showEngineState();
    m_stage->clear();
    if (cancelled) {
        static const QHash<int, QString> what = {{int(Kind::Sdk), QStringLiteral("SDK rebuild")},
                                                 {int(Kind::ProBuild), QStringLiteral("PS5 Pro build")},
                                                 {int(Kind::Fakelib), QStringLiteral("AMPR / PlayGo switch")}};
        // What the page shows: one whole sentence per kind (the log above keeps the English).
        static const QHash<int, QString> cancelledText = {{int(Kind::Sdk), FW_TR_NOOP("SDK rebuild cancelled")},
                                                          {int(Kind::ProBuild), FW_TR_NOOP("PS5 Pro build cancelled")},
                                                          {int(Kind::Fakelib), FW_TR_NOOP("AMPR / PlayGo switch cancelled")}};
        int made = 0;
        for (const auto& [item, res] : m_batchResults)
            if (res.value(QStringLiteral("status")).toString() == QLatin1String("DONE")) {
                ++made;
                log(QStringLiteral("OK"), QStringLiteral("%1: %2").arg(item.name, res.value(QStringLiteral("headline")).toString()));
                log(QStringLiteral("OK"), QStringLiteral("    ZIP: %1").arg(res.value(QStringLiteral("output")).toString()));
            }
        const int left = int(m_items.size()) - made;
        const QString summary = QStringLiteral("%1 done, %2 not made").arg(made).arg(left);
        const QString shownCancelled = i18n::translate(cancelledText.value(int(kind)));
        m_current->setText(shownCancelled);
        log(QStringLiteral("WARN"), QStringLiteral("%1 cancelled: %2").arg(what.value(int(kind)), summary));
        emit toastRequested(Tone::Warn, shownCancelled, FW_TR("%1 done, %2 not made.").arg(made).arg(left));
        return;
    }
    m_bar->setFraction(m_batchResults.empty() ? 0 : 1);
    m_percent->setText(m_batchResults.empty() ? QStringLiteral("0%") : QStringLiteral("100%"));
    if (kind == Kind::Restore) {
        m_current->setText(FW_TR("Restore finished"));
        if (m_batchResults.empty())
            return;
        const QJsonObject r = m_batchResults.front().second;
        const QString game = QDir::toNativeSeparators(r.value(QStringLiteral("game_dir")).toString());
        if (r.value(QStringLiteral("status")).toString() == QLatin1String("DONE")) {
            log(QStringLiteral("OK"), QStringLiteral("Originals restored in %1").arg(game));
            Sheet::message(window(), FW_TR("Originals restored"), FW_TR("The original files were restored in\n%1.").arg(game));
        } else {
            QString why = r.value(QStringLiteral("headline")).toString();
            if (why.isEmpty()) {
                QStringList probs;
                for (const QJsonValue& v : r.value(QStringLiteral("problems")).toArray())
                    probs << v.toString();
                why = probs.join(QStringLiteral("; "));
            }
            log(QStringLiteral("ERROR"), QStringLiteral("Restore did not complete: %1").arg(why));
            Sheet::message(window(), FW_TR("Restore did not complete"), FW_TR("Restore did not complete:\n%1").arg(why), Sheet::Icon::Error);
        }
        rescan();
        return;
    }
    static const QHash<int, QString> doneLabel = {{int(Kind::Sdk), FW_TR_NOOP("SDK rebuild finished")},
                                                  {int(Kind::ProBuild), FW_TR_NOOP("PS5 Pro build finished")},
                                                  {int(Kind::Fakelib), FW_TR_NOOP("AMPR / PlayGo switch finished")}};
    m_current->setText(i18n::translate(doneLabel.value(int(kind))));
    QStringList lines;
    int ok = 0, failed = 0;
    for (const auto& [item, res] : m_batchResults) {
        const bool good = res.value(QStringLiteral("status")).toString() == QLatin1String("DONE");
        const QString headline = res.value(QStringLiteral("headline")).toString();
        const QString output = res.value(QStringLiteral("output")).toString();
        if (good) {
            ++ok;
            log(QStringLiteral("OK"), QStringLiteral("%1: %2").arg(item.name, headline));
            if (kind == Kind::ProBuild)
                log(QStringLiteral("OK"), QStringLiteral("    ZIP: %1  (%2)").arg(output, modeNote(res)));
            else
                log(QStringLiteral("OK"), QStringLiteral("    ZIP: %1").arg(output));
            lines << QStringLiteral("\u2714 %1").arg(QFileInfo(output).fileName());
        } else {
            ++failed;
            log(QStringLiteral("ERROR"), QStringLiteral("%1: %2").arg(item.name, headline));
            if (res.contains(QStringLiteral("log_dir")))
                log(QStringLiteral("INFO"), QStringLiteral("    Full log: %1").arg(res.value(QStringLiteral("log_dir")).toString()));
            lines << QStringLiteral("\u2718 %1: %2").arg(item.name, headline.left(160));
        }
    }
    // A long batch is cut so the buttons stay on screen (the Activity log has every line).
    const int kShown = 14;
    QString text = lines.mid(0, kShown).join(u'\n');
    if (lines.size() > kShown)
        text += u'\n' + FW_TR("… and %1 more (see Activity)").arg(lines.size() - kShown);
    if (ok > 0) {
        if (kind == Kind::Sdk) {
            text += QStringLiteral("\n\n")
                + FW_TR("These builds need firmware %1.00 or later; keep the FW 4xx ZIP for older consoles.\n\n%2")
                      .arg(m_sdkPair) // what was asked for; a failed first item reports none
                      .arg(i18n::translate(kSdkRebuildCaveat));
            log(QStringLiteral("INFO"), kSdkRebuildCaveat);
        } else if (kind == Kind::ProBuild) {
            text += QStringLiteral("\n\n")
                + FW_TR("Install these only on a PS5 Pro with firmware 9.00 or later. Base PS5 consoles keep the normal backport.");
        } else {
            text += QStringLiteral("\n\n")
                + FW_TR("Test the result on the console. If the game still fails, try another library, or switch back by using "
                        "the source package again.");
        }
    }
    if (lines.isEmpty())
        return;
    const QString title = i18n::translate(doneLabel.value(int(kind)));
    if (failed > 0) {
        const QString note = kind == Kind::Sdk ? QStringLiteral("SDK rebuild failures")
                             : kind == Kind::ProBuild ? QStringLiteral("PS5 Pro build failures")
                                                      : QStringLiteral("AMPR / PlayGo switch failures");
        if (Sheet::confirm(window(), title, text + QStringLiteral("\n\n") + FW_TR("Save a ZIP with the full logs to send?"), FW_TR("No"),
                           FW_TR("Save logs ZIP"), false, Sheet::Icon::Warning))
            exportLogs(note, true);
    } else {
        Sheet::message(window(), title, text);
    }
}

QStringList BackportPage::chooseBackportFiles(const QString& question, const QString& fileTitle, bool* cancelled)
{
    *cancelled = false;
    auto* s = new Sheet(window(), FW_TR("Choose a backport"), Sheet::Icon::Info);
    s->addText(question);
    s->addButton(FW_TR("Cancel"), Sheet::Role::Safe);
    s->addButton(FW_TR("Choose a game folder\u2026"), Sheet::Role::Safe);
    s->addButton(FW_TR("Choose ZIP file(s)\u2026"), Sheet::Role::Primary);
    const int r = s->exec();
    if (r == 2) {
        const QStringList files = QFileDialog::getOpenFileNames(this, fileTitle, m_outField->path(),
                                                                FW_TR("Backport ZIP") + QStringLiteral(" (*.zip);;") + FW_TR("All files") + QStringLiteral(" (*)"));
        *cancelled = files.isEmpty();
        return files;
    }
    if (r == 1) {
        const QString d = QFileDialog::getExistingDirectory(this, FW_TR("Choose the backported game folder (it has a fakelib folder)"));
        *cancelled = d.isEmpty();
        return d.isEmpty() ? QStringList{} : QStringList{d};
    }
    *cancelled = true;
    return {};
}

void BackportPage::rebuildWithSdk(const QStringList& keys)
{
    if (!idleForNewWork() || !engineUsable())
        return;
    const int pair = targetSdk();
    if (pair < 5)
        return;
    m_sdkPair = pair;
    std::vector<Item> items;
    for (const QString& k : keys) {
        const BackportRow* r = m_model->rowByKey(k);
        if (!r)
            continue;
        const QString zip = r->result.value(QStringLiteral("zip")).toObject().value(QStringLiteral("path")).toString();
        if (zip.isEmpty() || !QFileInfo(zip).isFile())
            continue;
        items.push_back({{{QStringLiteral("cmd"), QStringLiteral("sdk")},
                          {QStringLiteral("pair"), pair},
                          {QStringLiteral("package"), zip},
                          {QStringLiteral("output_root"), outputRoot().isEmpty() ? QFileInfo(zip).absolutePath() : outputRoot()},
                          {QStringLiteral("title_id"), r->game.titleId},
                          {QStringLiteral("title"), r->game.title},
                          {QStringLiteral("version"), r->game.version},
                          {QStringLiteral("source_sdk"), r->game.sdk}},
                         k, QFileInfo(zip).fileName()});
    }
    if (items.empty()) {
        if (!keys.isEmpty()) {
            QPointer<BackportPage> self(this);
            Sheet::message(window(), QStringLiteral("Backport"),
                           FW_TR("The selected games have no backport ZIP from this session yet. Run Start Backport first, or choose a "
                                          "backport ZIP next."));
            if (!self || !isIdle())
                return;
        }
        const QStringList files = QFileDialog::getOpenFileNames(this, FW_TR("Choose the FW 4xx backport ZIP(s) to rebuild with SDK %1").arg(pair),
                                                                m_outField->path(), FW_TR("Backport ZIP") + QStringLiteral(" (*.zip);;") + FW_TR("All files") + QStringLiteral(" (*)"));
        for (const QString& f : files)
            items.push_back({{{QStringLiteral("cmd"), QStringLiteral("sdk")},
                              {QStringLiteral("pair"), pair},
                              {QStringLiteral("package"), QDir::toNativeSeparators(f)},
                              {QStringLiteral("output_root"), outputRoot().isEmpty() ? QDir::toNativeSeparators(QFileInfo(f).absolutePath()) : outputRoot()},
                              {QStringLiteral("title_id"), QString()},
                              {QStringLiteral("title"), QString()},
                              {QStringLiteral("version"), QString()},
                              {QStringLiteral("source_sdk"), QString()}},
                             QString(), QFileInfo(f).fileName()});
    }
    startBatch(Kind::Sdk, std::move(items));
}

void BackportPage::makeProBuild(const QStringList& keys)
{
    if (!idleForNewWork() || !engineUsable())
        return;
    std::vector<Item> items;
    auto item = [this](const QString& src, const QString& key, const BackportRow* r, const QString& gameDir) {
        return Item{{{QStringLiteral("cmd"), QStringLiteral("pssr2")},
                     {QStringLiteral("package"), QDir::toNativeSeparators(src)},
                     {QStringLiteral("output_root"), outputRoot().isEmpty() ? QDir::toNativeSeparators(QFileInfo(src).absolutePath()) : outputRoot()},
                     {QStringLiteral("title_id"), r ? r->game.titleId : QString()},
                     {QStringLiteral("title"), r ? r->game.title : QString()},
                     {QStringLiteral("version"), r ? r->game.version : QString()},
                     {QStringLiteral("game_dir"), gameDir}},
                    key, QFileInfo(src).fileName()};
    };
    for (const QString& k : keys) {
        const BackportRow* r = m_model->rowByKey(k);
        if (!r)
            continue;
        const QString zip = r->result.value(QStringLiteral("zip")).toObject().value(QStringLiteral("path")).toString();
        if (!zip.isEmpty() && QFileInfo(zip).isFile())
            items.push_back(item(zip, k, r, r->game.path));
        else if (QFileInfo(QDir(r->game.path).filePath(QStringLiteral("fakelib/libScePsml.sprx"))).isFile())
            items.push_back(item(r->game.path, k, r, r->game.path));
    }
    if (items.empty()) {
        if (!keys.isEmpty())
            Sheet::message(window(), QStringLiteral("Backport"),
                           FW_TR("This game has no backport ZIP from this session and its folder is not backported. Choose a backport ZIP or "
                                          "backported folder next."));
        bool cancelled = false;
        const QStringList files = chooseBackportFiles(FW_TR("Make the PS5 Pro build from backport ZIP file(s), or from a backported game folder?"),
                                                      FW_TR("Choose the backport ZIP(s) for a PS5 Pro build"), &cancelled);
        for (const QString& f : files)
            items.push_back(item(f, QString(), nullptr, QFileInfo(f).isDir() ? QDir::toNativeSeparators(f) : QString()));
    }
    startBatch(Kind::ProBuild, std::move(items));
}

void BackportPage::switchFakelib(const QStringList& keys)
{
    if (!idleForNewWork() || !engineUsable())
        return;
    QStringList sources, rowKeys;
    for (const QString& k : keys) {
        const BackportRow* r = m_model->rowByKey(k);
        if (!r)
            continue;
        const QString zip = r->result.value(QStringLiteral("zip")).toObject().value(QStringLiteral("path")).toString();
        if (!zip.isEmpty() && QFileInfo(zip).isFile()) {
            sources << zip;
            rowKeys << k;
        } else if (QFileInfo(QDir(r->game.path).filePath(QStringLiteral("fakelib/libSceAgc.sprx"))).isFile()) {
            sources << r->game.path;
            rowKeys << k;
        }
    }
    if (sources.isEmpty()) {
        if (!keys.isEmpty())
            Sheet::message(window(), QStringLiteral("Backport"),
                           FW_TR("The ticked games have no backport ZIP from this session and their folders are not backported. Choose a "
                                          "backport ZIP or backported folder next."));
        bool cancelled = false;
        sources = chooseBackportFiles(FW_TR("Switch the libraries in backport ZIP file(s), or in a backported game folder?"),
                                      FW_TR("Choose the backport ZIP(s) whose AMPR / PlayGo to switch"), &cancelled);
        rowKeys = QStringList(sources.size(), QString());
        if (sources.isEmpty())
            return;
    }
    QStringList names;
    for (const QString& s : sources)
        names << QFileInfo(s).fileName();
    QJsonValue ampr, playgo;
    if (!chooseFakelib(names, &ampr, &playgo))
        return;
    QStringList what;
    if (!ampr.isNull())
        what << QStringLiteral("AMPR → %1").arg(ampr.toString());
    if (!playgo.isNull())
        what << QStringLiteral("PlayGo → %1").arg(playgo.toString());
    log(QStringLiteral("INFO"), QStringLiteral("Switch AMPR / PlayGo: %1; %2").arg(counted(sources.size(), QStringLiteral("backport"), QStringLiteral("backports")), what.join(QStringLiteral(", "))));
    std::vector<Item> items;
    for (int i = 0; i < sources.size(); ++i) {
        const QString src = QDir::toNativeSeparators(sources[i]);
        items.push_back({{{QStringLiteral("cmd"), QStringLiteral("fakelib")},
                          {QStringLiteral("package"), src},
                          {QStringLiteral("output_root"), outputRoot().isEmpty() ? QDir::toNativeSeparators(QFileInfo(src).absolutePath()) : outputRoot()},
                          {QStringLiteral("ampr"), ampr},
                          {QStringLiteral("playgo"), playgo},
                          {QStringLiteral("title_id"), QString()},
                          {QStringLiteral("title"), QString()},
                          {QStringLiteral("version"), QString()}},
                         rowKeys.value(i), QFileInfo(src).fileName()});
    }
    startBatch(Kind::Fakelib, std::move(items));
}

bool BackportPage::chooseFakelib(const QStringList& names, QJsonValue* ampr, QJsonValue* playgo)
{
    auto* s = new Sheet(window(), FW_TR("Switch AMPR / PlayGo"), Sheet::Icon::None, 760);
    s->addText(FW_TR("Some dumps bring their own AMPR and PlayGo libraries, and a bad one stops the game right after launch (error "
                              "0xa0020104) or leaves it without content or audio. Pick a library that is known to work on other games; the "
                              "game's executables stay as they are. Test the result on the console: which library a game needs is only known "
                              "once it has run."));
    QString pkg = names.join(QStringLiteral(", "));
    if (pkg.size() > 240)
        pkg = pkg.left(240) + QStringLiteral("\u2026");
    auto* pk = label((names.size() > 1 ? FW_TR("Packages: %1") : FW_TR("Package: %1")).arg(pkg), TextStyle::Caption, color::fg_3, s);
    pk->setTextFormat(Qt::PlainText); // file names
    pk->setWordWrap(true);
    s->body()->addWidget(pk);
    struct Row {
        QString kind, name;
        QComboBox* combo = nullptr;
        QLabel* prov = nullptr;
        QString file;
    };
    Row rows[2] = {{QStringLiteral("ampr"), QStringLiteral("AMPR")}, {QStringLiteral("playgo"), QStringLiteral("PlayGo")}};
    for (Row& r : rows) {
        auto* w = new QWidget(s);
        auto* v = vbox(w, 4);
        auto* h = new QHBoxLayout;
        h->setSpacing(space::md);
        auto* l = label(r.name + u':', TextStyle::BodyStrong, color::fg_0, w);
        l->setFixedWidth(64);
        h->addWidget(l);
        r.combo = new QComboBox(w);
        r.combo->setAccessibleName(FW_TR("%1 library").arg(r.name));
        r.combo->addItem(FW_TR("Keep the %1 the package has").arg(r.name), QStringLiteral("keep"));
        for (const bp::Variant& var : bp::variants())
            if (var.kind == r.kind)
                r.combo->addItem(i18n::translate(var.label), var.id);
        r.combo->addItem(FW_TR("Choose a signed .sprx file\u2026"), QStringLiteral("file"));
        h->addWidget(r.combo, 1);
        v->addLayout(h);
        r.prov = label(FW_TR("The package's own %1 stays as it is.").arg(r.name), TextStyle::Caption, color::fg_3, w);
        r.prov->setTextFormat(Qt::PlainText); // may name a file the user chose
        r.prov->setWordWrap(true);
        r.prov->setContentsMargins(76, 0, 0, 0);
        v->addWidget(r.prov);
        s->body()->addWidget(w);
    }
    for (Row& r : rows) {
        Row* rp = &r;
        connect(r.combo, &QComboBox::currentIndexChanged, s, [this, rp, s](int) {
            const QString v = rp->combo->currentData().toString();
            if (v == QLatin1String("keep")) {
                rp->prov->setText(FW_TR("The package's own %1 stays as it is.").arg(rp->name));
            } else if (v == QLatin1String("file")) {
                const QString f = QFileDialog::getOpenFileName(s, FW_TR("Choose a signed %1 library (.sprx)").arg(rp->name), QString(),
                                                               FW_TR("PS5 library") + QStringLiteral(" (*.sprx);;") + FW_TR("All files") + QStringLiteral(" (*)"));
                if (f.isEmpty()) {
                    rp->combo->setCurrentIndex(0);
                    return;
                }
                const bp::UserLibrary u = bp::checkUserLibrary(f, rp->kind);
                if (!u.ok) {
                    Sheet::message(s, FW_TR("Switch AMPR / PlayGo"), u.error, Sheet::Icon::Warning);
                    rp->combo->setCurrentIndex(0);
                    return;
                }
                rp->file = QDir::toNativeSeparators(f);
                rp->prov->setText(QStringLiteral("%1: %2").arg(QFileInfo(f).fileName(), u.label));
            } else {
                for (const bp::Variant& var : bp::variants())
                    if (var.id == v)
                        rp->prov->setText(FW_TR("%1\nSource: %2. Licence: %3.").arg(i18n::translate(var.provenance), var.source, i18n::translate(var.licence)));
            }
        });
    }
    auto* hint = new MessageRow(s);
    s->body()->addWidget(hint);
    s->addButton(FW_TR("Cancel"), Sheet::Role::Safe);
    QPushButton* make = s->addButton(FW_TR("Make the package"), Sheet::Role::Primary);
    auto value = [](const Row& row) -> QJsonValue {
        const QString v = row.combo->currentData().toString();
        if (v == QLatin1String("keep"))
            return QJsonValue(QJsonValue::Null);
        if (v == QLatin1String("file"))
            return row.file;
        return v;
    };
    // The button checks the choice before it closes the sheet: a sheet that
    // has closed is on its way out and cannot be shown or asked again.
    const Row* rowsPtr = rows;
    QObject::disconnect(make, &QPushButton::clicked, s, nullptr);
    connect(make, &QPushButton::clicked, s, [s, hint, rowsPtr, value] {
        if (value(rowsPtr[0]).isNull() && value(rowsPtr[1]).isNull()) {
            hint->set(FW_TR("Choose an AMPR library, a PlayGo library, or both."), Tone::Warn);
            return;
        }
        s->close(1);
    });
    for (Row& r : rows)
        connect(r.combo, &QComboBox::currentIndexChanged, hint, [hint](int) { hint->clear(); });
    if (s->exec() != 1)
        return false;
    // The sheet is hidden but alive until control returns to the event loop.
    *ampr = value(rows[0]);
    *playgo = value(rows[1]);
    return true;
}

// ---- restore -----------------------------------------------------------------------------------

void BackportPage::pickRestore()
{
    if (!idleForNewWork() || !engineUsable())
        return;
    QString start = m_bakField->path();
    if (QFileInfo(QDir(start).filePath(QStringLiteral("_backport_details"))).isDir())
        start = QDir(start).filePath(QStringLiteral("_backport_details"));
    const QString d = QFileDialog::getExistingDirectory(this, FW_TR("Choose the game's folder inside the originals backup folder"), start);
    if (d.isEmpty())
        return;
    const QString journal = QDir(d).filePath(QStringLiteral("APPLY_JOURNAL.json"));
    if (!QFileInfo(journal).isFile()) {
        Sheet::message(window(), FW_TR("Not an applied backup"),
                       FW_TR("That folder is not a backup of an applied backport (it has no APPLY_JOURNAL.json). Choose the per-game "
                                      "folder that contains 'originals' and APPLY_JOURNAL.json."),
                       Sheet::Icon::Error);
        return;
    }
    QFile jf(journal);
    QJsonParseError pe;
    QJsonDocument doc;
    QString why;
    if (jf.open(QIODevice::ReadOnly)) {
        QByteArray b = jf.readAll();
        if (b.startsWith("\xEF\xBB\xBF"))
            b.remove(0, 3);
        doc = QJsonDocument::fromJson(b, &pe);
        if (pe.error != QJsonParseError::NoError)
            why = FW_TR("The journal cannot be read: %1").arg(pe.errorString());
        else if (!doc.isObject())
            why = FW_TR("The journal cannot be read: it is not a journal");
    } else {
        why = FW_TR("The journal cannot be read: %1").arg(jf.errorString());
    }
    if (!doc.isObject()) {
        Sheet::message(window(), FW_TR("Restore"), why, Sheet::Icon::Error);
        return;
    }
    const QJsonObject j = doc.object();
    const QString state = j.value(QStringLiteral("state")).toString();
    if (state == QLatin1String("ROLLED_BACK") || state == QLatin1String("RESTORED")) {
        Sheet::message(window(), FW_TR("Restore"), FW_TR("This backup was already restored."));
        return;
    }
    const QString game = QDir::toNativeSeparators(j.value(QStringLiteral("game_dir")).toString());
    if (!Sheet::confirm(window(), FW_TR("Restore original files?"),
                        FW_TR("Restore the original files of\n%1\nfrom\n%2?\n\nFiles the backport added are removed only if they are unchanged.")
                            .arg(game, QDir::toNativeSeparators(d)),
                        FW_TR("Cancel"), FW_TR("Restore"), true, Sheet::Icon::Warning))
        return;
    runRestore(QDir::toNativeSeparators(d));
}

void BackportPage::runRestore(const QString& backupRun)
{
    if (!idleForNewWork() || !engineUsable())
        return;
    log(QStringLiteral("INFO"), QStringLiteral("Restoring originals from %1").arg(backupRun));
    startBatch(Kind::Restore, {{{{QStringLiteral("cmd"), QStringLiteral("restore")}, {QStringLiteral("backup_run"), backupRun}},
                                QString(), QFileInfo(backupRun).fileName()}});
}

// ---- row menu and details --------------------------------------------------------------------

void BackportPage::showRowMenu(const QPoint& globalPos)
{
    BackportRow* row = currentRow();
    if (!row)
        return;
    const QString key = row->game.key;
    const QStringList sel = selectedKeys().isEmpty() ? QStringList{key} : selectedKeys();
    QMenu menu(this);
    menu.addAction(icon(QStringLiteral("info"), color::fg_3), FW_TR("Show details"), this, [this, key] { showDetails(key); });
    menu.addAction(icon(QStringLiteral("folder_open"), color::fg_3), FW_TR("Open game folder"), this, [this, key] {
        if (auto* r = m_model->rowByKey(key))
            openPath(r->game.path);
    });
    for (const auto& [k, text] : {std::pair{QStringLiteral("output_dir"), FW_TR("Open backport output")},
                                  std::pair{QStringLiteral("backup_dir"), FW_TR("Open originals backup")},
                                  std::pair{QStringLiteral("log_dir"), FW_TR("Open full log")}}) {
        const QString p = row->result.value(k).toString();
        if (!p.isEmpty())
            menu.addAction(text, this, [this, p] { openPath(p); });
    }
    if (isIdle()) {
        menu.addSeparator();
        const int n = targetSdk();
        if (n > 4) {
            menu.addAction(icon(QStringLiteral("layers"), color::fg_3), FW_TR("Rebuild its backport ZIP with SDK %1\u2026").arg(n), this,
                           [this, sel] { rebuildWithSdk(sel); });
        } else {
            auto* a = menu.addAction(FW_TR("Rebuild its backport ZIP with another SDK: choose Target SDK 5-13 first"));
            a->setEnabled(false);
        }
        {
            // The ppr-pkg Companion's Backport tab: the backport as its own update package.
            const QString zip = row->result.value(QStringLiteral("zip")).toObject().value(QStringLiteral("path")).toString();
            const QString game = row->game.path;
            menu.addAction(icon(QStringLiteral("package"), color::fg_3), FW_TR("Package as an installable update (FPKG)\u2026"), this,
                           [this, zip, game] {
                               QVariantMap args{{QStringLiteral("format"), QStringLiteral("fpkg")},
                                                {QStringLiteral("packageType"), QStringLiteral("backport")},
                                                {QStringLiteral("source"), game}};
                               // Only a ZIP whose name says its firmware group is placed; otherwise
                               // the user picks the 4.xx or 5.xx+ slot (a wrong label would put a
                               // 5.xx+ backport on a 4.xx console).
                               if (!zip.isEmpty() && QFileInfo(zip).isFile()) {
                                   const QString group = fpkg::firmwareGroupFromName(QFileInfo(zip).fileName());
                                   if (!group.isEmpty())
                                       args.insert(group == u"4xx" ? QStringLiteral("backport4xx") : QStringLiteral("backport5xx"),
                                                   QDir::toNativeSeparators(zip));
                               }
                               emit navigateRequested({QStringLiteral("build"), args});
                           });
        }
        menu.addAction(icon(QStringLiteral("zap"), color::fg_3), FW_TR("Make a PS5 Pro build from its backport\u2026"), this,
                       [this, sel] { makeProBuild(sel); });
        menu.addAction(FW_TR("Make a PS5 Pro build from a backport ZIP or folder\u2026"), this, [this] { makeProBuild({}); });
        menu.addAction(icon(QStringLiteral("replace"), color::fg_3), FW_TR("Switch its AMPR / PlayGo library (known-good libraries)\u2026"),
                       this, [this, sel] { switchFakelib(sel); });
        menu.addAction(FW_TR("Switch AMPR / PlayGo in a backport ZIP or folder\u2026"), this, [this] { switchFakelib({}); });
    }
    menu.addSeparator();
    menu.addAction(icon(QStringLiteral("copy"), color::fg_3), FW_TR("Copy source path"), this, [this, key] {
        if (auto* r = m_model->rowByKey(key))
            QApplication::clipboard()->setText(r->game.path);
    });
    if (isIdle())
        menu.addAction(icon(QStringLiteral("close"), color::fg_3), FW_TR("Remove from list"), this, [this, sel] {
            if (!isIdle())
                return;
            for (const QString& k : sel) {
                m_model->removeKey(k);
                m_rowMemory.remove(k);
            }
            updateLibraryState();
            updateStats();
        });
    menu.exec(globalPos);
}

void BackportPage::showDetails(const QString& key)
{
    const BackportRow* r = m_model->rowByKey(key);
    if (!r)
        return;
    const QString title = r->game.title.isEmpty() ? FW_TR("Game details") : r->game.title;
    auto* s = new Sheet(window(), title, Sheet::Icon::None, size::dialogWide);
    QStringList lines;
    auto kv = [&lines](const QString& k, const QString& v) {
        lines << QStringLiteral("<tr><td style='color:%1;padding-right:14px;white-space:nowrap;vertical-align:top'>%2</td><td style='color:%3'>%4</td></tr>")
                     .arg(hex(color::fg_4), k.toHtmlEscaped(), hex(color::fg_1), v.toHtmlEscaped().replace(u'\n', QStringLiteral("<br>")));
    };
    auto orUnknown = [](const QString& v) { return v.isEmpty() ? FW_TR("Unknown") : v; };
    kv(FW_TR("Source folder"), orUnknown(r->game.path));
    kv(QStringLiteral("PPSA"), orUnknown(r->game.titleId));
    kv(FW_TR("Version"), orUnknown(r->game.version));
    kv(FW_TR("Source SDK"), orUnknown(r->game.sdk));
    kv(FW_TR("Required firmware"), orUnknown(r->game.fw));
    kv(FW_TR("Status"), r->detail.isEmpty() ? i18n::translate(r->status)
                                         : QStringLiteral("%1 \u2014 %2").arg(i18n::translate(r->status), r->detail));
    const QJsonObject res = r->result;
    const QJsonObject stages = res.value(QStringLiteral("stages")).toObject();
    for (auto it = stages.begin(); it != stages.end(); ++it) {
        const QJsonObject st = it.value().toObject();
        kv(FW_TR("Stage: %1").arg(it.key()),
           QStringLiteral("%1 \u2014 %2").arg(st.value(QStringLiteral("state")).toString(), st.value(QStringLiteral("detail")).toString()));
    }
    if (!res.isEmpty()) {
        QStringList fixes;
        for (const QJsonValue& v : res.value(QStringLiteral("engine")).toObject().value(QStringLiteral("fixes")).toArray())
            fixes << v.toString();
        kv(FW_TR("Engine fixes applied"), fixes.isEmpty() ? FW_TR("none (baseline only)") : fixes.join(QStringLiteral(", ")));
        const QJsonObject sdk = res.value(QStringLiteral("sdk_change")).toObject();
        if (sdk.value(QStringLiteral("requested")).toBool() || sdk.value(QStringLiteral("target")).toInt() > 4) {
            const int n = sdk.value(QStringLiteral("target")).toInt();
            kv(FW_TR("Target SDK"), sdk.value(QStringLiteral("applied")).toBool()
                                                  ? FW_TR("SDK %1 (needs FW %1.00+)").arg(n)
                                                  : FW_TR("SDK %1 not applied: %2").arg(n).arg(sdk.value(QStringLiteral("reason")).toString()));
        }
        const QJsonObject band = res.value(QStringLiteral("engine")).toObject().value(QStringLiteral("target_band")).toObject();
        if (band.contains(QStringLiteral("target_fw"))) {
            QStringList na;
            for (const QJsonValue& v : band.value(QStringLiteral("not_applied")).toArray())
                na << v.toString();
            const QString fw = band.value(QStringLiteral("target_fw")).toString();
            const QString t = na.isEmpty() ? FW_TR("planned for FW %1+: every fix still applies").arg(fw)
                                           : FW_TR("planned for FW %1+: %2 FW4-only fix(es) left out (%3)")
                                                 .arg(fw, QString::number(na.size()), na.join(QStringLiteral(", ")).left(200));
            kv(FW_TR("Firmware band"), t);
        }
        const QJsonObject pro = res.value(QStringLiteral("pro_fix")).toObject();
        if (pro.value(QStringLiteral("requested")).toBool()) {
            QStringList fam;
            for (const QJsonValue& v : pro.value(QStringLiteral("families")).toArray())
                fam << v.toString();
            kv(FW_TR("PS5 Pro fix"), fam.isEmpty() ? FW_TR("already present in the package") : fam.join(QStringLiteral(", ")));
        }
        const QJsonObject dump = res.value(QStringLiteral("dump")).toObject();
        if (dump.contains(QStringLiteral("note")) || dump.contains(QStringLiteral("sdk_note")))
            kv(FW_TR("Dump"), QStringList{dump.value(QStringLiteral("note")).toString(), dump.value(QStringLiteral("sdk_note")).toString()}
                                           .join(u'\n')
                                           .trimmed());
        const QJsonObject pssr2 = res.value(QStringLiteral("pssr2")).toObject();
        if (pssr2.value(QStringLiteral("requested")).toBool())
            kv(FW_TR("PS5 Pro build"), pssr2.value(QStringLiteral("applied")).toBool()
                                                    ? FW_TR("built (Pro FW 9.00+ only): %1").arg(modeNote(pssr2, true))
                                                    : FW_TR("not built: %1").arg(pssr2.value(QStringLiteral("reason")).toString()));
        const QString zip = res.value(QStringLiteral("zip")).toObject().value(QStringLiteral("path")).toString();
        const QString ozip = res.value(QStringLiteral("originals_zip")).toObject().value(QStringLiteral("path")).toString();
        if (!zip.isEmpty())
            kv(FW_TR("Backport ZIP"), zip);
        if (!ozip.isEmpty())
            kv(FW_TR("Originals ZIP"), ozip);
        if (res.contains(QStringLiteral("log_dir")))
            kv(FW_TR("Full log"), res.value(QStringLiteral("log_dir")).toString());
    }
    for (const QString& n : r->game.notes)
        kv(FW_TR("Note"), n);
    auto* text = new QTextEdit(s);
    text->setReadOnly(true);
    text->setMinimumHeight(340);
    text->setHtml(QStringLiteral("<table cellspacing='0' cellpadding='3'>%1</table>").arg(lines.join(QString())));
    text->setStyleSheet(QStringLiteral("QTextEdit{background:%1;border:1px solid %2;border-radius:10px;}").arg(hex(color::field_bg), hex(color::border_2)));
    s->body()->addWidget(text);
    const QString outDir = res.value(QStringLiteral("output_dir")).toString();
    const QString bakDir = res.value(QStringLiteral("backup_dir")).toString();
    s->addButton(FW_TR("Open game folder"), Sheet::Role::Safe);
    int outIdx = -1, bakIdx = -1, n = 1;
    if (!outDir.isEmpty()) {
        s->addButton(FW_TR("Open output"), Sheet::Role::Safe);
        outIdx = n++;
    }
    if (!bakDir.isEmpty()) {
        s->addButton(FW_TR("Open backup"), Sheet::Role::Safe);
        bakIdx = n++;
    }
    s->addButton(FW_TR("Close"), Sheet::Role::Primary);
    const QString path = r->game.path;
    const int choice = s->exec();
    if (choice == 0)
        openPath(path);
    else if (choice == outIdx)
        openPath(outDir);
    else if (choice == bakIdx)
        openPath(bakDir);
}

// ---- logs and reports -------------------------------------------------------------------------

// Writes the support ZIP on a pool thread (it reads and compresses every log)
// and then shows it in Explorer; `showSheet` also says where it was saved.
void BackportPage::exportLogs(const QString& note, bool showSheet)
{
    if (m_exporting)
        return;
    m_exporting = true;
    QStringList hostLogs;
    const QString ourLogs = QDir(Paths::logsDir()).absolutePath();
    for (const QFileInfo& fi : QDir(ourLogs).entryInfoList({QStringLiteral("*.log")}, QDir::Files, QDir::Time).mid(0, 3))
        hostLogs << fi.absoluteFilePath();
    log(QStringLiteral("INFO"), QStringLiteral("Saving the support logs ZIP\u2026"));
    const QString revision = m_engineRevision;
    QPointer<BackportPage> self(this);
    (void)QtConcurrent::run([self, note, revision, hostLogs, showSheet] {
        const QString res = bp::makeSupportZip(note, revision, hostLogs);
        QMetaObject::invokeMethod(qApp, [self, res, showSheet] {
            if (!self)
                return;
            self->m_exporting = false;
            if (res.startsWith(u'!')) {
                self->log(QStringLiteral("ERROR"), QStringLiteral("Could not write the log ZIP: %1").arg(res.mid(1)));
                Sheet::message(self->window(), FW_TR("Export logs"), FW_TR("Could not write the log ZIP: %1").arg(res.mid(1)),
                               Sheet::Icon::Error);
                return;
            }
            self->log(QStringLiteral("OK"), QStringLiteral("Support logs saved: %1").arg(QDir::toNativeSeparators(res)));
#ifdef Q_OS_WIN
            QProcess::startDetached(QStringLiteral("explorer.exe"), {QStringLiteral("/select,"), QDir::toNativeSeparators(res)});
#endif
            if (showSheet)
                Sheet::message(self->window(), FW_TR("Logs saved"),
                               FW_TR("Saved:\n%1\n\nSend this file with your report. It contains this program's logs only (no game files). "
                                              "Your user folder is shown as ~ and the console IP is left out, but the names of the game and "
                                              "output folders you chose are in it.")
                                   .arg(QDir::toNativeSeparators(res)));
            else
                emit self->toastRequested(Tone::Ok, FW_TR("Logs saved"), QDir::toNativeSeparators(res));
        });
    });
}

void BackportPage::reportProblem()
{
    if (Sheet::confirm(window(), FW_TR("Report a problem"),
                       FW_TR("Save a ZIP with this program's logs first, so you can attach it to the report?\n\nIt contains this program's "
                                      "logs only (no game files)."),
                       FW_TR("No"), FW_TR("Save logs ZIP"), false, Sheet::Icon::Info)) {
        exportLogs(QStringLiteral("report a problem"), false);
    }
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("template"), QStringLiteral("bug_report.yml"));
    q.addQueryItem(QStringLiteral("tool_version"), QStringLiteral("%1 (backend %2) in exFAT Image Builder")
                                                       .arg(QLatin1String(bp::kEngineVersion), m_engineRevision.isEmpty() ? QStringLiteral("not ready") : m_engineRevision));
    QString sdk = QStringLiteral("Target SDK %1 (FW %1.00+)").arg(targetSdk());
    if (m_proFix->isChecked())
        sdk += QStringLiteral(", PS5 Pro fix");
    if (m_proBuild->isChecked())
        sdk += QStringLiteral(", PS5 Pro build (Pro FW 9.00+ only)");
    q.addQueryItem(QStringLiteral("sdk"), sdk);
    QString fakelib = QStringLiteral("Default (generated by this program)");
    if (m_proBuild->isChecked())
        fakelib += QStringLiteral(" + PS5 Pro build (eboot Pro checks; PSSR PSML)");
    q.addQueryItem(QStringLiteral("fakelib"), fakelib);
    const BackportRow* one = nullptr;
    int checked = 0, bad = 0;
    const BackportRow* badRow = nullptr;
    for (const auto& r : m_model->rows()) {
        if (r.checked) {
            ++checked;
            one = &r;
        }
        if (r.status == QLatin1String("Failed") || r.status == QLatin1String("Partial")) {
            ++bad;
            badRow = &r;
        }
    }
    const BackportRow* pick = checked == 1 ? one : (bad == 1 ? badRow : nullptr);
    if (pick) {
        if (!pick->game.title.isEmpty())
            q.addQueryItem(QStringLiteral("game_title"), pick->game.title);
        if (!pick->game.titleId.isEmpty())
            q.addQueryItem(QStringLiteral("ppsa"), pick->game.titleId);
        if (!pick->game.version.isEmpty())
            q.addQueryItem(QStringLiteral("game_version"), pick->game.version);
    }
    QUrl url(QString::fromLatin1(bp::kIssuesUrl));
    url.setQuery(q);
    log(QStringLiteral("INFO"), QStringLiteral("Opening the GitHub bug report form: %1").arg(QString::fromLatin1(bp::kIssuesUrl)));
    QDesktopServices::openUrl(url);
}

// As wide as the widest state in the list, within the column's limits and as
// long as the title keeps its room (a state that does not fit is cut in its
// cell; the row's tooltip has it whole).
void BackportPage::fitStatusColumn()
{
    int w = BackportGameModel::kStatusColMin;
    for (const BackportRow& r : m_model->rows())
        w = std::max(w, BackportGameModel::statusWidth(r));
    int others = 0;
    for (int c = 0; c < BackportGameModel::ColumnCount; ++c)
        if (c != BackportGameModel::TitleCol && c != BackportGameModel::StatusCol && !m_table->isColumnHidden(c))
            others += m_table->columnWidth(c);
    const int room = m_table->viewport()->width() - others - BackportGameModel::kTitleColMin;
    w = std::clamp(w, BackportGameModel::kStatusColMin, std::max(BackportGameModel::kStatusColMin, std::min(BackportGameModel::kStatusColMax, room)));
    if (m_table->columnWidth(BackportGameModel::StatusCol) != w)
        m_table->setColumnWidth(BackportGameModel::StatusCol, w);
}

bool BackportPage::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_table->viewport() && event->type() == QEvent::Resize) {
        const int w = m_table->viewport()->width();
        m_table->setColumnHidden(BackportGameModel::FwCol, w < 720);
        m_table->setColumnHidden(BackportGameModel::SdkCol, w < 620);
        m_table->setColumnHidden(BackportGameModel::CoverCol, w < 520);
        fitStatusColumn();
    }
    return Page::eventFilter(watched, event);
}

void BackportPage::cancelAndWait(int ms)
{
    if (!isBusy())
        return;
    if (m_kind != Kind::Restore)
        requestCancel(); // a restore ignores cancel and must run to its end
    QElapsedTimer t;
    t.start();
    while (isBusy() && t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
}

// ---- screenshots -----------------------------------------------------------------------------

QStringList BackportPage::demoStates() const
{
    return {QStringLiteral("library"), QStringLiteral("running")};
}

void BackportPage::applyDemoState(const QString& name)
{
    m_demo = true;
    ++m_scanGen;
    if (m_scanCancel)
        m_scanCancel->store(true);
    setScanning(false);
    m_rowMemory.clear();
    m_model->clear();
    auto add = [this](const QString& id, const QString& title, const QString& ver, const QString& sdk, const QString& fw,
                      const QString& status, const QString& tone, int pct = 0) {
        BackportRow r;
        r.game.path = QStringLiteral("D:\\Games\\%1 %2").arg(id, title);
        r.game.key = r.game.path.toLower();
        r.game.titleId = id;
        r.game.title = title;
        r.game.version = ver;
        r.game.sdk = sdk;
        r.game.fw = fw;
        r.status = status;
        r.tone = tone;
        r.pct = pct;
        m_model->append(r);
    };
    add(QStringLiteral("PPSA18391"), QStringLiteral("Afterdream"), QStringLiteral("02.000.000"), QStringLiteral("8.00"), QStringLiteral("8.00"),
        QStringLiteral("Ready"), QStringLiteral("idle"));
    add(QStringLiteral("PPSA09232"), QStringLiteral("Ghost Song"), QStringLiteral("01.005.000"), QStringLiteral("6.00"), QStringLiteral("6.00"),
        name == QLatin1String("running") ? QStringLiteral("Patching, signing and packaging") : QStringLiteral("Ready"),
        name == QLatin1String("running") ? QStringLiteral("busy") : QStringLiteral("idle"), 50);
    add(QStringLiteral("PPSA27500"), QStringLiteral("ONE PIECE Pirate Warriors 4"), QStringLiteral("01.003.000"), QStringLiteral("9.00"),
        QStringLiteral("9.00"), name == QLatin1String("running") ? QStringLiteral("Done") : QStringLiteral("Ready"),
        name == QLatin1String("running") ? QStringLiteral("good") : QStringLiteral("idle"));
    add(QStringLiteral("PPSA10528"), QStringLiteral("Prince of Persia The Lost Crown"), QStringLiteral("01.000.003"), QStringLiteral("7.00"),
        QStringLiteral("7.00"), QStringLiteral("Already backported"), QStringLiteral("warn"));
    m_model->rows()[3].checked = false;
    m_settings.scanRoots = QStringList{QStringLiteral("D:\\Games")};
    m_outField->setPath(QStringLiteral("D:\\Backports"));
    m_bakField->setPath(QStringLiteral("D:\\Backports Backups"));
    setState(FW_TR("Ready"), Tone::Ok);
    m_engineBar->hide();
    m_engineLine->setText(QStringLiteral("PS5 Backport engine 0.3.4 by Deckerr97  \u00b7  backend 3e08eda5-914d90065d"));
    if (name == QLatin1String("running")) {
        m_bar->setFraction(0.42);
        m_percent->setText(QStringLiteral("42%"));
        m_current->setText(QStringLiteral("Processing: Ghost Song (PPSA09232)  \u2014  game 2 of 3"));
        m_stage->setText(QStringLiteral("Patching, signing and packaging\u2026"));
        log(QStringLiteral("INFO"), QStringLiteral("Starting 3 games: SDK 4 (FW 4.00+)"));
        log(QStringLiteral("OK"), QStringLiteral("ONE PIECE Pirate Warriors 4 (PPSA27500): Done \u2014 Backport files created. Not yet tested on a console."));
        log(QStringLiteral("OK"), QStringLiteral("    Backport ZIP: D:\\Backports\\FW 4xx PPSA27500 ONE PIECE (01.003.000) backport files.zip"));
        log(QStringLiteral("INFO"), QStringLiteral("PPSA09232: Patching, signing and packaging"));
    }
    m_scanLabel->setText(QStringLiteral("Last scan 14:02"));
    updateButtons();
    updateLibraryState();
    updateStats();
}

void BackportPage::clearDemoState()
{
    if (!m_demo)
        return;
    m_model->clear();
    m_settings = mayReadSharedFiles() ? bp::loadSettings() : bp::Settings();
    m_suppressSave = true;
    m_outField->setPath(m_settings.outputDir);
    m_bakField->setPath(m_settings.backupDir);
    m_suppressSave = false;
    m_demo = false;
    m_bar->setFraction(0);
    m_percent->setText(QStringLiteral("0%"));
    m_current->setText(FW_TR("Idle"));
    m_stage->clear();
    updateButtons();
    updateLibraryState();
    updateStats();
}

} // namespace fw::pages
