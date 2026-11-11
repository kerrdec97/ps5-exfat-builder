#include "app/pages/SettingsPage.h"

#include "app/AppContext.h"
#include "app/services/Language.h"
#include "app/services/PortableMode.h"
#include "app/ui/Components.h"
#include "app/ui/Icons.h"
#include "app/ui/Motion.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "core/Paths.h"
#include "core/Tr.h"
#include "engines/general/Module.h"

#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QDir>
#include <QFileDialog>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

QString sectionIcon(const QString& section)
{
    static const QHash<QString, QString> icons = {
        {QStringLiteral("General"), QStringLiteral("settings")},
        {QStringLiteral("Build"), QStringLiteral("build")},
        {QStringLiteral("FPKG"), QStringLiteral("package")},
        {QStringLiteral("PFS"), QStringLiteral("compress")},
        {QStringLiteral("PlayGo"), QStringLiteral("boxes")},
        {QStringLiteral("Backport"), QStringLiteral("backports")},
        {QStringLiteral("DLC"), QStringLiteral("package_plus")},
        {QStringLiteral("Firmware and SDK"), QStringLiteral("cpu")},
        {QStringLiteral("Tools"), QStringLiteral("wrench")},
        {QStringLiteral("Performance"), QStringLiteral("gauge")},
        {QStringLiteral("Storage"), QStringLiteral("hard_drive")},
        {QStringLiteral("Verification"), QStringLiteral("shield_check")},
        {QStringLiteral("PS5 connection"), QStringLiteral("plug")},
        {QStringLiteral("Logging"), QStringLiteral("file_text")},
        {QStringLiteral("Advanced"), QStringLiteral("advanced")},
    };
    return icons.value(section, QStringLiteral("settings"));
}

// A 34 px section row for the left list: icon + name (+ count badge).
class SectionButton : public QAbstractButton {
public:
    SectionButton(const QString& text, const QString& iconName, QWidget* parent) : QAbstractButton(parent), m_icon(iconName)
    {
        setText(text);
        setAccessibleName(text);
        setCheckable(true);
        setFixedHeight(34);
        setFocusPolicy(Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
        setCursor(Qt::PointingHandCursor);
        setProperty("fwRadius", radius::control);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setBadge(Badge* b)
    {
        m_badge = b;
        placeBadge();
    }
    void placeBadge()
    {
        if (!m_badge)
            return;
        m_badge->adjustSize();
        if (m_compact)
            m_badge->move(width() - m_badge->width(), 0); // corner of the icon-only row
        else
            m_badge->move(width() - m_badge->width() - 8, (height() - m_badge->height()) / 2);
    }
    // Icon only (narrow windows); the name moves to the tooltip.
    void setCompact(bool on)
    {
        m_compact = on;
        setToolTip(on ? text() : QString());
        placeBadge();
        update();
    }
    // The selection bar, as the sidebar's items have it. The list draws one
    // bar for all its rows (it slides between them) and tells them so.
    QRect barRect() const { return QRect(1, (height() - 18) / 2, 3, 18); }
    void setExternalBar(bool external)
    {
        m_externalBar = external;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        if (isChecked()) {
            p.setPen(Qt::NoPen);
            p.setBrush(accentGradient(r, 46));
            p.drawRoundedRect(r, radius::control, radius::control);
            if (!m_externalBar)
                NavItem::paintSelectionBar(p, barRect());
        } else if (underMouse()) {
            p.setPen(Qt::NoPen);
            p.setBrush(color::bg_3);
            p.drawRoundedRect(r, radius::control, radius::control);
        }
        p.drawPixmap(QRect(m_compact ? (width() - 16) / 2 : 12, (height() - 16) / 2, 16, 16),
                     iconPixmap(m_icon, 16, isChecked() ? color::accent_hi : color::fg_4, devicePixelRatioF()));
        if (m_compact)
            return;
        p.setPen(isChecked() ? color::fg_0 : color::fg_2);
        p.setFont(isChecked() ? font::bodyStrong() : font::body());
        const int right = m_badge && m_badge->isVisible() ? m_badge->width() + 16 : 8;
        p.drawText(rect().adjusted(38, 0, -right, 0), Qt::AlignVCenter | Qt::AlignLeft, text());
    }
    void resizeEvent(QResizeEvent* e) override
    {
        QAbstractButton::resizeEvent(e);
        placeBadge();
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Down) {
            focusNextPrevChild(e->key() == Qt::Key_Down);
            return;
        }
        QAbstractButton::keyPressEvent(e);
    }

private:
    QString m_icon;
    Badge* m_badge = nullptr;
    bool m_compact = false;
    bool m_externalBar = false;
};

// The section list's one selection bar: it slides from row to row, like the sidebar's.
class SectionBar : public QWidget {
public:
    explicit SectionBar(QWidget* parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setProperty("fwNoRing", true);
        hide();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        NavItem::paintSelectionBar(p, QRectF(rect()));
    }
};

} // namespace

SettingsPage::SettingsPage(AppContext& ctx, QWidget* parent)
    : Page(ctx, QStringLiteral("settings"), FW_TR("Settings"),
           FW_TR("Defaults for new jobs; a job page can override any of them."), parent)
{
    m_startLanguage = m_ctx.settings().value(general::keys::kLanguage).toString();
    m_search = textInput(FW_TR("Search all settings"), false, this);
    m_search->setFixedWidth(340);
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(FW_TR("Search settings"));
    m_search->setToolTip(FW_TR("Search every section (Ctrl+F)"));
    m_search->addAction(icon(QStringLiteral("search"), color::fg_4), QLineEdit::LeadingPosition);
    header()->addAction(m_search);
    auto* find = new QShortcut(QKeySequence::Find, this);
    find->setContext(Qt::WindowShortcut); // only while this page is the visible one
    connect(find, &QShortcut::activated, this, [this] {
        m_search->setFocus(Qt::ShortcutFocusReason);
        m_search->selectAll();
    });
    // Every keystroke rebuilds the matching rows: wait for a short pause first.
    m_searchDelay = new QTimer(this);
    m_searchDelay->setSingleShot(true);
    m_searchDelay->setInterval(150);
    connect(m_search, &QLineEdit::textChanged, m_searchDelay, qOverload<>(&QTimer::start));
    connect(m_searchDelay, &QTimer::timeout, this, [this] { setSearchText(m_search->text()); });

    auto* columns = new QWidget(this);
    m_columns = columns;
    columns->installEventFilter(this);
    auto* h = hbox(columns, space::xxl);
    h->setAlignment(Qt::AlignTop);
    m_sectionList = new QWidget(columns);
    m_sectionList->setFixedWidth(size::settingsList + 16);
    m_sectionLayout = vbox(m_sectionList, 2);
    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    m_sections = m_ctx.schema().sections();
    if (!m_sections.contains(QStringLiteral("Tools")) && !m_ctx.dependencies().ids().empty())
        m_sections << QStringLiteral("Tools");
    // Keep canonical order.
    QStringList ordered;
    for (const QString& s : SettingsSchema::sectionOrder())
        if (m_sections.contains(s))
            ordered << s;
    m_sections = ordered;
    for (const QString& s : m_sections) {
        auto* wrap = new QWidget(m_sectionList);
        auto* wl = hbox(wrap, 0);
        auto* b = new SectionButton(shownSection(s), sectionIcon(s), wrap);
        auto* badge = new Badge(QString(), Tone::Warn, b);
        badge->setCount(true);
        badge->setIconName({});
        badge->hide();
        b->setBadge(badge);
        b->setExternalBar(true);
        wrap->installEventFilter(this); // the selection bar follows its row
        wl->addWidget(b);
        group->addButton(b);
        m_sectionButtons.insert(s, b);
        m_sectionBadges.insert(s, badge);
        m_sectionLayout->addWidget(wrap);
        connect(b, &QAbstractButton::clicked, this, [this, s] { showSection(s); });
    }
    // One selection bar for all rows, over them: it slides to the section that is opened.
    m_sectionBar = new SectionBar(m_sectionList);
    m_sectionBarY = new motion::Value(m_sectionList, 0.0, motion::base);
    m_sectionBarY->onChange([this] { m_sectionBar->move(m_sectionBar->x(), qRound(m_sectionBarY->value())); });
    // Whole-app actions live under the list, away from the section's own.
    m_sectionLayout->addSpacing(space::lg);
    m_sectionLayout->addWidget(hairline(m_sectionList));
    m_sectionLayout->addSpacing(space::sm);
    auto* import = m_importBtn = button(FW_TR("Import legacy settings…"), ButtonVariant::Subtle, QStringLiteral("file_down"), m_sectionList);
    import->setToolTip(FW_TR("Take over the values of the Python edition's settings.json"));
    import->setStyleSheet(QStringLiteral("text-align:left;"));
    auto* resetAllBtn = m_resetAllBtn = button(FW_TR("Reset all settings…"), ButtonVariant::Subtle, QStringLiteral("undo"), m_sectionList);
    resetAllBtn->setStyleSheet(QStringLiteral("text-align:left;"));
    resetAllBtn->setToolTip(FW_TR("Put every setting back to its default"));
    m_sectionLayout->addWidget(import);
    m_sectionLayout->addWidget(resetAllBtn);
    m_sectionLayout->addStretch(1);
    connect(import, &QPushButton::clicked, this, &SettingsPage::importLegacy);
    connect(resetAllBtn, &QPushButton::clicked, this, &SettingsPage::resetAll);
    h->addWidget(m_sectionList, 0, Qt::AlignTop);

    m_pane = new QWidget(columns);
    m_pane->setMaximumWidth(size::formMax);
    m_pane->installEventFilter(this);
    m_paneLayout = vbox(m_pane, space::lg);
    auto* secHeader = new QWidget(m_pane);
    auto* sh = hbox(secHeader, space::lg);
    sh->setAlignment(Qt::AlignTop);
    auto* scol = new QVBoxLayout;
    scol->setSpacing(2);
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(space::sm);
    m_secTitle = heading({}, TextStyle::Title, secHeader);
    titleRow->addWidget(m_secTitle);
    m_secChanged = new Badge({}, Tone::Info, secHeader);
    m_secChanged->setIconName(QStringLiteral("advanced"));
    m_secChanged->hide();
    titleRow->addWidget(m_secChanged, 0, Qt::AlignVCenter);
    titleRow->addStretch(1);
    scol->addLayout(titleRow);
    m_secCaption = caption({}, secHeader);
    m_secCaption->setWordWrap(true);
    scol->addWidget(m_secCaption);
    // Tools lists components by the path they run from: say what that folder is.
    m_secLink = new Link(FW_TR("Bundled components live in the unpacked program folder (Settings › Storage shows where)."), secHeader);
    m_secLink->setFont(font::caption());
    m_secLink->setWordWrap(true);
    m_secLink->hide();
    // Queued: opening Storage rebuilds the cards under the pointer.
    connect(m_secLink, &Link::activated, this, [this] { navigateWith({{QStringLiteral("section"), QStringLiteral("Storage")}, {QStringLiteral("show"), QStringLiteral("locations")}}); },
            Qt::QueuedConnection);
    scol->addWidget(m_secLink);
    sh->addLayout(scol, 1);
    m_resetSection = button(FW_TR("Reset section"), ButtonVariant::Subtle, QStringLiteral("undo"), secHeader);
    m_resetSection->setToolTip(FW_TR("Put this section's settings back to their defaults"));
    sh->addWidget(m_resetSection, 0, Qt::AlignVCenter);
    m_paneLayout->addWidget(secHeader);
    m_cards = new QWidget(m_pane);
    m_cardsLayout = vbox(m_cards, space::lg);
    m_paneLayout->addWidget(m_cards);
    h->addWidget(m_pane, 1, Qt::AlignTop);
    content()->addWidget(columns);
    connect(m_resetSection, &QPushButton::clicked, this, [this] {
        if (m_section == QLatin1String("Tools") && m_ctx.schema().inSection(m_section).empty())
            return;
        // Stored values that would change: asked about first, as Reset all is.
        // With none there is only a refused entry to take back: no question.
        int changed = 0;
        bool paths = false;
        for (const SettingDef* d : m_ctx.schema().inSection(m_section)) {
            if (m_ctx.settings().isDefault(d->key))
                continue;
            ++changed;
            paths = paths || d->type == SettingType::DirPath || d->type == SettingType::FilePath;
        }
        if (changed > 0
            && !Sheet::confirm(window(), changed == 1 ? FW_TR("Reset 1 setting?") : FW_TR("Reset %1 settings?").arg(changed),
                               (paths ? FW_TR("The settings you changed in %1 go back to their defaults. Paths you entered there are cleared too. "
                                              "Jobs already in the queue keep their settings.")
                                      : FW_TR("The settings you changed in %1 go back to their defaults. Jobs already in the queue keep their settings."))
                                   .arg(shownSection(m_section)),
                               FW_TR("Keep settings"), FW_TR("Reset section"), true, Sheet::Icon::Warning))
            return;
        m_ctx.settings().resetSection(m_section);
        (void)m_ctx.settings().save();
        rebuild(); // also forgets the refused values of the rows it replaces
        emit toastRequested(Tone::Ok, FW_TR("%1 reset").arg(shownSection(m_section)), FW_TR("Defaults are back for this section."));
    });

    connect(&m_ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant&) {
        refreshRow(key);
        refreshDependencyStates();
        updateSectionHeader();
        if (key == general::keys::kLanguage)
            updateLanguageBar();
        if (key == general::keys::kTempFolder) {
            updateTempUsage(); // "Temporary files" names and measures that folder too
            if (m_tempLocation) {
                m_tempLocation->setFullText(nativePath(tempLocation()));
                m_tempLocation->setToolTip(plainTip(nativePath(tempLocation())));
            }
        }
    });
    connect(&m_ctx.settings(), &SettingsStore::resetDone, this, &SettingsPage::rebuild);
    connect(&m_ctx, &AppContext::dependenciesChecked, this, [this] {
        updateSectionBadges();
        emit attentionChanged();
    });
    if (!m_sections.isEmpty())
        showSection(m_sections.first());
    updateSectionBadges();
}

QString SettingsPage::sectionCaption(const QString& section) const
{
    static const QHash<QString, QString> captions = {
        {QStringLiteral("General"), FW_TR_NOOP("How the app behaves.")},
        {QStringLiteral("Build"), FW_TR_NOOP("Defaults for Build image jobs.")},
        {QStringLiteral("FPKG"), FW_TR_NOOP("Defaults for FPKG packages.")},
        {QStringLiteral("PFS"), FW_TR_NOOP("Defaults for PFS and FFPFSC images.")},
        {QStringLiteral("PlayGo"), FW_TR_NOOP("PlayGo chunk layout and AMPR index.")},
        {QStringLiteral("DLC"), FW_TR_NOOP("DLC packages from DLC INIs and DLC .pkg files.")},
        {QStringLiteral("Firmware and SDK"), FW_TR_NOOP("Firmware targets and SDK versions.")},
        {QStringLiteral("Tools"), FW_TR_NOOP("External tools the app uses. Nothing is downloaded; a private build includes your own copies.")},
        {QStringLiteral("Performance"), FW_TR_NOOP("Threads, buffers and the queue.")},
        {QStringLiteral("Storage"), FW_TR_NOOP("Folders and free-space safety.")},
        {QStringLiteral("Verification"), FW_TR_NOOP("What happens after a build.")},
        {QStringLiteral("PS5 connection"), FW_TR_NOOP("How the app reaches the console.")},
        {QStringLiteral("Logging"), FW_TR_NOOP("Log files and problem reports.")},
        {QStringLiteral("Advanced"), FW_TR_NOOP("Troubleshooting; change these only when asked to.")},
    };
    if (const auto it = captions.constFind(section); it != captions.constEnd())
        return i18n::translate(it.value());
    return FW_TR("Defaults for %1.").arg(shownSection(section));
}

void SettingsPage::showSection(const QString& section)
{
    if (!m_sections.contains(section))
        return;
    m_section = section;
    if (auto* b = m_sectionButtons.value(section))
        b->setChecked(true);
    syncSectionBar(true);
    if (!m_search->text().trimmed().isEmpty())
        m_search->clear();
    m_searchDelay->stop(); // the rebuild below is the one; no second one 150 ms later
    rebuild();
}

void SettingsPage::setShowAdvanced(bool on)
{
    if (m_advanced == on)
        return;
    m_advanced = on;
    if (m_advancedFold && m_advancedFold->isExpanded() != on)
        m_advancedFold->setExpanded(on);
}

void SettingsPage::setSearchText(const QString& text)
{
    if (m_search->text() != text)
        m_search->setText(text);
    m_searchDelay->stop();
    rebuild();
}

bool SettingsPage::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Resize) {
        if (watched == m_pane)
            fitLabels();
        else if (watched == m_columns)
            setCompactList(m_columns->width() < 860);
    }
    // A row of the section list moved or changed size (first layout, the
    // icon-only list): the bar follows at once.
    if ((event->type() == QEvent::Move || event->type() == QEvent::Resize) && watched != m_sectionBar
        && watched->parent() == m_sectionList)
        syncSectionBar(false);
    return Page::eventFilter(watched, event);
}

// Puts the selection bar on the open section's row; `animate`: it slides
// there from where it is.
void SettingsPage::syncSectionBar(bool animate)
{
    if (!m_sectionBar)
        return;
    auto* cur = static_cast<SectionButton*>(m_sectionButtons.value(m_section));
    if (!cur || !cur->isChecked()) {
        m_sectionBar->hide();
        return;
    }
    const QRect r = cur->barRect().translated(cur->mapTo(m_sectionList, QPoint(0, 0)));
    const bool slide = animate && !m_sectionBar->isHidden();
    m_sectionBar->resize(r.size());
    if (slide) {
        m_sectionBar->move(r.x(), m_sectionBar->y());
        m_sectionBarY->animateTo(r.y());
    } else {
        m_sectionBarY->jumpTo(r.y());
        m_sectionBar->move(r.topLeft());
    }
    m_sectionBar->raise();
    m_sectionBar->show();
}

void SettingsPage::setCompactList(bool compact)
{
    if (compact == m_compactList)
        return;
    m_compactList = compact;
    m_sectionList->setFixedWidth(compact ? 44 : size::settingsList + 16);
    for (QAbstractButton* b : std::as_const(m_sectionButtons))
        static_cast<SectionButton*>(b)->setCompact(compact);
    m_importBtn->setText(compact ? QString() : FW_TR("Import legacy settings…"));
    m_resetAllBtn->setText(compact ? QString() : FW_TR("Reset all settings…"));
    m_importBtn->setToolTip(compact ? FW_TR("Import legacy settings…") : FW_TR("Take over the values of the Python edition's settings.json"));
    m_resetAllBtn->setToolTip(compact ? FW_TR("Reset all settings…") : FW_TR("Put every setting back to its default"));
    updateSectionBadges();
}

// SettingRow labels word-wrap; inside a row they would otherwise shrink to a
// narrow column ("Ask before closing / while jobs run"). Keep each label on
// one line up to ~40 % of the pane.
void SettingsPage::fitLabels()
{
    const int cap = std::max(140, int((m_pane->width() - 2 * space::lg) * 0.42));
    for (SettingRow* row : std::as_const(m_rows)) {
        QLabel* l = row->labelWidget();
        const int natural = l->fontMetrics().horizontalAdvance(l->text()) + 2;
        l->setMinimumWidth(std::min(natural, cap));
    }
}

SettingRow* SettingsPage::addRow(const SettingDef& def, QWidget* parent, QVBoxLayout* layout, bool divider)
{
    auto* row = new SettingRow(def, parent);
    row->setDivider(divider);
    row->setRestartNote(def.restartRequired);
    m_rows.insert(def.key, row);
    const QString key = def.key;
    connect(row->editor(), &SettingEditor::edited, this, [this, key](const QVariant& v) { applyValue(key, v); });
    layout->addWidget(row);
    refreshRow(key);
    return row;
}

void SettingsPage::rebuild()
{
    while (QLayoutItem* it = m_cardsLayout->takeAt(0)) {
        // A rebuild can start inside a click handler of a widget on one of
        // these cards (a "How to fix…" link, the Import button): the cards
        // leave the page now and are destroyed once that event is over.
        if (QWidget* w = it->widget()) {
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete it;
    }
    m_rows.clear();
    // A refused value is only on screen in the editor that was just replaced:
    // the new rows show the stored values, so its error has nothing to point at.
    if (!m_errors.isEmpty()) {
        m_errors.clear();
        QTimer::singleShot(0, this, [this] {
            emit attentionChanged();
            emit statusContextChanged();
        });
    }
    m_advancedKeys.clear();
    m_advancedFold = nullptr;
    m_fixBar = nullptr;
    m_deps = nullptr;
    m_tempUsage = nullptr;
    m_tempClear = nullptr;
    const QString query = m_search->text().trimmed();
    if (!query.isEmpty()) {
        m_secTitle->setText(FW_TR("Search results"));
        m_secChanged->hide();
        m_resetSection->hide();
        m_secLink->hide();
        int n = 0;
        for (const QString& s : m_sections) {
            std::vector<const SettingDef*> hits;
            for (const SettingDef* d : m_ctx.schema().inSection(s))
                // The English texts and the shown ones: a translated interface finds both words.
                if (d->label.contains(query, Qt::CaseInsensitive) || d->description.contains(query, Qt::CaseInsensitive) || d->key.contains(query, Qt::CaseInsensitive)
                    || shownLabel(*d).contains(query, Qt::CaseInsensitive) || shownDescription(*d).contains(query, Qt::CaseInsensitive))
                    hits.push_back(d);
            if (hits.empty())
                continue;
            auto* card = new Card(m_cards);
            card->body()->setSpacing(0);
            auto* open = new Link(FW_TR("Open section"), card);
            card->setTitle(shownSection(s), open);
            connect(open, &Link::activated, this, [this, s] { showSection(s); }, Qt::QueuedConnection);
            for (size_t i = 0; i < hits.size(); ++i) {
                addRow(*hits[i], card, card->body(), i + 1 < hits.size());
                ++n;
            }
            m_cardsLayout->addWidget(card);
        }
        if (n == 0) {
            auto* none = new EmptyState(QStringLiteral("search"), FW_TR("No setting matches"),
                                        FW_TR("Try a shorter word, or browse the sections."), m_cards);
            // Queued: clearing the search rebuilds the cards, this button among them.
            connect(none->addAction(FW_TR("Clear search")), &QPushButton::clicked, this, [this] { m_search->clear(); },
                    Qt::QueuedConnection);
            m_cardsLayout->addWidget(none);
        }
        m_secCaption->setText(n == 0   ? FW_TR("Nothing matches “%1”.").arg(query)
                              : n == 1 ? FW_TR("%1 setting matches “%2”.").arg(n).arg(query)
                                       : FW_TR("%1 settings match “%2”.").arg(n).arg(query));
        refreshDependencyStates();
        fitLabels();
        return;
    }
    m_resetSection->show();
    buildSection(m_section);
    updateSectionHeader();
    refreshDependencyStates();
    fitLabels();
}

void SettingsPage::buildSection(const QString& section)
{
    const auto defs = m_ctx.schema().inSection(section);
    QStringList groups;
    bool allAdvanced = !defs.empty();
    for (const SettingDef* d : defs) {
        if (!groups.contains(d->group))
            groups << d->group;
        allAdvanced = allAdvanced && d->advanced;
    }
    // Rare settings fold into one "Advanced" disclosure at the end of the
    // section; a section that is advanced as a whole shows them directly.
    std::vector<std::pair<QString, std::vector<const SettingDef*>>> advanced;
    QStringList depIds;
    for (const QString& g : groups) {
        std::vector<const SettingDef*> rows, rare;
        for (const SettingDef* d : defs) {
            if (d->group != g)
                continue;
            if (!d->dependencyId.isEmpty() && !depIds.contains(d->dependencyId))
                depIds << d->dependencyId;
            (d->advanced && !allAdvanced ? rare : rows).push_back(d);
        }
        if (!rare.empty())
            advanced.emplace_back(g.isEmpty() ? section : g, rare);
        if (rows.empty())
            continue;
        auto* card = new Card(m_cards);
        card->body()->setSpacing(0);
        card->setTitle(shownSection(g.isEmpty() ? section : g));
        for (size_t i = 0; i < rows.size(); ++i) {
            addRow(*rows[i], card, card->body(), i + 1 < rows.size());
            if (rows[i]->key == general::keys::kLanguage)
                addLanguageExtras(card, card->body());
        }
        m_cardsLayout->addWidget(card);
    }
    if (section == QLatin1String("Storage")) {
        addLocationsCard();
        // Temporary files: the app's own temp root and, when one is chosen,
        // the temporary folder of this section (jobs run there then).
        auto* card = new Card(m_cards);
        card->body()->setSpacing(0);
        card->setTitle(FW_TR("Temporary files"));
        auto* trow = new QWidget(card);
        auto* th = hbox(trow, space::lg, QMargins(0, 6, 0, 6));
        auto* tcol = new QVBoxLayout;
        tcol->setSpacing(2);
        tcol->addWidget(label(FW_TR("Files left by jobs"), TextStyle::Body, color::fg_1, trow));
        m_tempUsage = caption(FW_TR("Measuring…"), trow);
        tcol->addWidget(m_tempUsage);
        th->addLayout(tcol, 1);
        m_tempClear = button(FW_TR("Clear"), ButtonVariant::Secondary, QStringLiteral("trash"), trow);
        m_tempClear->setEnabled(false);
        connect(m_tempClear, &QPushButton::clicked, this, [this] {
            if (m_ctx.queue().isBusy()) {
                emit toastRequested(Tone::Warn, FW_TR("Jobs are running"), FW_TR("Temporary files are cleared when the queue is idle."));
                return;
            }
            int n = Paths::sweepStaleTemp(0);
            if (const QString custom = m_ctx.settings().value(general::keys::kTempFolder).toString().trimmed(); !custom.isEmpty())
                n += Paths::sweepStaleTempIn(custom, 0);
            emit toastRequested(Tone::Ok, FW_TR("Temporary files cleared"), n == 1 ? FW_TR("%1 folder removed").arg(n) : FW_TR("%1 folders removed").arg(n));
            updateTempUsage();
        });
        th->addWidget(m_tempClear);
        card->body()->addWidget(trow);
        m_cardsLayout->addWidget(card);
        updateTempUsage();
    }
    if (section == QLatin1String("Advanced")) {
        auto* card = new Card(m_cards);
        card->setTitle(FW_TR("Import settings from the old app"));
        auto* note = caption(FW_TR("Takes over every value this app understands. Nothing is deleted."), card);
        note->setToolTip(FW_TR("Reads the Python edition's settings.json; values the native app does not use are skipped."));
        card->body()->addWidget(note);
        auto* pf = new PathField(PathField::Mode::OpenFile, card);
        pf->setFilter(FW_TR("JSON files") + QStringLiteral(" (*.json)"));
        pf->setDialogTitle(FW_TR("Choose the old settings.json"));
        const QString legacy = QDir::home().filePath(QStringLiteral(".exfat_builder_settings.json"));
        if (QFile::exists(legacy))
            pf->setPath(legacy);
        else
            pf->setPlaceholder(nativePath(legacy));
        card->body()->addWidget(field(FW_TR("Old settings file"), pf, {}, card));
        auto* row = new QWidget(card);
        auto* rh = hbox(row, space::sm);
        rh->addStretch(1);
        auto* go = button(FW_TR("Import"), ButtonVariant::Secondary, QStringLiteral("file_down"), row);
        connect(go, &QPushButton::clicked, this, [this, pf] {
            if (pf->path().isEmpty()) {
                pf->setError(FW_TR("Choose the old settings file first."));
                return;
            }
            pf->setError({});
            importLegacyFrom(pf->path());
        });
        rh->addWidget(go);
        card->body()->addWidget(row);
        m_cardsLayout->addWidget(card);
    }
    if (section == QLatin1String("Tools")) {
        auto* card = new Card(m_cards);
        card->body()->setSpacing(0);
        auto* badge = new Badge(FW_TR("Checking…"), Tone::Neutral, card);
        auto* row = card->setTitle(FW_TR("Components and tools"), badge);
        auto* again = button(FW_TR("Check again"), ButtonVariant::Subtle, QStringLiteral("refresh"), card);
        setButtonSmall(again, true);
        auto* copyReport = new IconButton(QStringLiteral("copy"), FW_TR("Copy report"), IconButton::Size::Small, card);
        copyReport->setToolTip(FW_TR("Copy the tool report (JSON)"));
        row->addWidget(again);
        row->addWidget(copyReport);
        m_deps = new DependencyList(m_ctx, {}, card);
        card->body()->addWidget(m_deps);
        if (m_deps->isEmpty())
            card->body()->addWidget(caption(FW_TR("No external tools are registered in this build."), card));
        connect(again, &QPushButton::clicked, m_deps, &DependencyList::checkAgain);
        connect(copyReport, &QToolButton::clicked, this, [this] {
            QGuiApplication::clipboard()->setText(m_deps->report());
            emit toastRequested(Tone::Ok, FW_TR("Report copied"), FW_TR("Dependency statuses are on the clipboard as JSON."));
        });
        connect(m_deps, &DependencyList::changed, this, [this, badge] {
            if (m_deps->isEmpty()) {
                badge->set(FW_TR("None"), Tone::Neutral);
                return;
            }
            const int ready = m_deps->readyCount(), total = m_deps->total();
            badge->set(FW_TR("%1 of %2 ready").arg(ready).arg(total), ready == total ? Tone::Ok : Tone::Warn);
        });
        connect(m_deps, &DependencyList::fixRequested, this, [this](const QString&, const QString& how, const QString& settingKey) {
            navigateWith({{QStringLiteral("key"), settingKey}, {QStringLiteral("howToFix"), how}});
        });
        m_deps->refresh();
        m_cardsLayout->insertWidget(0, card);
    } else if (!depIds.isEmpty()) {
        auto* card = new Card(m_cards);
        card->body()->setSpacing(0);
        auto* again = button(FW_TR("Detect again"), ButtonVariant::Subtle, QStringLiteral("refresh"), card);
        setButtonSmall(again, true);
        card->setTitle(FW_TR("Tools this section uses"), again);
        auto* list = new DependencyList(m_ctx, depIds, card);
        card->body()->addWidget(list);
        connect(again, &QPushButton::clicked, list, &DependencyList::checkAgain);
        // Queued: going to Tools rebuilds this page, and the link that asked is on this card.
        connect(list, &DependencyList::fixRequested, this, [this](const QString&, const QString& how, const QString& settingKey) {
            navigateWith({{QStringLiteral("section"), QStringLiteral("Tools")}, {QStringLiteral("key"), settingKey}, {QStringLiteral("howToFix"), how}});
        }, Qt::QueuedConnection);
        m_cardsLayout->addWidget(card);
    }
    if (!advanced.empty()) {
        QStringList names;
        for (const auto& entry : advanced)
            names << shownSection(entry.first);
        auto* fold = new Disclosure(FW_TR("Advanced"), elideMiddle(names.join(QStringLiteral(", ")), 80), m_cards);
        fold->setObjectName(QStringLiteral("settings-advanced"));
        fold->body()->setSpacing(0);
        for (size_t gi = 0; gi < advanced.size(); ++gi) {
            const auto& [g, list] = advanced[gi];
            if (advanced.size() > 1) {
                auto* head = label(shownSection(g), TextStyle::Overline, color::fg_4, fold);
                head->setContentsMargins(0, gi == 0 ? 2 : space::lg, 0, 2);
                fold->body()->addWidget(head);
            }
            for (size_t i = 0; i < list.size(); ++i) {
                addRow(*list[i], fold, fold->body(), i + 1 < list.size());
                m_advancedKeys << list[i]->key;
            }
        }
        fold->setExpanded(m_advanced);
        connect(fold, &Disclosure::toggled, this, [this](bool on) {
            m_advanced = on;
            if (on)
                fitLabels();
        });
        m_advancedFold = fold;
        m_cardsLayout->addWidget(fold);
    }
    if (m_cardsLayout->count() == 0)
        m_cardsLayout->addWidget(new EmptyState(QStringLiteral("settings"), FW_TR("Nothing to set here yet"),
                                                FW_TR("The modules that use this section are not part of this build."), m_cards));
}

namespace {

// What Settings › Storage lists. Screenshots show an ordinary installation,
// not the folders of the machine that renders them.
struct StorageLocations {
    QString launcherExe; // "" when the program was started directly
    QString folderExe;   // the folder edition's exe when started directly from a release layout (GitHub #78)
    QString program, data, logs, temp, cache;
    QString marker;      // portable.txt, whether it exists or not
    bool portable = false;
    bool markerExists = false;
    bool overridden = false;
};

StorageLocations storageLocations(const AppContext& ctx, const QString& temp)
{
    StorageLocations l;
    if (qApp->property("fwScreenshotMode").toBool()) {
        const QString local = QStringLiteral("C:/Users/you/AppData/Local/kerrdec97/");
        l.launcherExe = QStringLiteral("C:/Users/you/Downloads/exFAT Image Builder.exe");
        l.program = local + QStringLiteral("exFAT Image Builder/runtime/5.0.2-0f3a9c21d4b7");
        l.data = QStringLiteral("C:/Users/you/AppData/Roaming/kerrdec97/exFAT Image Builder Farewell");
        l.logs = local + QStringLiteral("exFAT Image Builder Farewell/logs");
        l.temp = local + QStringLiteral("exFAT Image Builder Farewell/temp");
        l.cache = local + QStringLiteral("exFAT Image Builder Farewell/cache");
        l.marker = QStringLiteral("C:/Users/you/Downloads/portable.txt");
        return l;
    }
    l.launcherExe = Paths::launcherExe();
    if (l.launcherExe.isEmpty() && portable::isFolderEdition(QCoreApplication::applicationDirPath()))
        l.folderExe = QDir::cleanPath(QCoreApplication::applicationFilePath());
    l.program = Paths::programDir();
    l.data = Paths::dataDir();
    const QString logFile = ctx.logFilePath(); // the log folder in use (Settings › Logging may name another)
    l.logs = logFile.isEmpty() ? Paths::logsDir() : QFileInfo(logFile).absolutePath();
    l.temp = temp;
    l.cache = Paths::cacheDir();
    l.marker = Paths::portableMarkerFile();
    l.portable = Paths::isPortable();
    l.markerExists = !l.marker.isEmpty() && QFileInfo::exists(l.marker);
    l.overridden = Paths::isOverridden();
    return l;
}

} // namespace

QString SettingsPage::tempLocation() const
{
    const QString custom = m_ctx.settings().value(general::keys::kTempFolder).toString().trimmed();
    return !custom.isEmpty() && QFileInfo(custom).isDir() ? custom : Paths::tempRoot();
}

void SettingsPage::addLocationsCard()
{
    const StorageLocations at = storageLocations(m_ctx, tempLocation());
    auto* card = new Card(m_cards);
    card->setObjectName(QStringLiteral("locations-card"));
    card->body()->setSpacing(0);
    card->setTitle(FW_TR("Where the app keeps its files"));
    m_locationsCard = card;

    // One location: its name, the path in mono (cut in the middle, whole in the tooltip) and "Show in folder".
    const auto showButton = [this, card](const QString& what, ElideLabel* path) {
        auto* show = button(FW_TR("Show in folder"), ButtonVariant::Subtle, QStringLiteral("folder_open"), card);
        setButtonSmall(show, true);
        show->setAccessibleName(FW_TR("Show %1 in folder").arg(what));
        const QPointer<ElideLabel> label(path);
        connect(show, &QPushButton::clicked, this, [this, label] {
            if (label && !openInFileManager(label->fullText()))
                emit toastRequested(Tone::Warn, FW_TR("The folder is not there"), label->fullText());
        });
        return show;
    };
    const auto pathLabel = [card](const QString& path) {
        auto* p = new ElideLabel(nativePath(path), Qt::ElideMiddle, card);
        p->setObjectName(QStringLiteral("locationPath"));
        p->setFont(font::mono());
        setTextColor(p, color::fg_3);
        p->setToolTip(plainTip(nativePath(path)));
        return p;
    };
    const auto addLocation = [&](const QString& key, const QString& name, const QString& path) {
        auto* row = new QWidget(card);
        row->setObjectName(QStringLiteral("location-") + key);
        auto* h = hbox(row, space::lg, QMargins(0, 8, 0, 8));
        auto* col = new QVBoxLayout;
        col->setSpacing(2);
        col->addWidget(label(name, TextStyle::Body, color::fg_1, row));
        ElideLabel* p = pathLabel(path);
        col->addWidget(p);
        h->addLayout(col, 1);
        h->addWidget(showButton(inSentence(name), p), 0, Qt::AlignVCenter);
        card->body()->addWidget(row);
        card->body()->addWidget(hairline(card));
        return p;
    };
    const bool fromLauncher = !at.launcherExe.isEmpty();
    if (fromLauncher)
        addLocation(QStringLiteral("exe"), FW_TR("The exe you start"), at.launcherExe);
    addLocation(QStringLiteral("program"), fromLauncher ? FW_TR("Unpacked program") : FW_TR("Program"), at.program);
    addLocation(QStringLiteral("data"), FW_TR("Settings, queue and history"), at.data);
    addLocation(QStringLiteral("logs"), FW_TR("Logs"), at.logs);
    m_tempLocation = addLocation(QStringLiteral("temp"), FW_TR("Temporary files"), at.temp);
    addLocation(QStringLiteral("cache"), FW_TR("Covers and downloaded updates"), at.cache);

    // Portable mode: how to move all of the above next to the exe.
    auto* prow = new QWidget(card);
    prow->setObjectName(QStringLiteral("portable-row"));
    auto* ph = hbox(prow, space::lg, QMargins(0, 10, 0, 2));
    auto* col = new QVBoxLayout;
    col->setSpacing(4);
    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(space::sm);
    titleRow->addWidget(label(FW_TR("Portable mode"), TextStyle::BodyStrong, color::fg_0, prow));
    const bool pending = at.markerExists && !at.portable; // the file is there, this run started without it
    auto* badge = new Badge(at.portable ? FW_TR("On") : pending ? FW_TR("After restart") : FW_TR("Off"),
                            at.portable ? Tone::Ok : pending ? Tone::Info : Tone::Neutral, prow);
    badge->setObjectName(QStringLiteral("portable-badge"));
    titleRow->addWidget(badge, 0, Qt::AlignVCenter);
    titleRow->addStretch(1);
    col->addLayout(titleRow);
    auto* what = caption(FW_TR("Keeps the unpacked program, settings, queue and logs in folders next to the exe, for example on an external drive."), prow);
    what->setWordWrap(true);
    col->addWidget(what);
    ph->addLayout(col, 1);
    const auto note = [prow, col](const QString& text) {
        auto* l = caption(text, prow);
        l->setObjectName(QStringLiteral("portable-note"));
        l->setWordWrap(true);
        col->addWidget(l);
    };
    if (at.portable || pending) {
        ElideLabel* marker = pathLabel(at.marker);
        col->addWidget(marker);
        note(at.portable ? FW_TR("To turn it off, delete portable.txt and start the app again. The folders next to the exe stay.")
                         : FW_TR("portable.txt is in place. Close the app and start it again to switch; delete the file to stay as you are."));
        ph->addWidget(showButton(QStringLiteral("portable.txt"), marker), 0, Qt::AlignTop);
    } else {
        auto* make = button(FW_TR("Make this copy portable…"), ButtonVariant::Secondary, {}, prow);
        make->setObjectName(QStringLiteral("make-portable"));
        const bool canMake = fromLauncher || !at.folderExe.isEmpty();
        make->setEnabled(canMake);
        if (!canMake) {
            const QString why = FW_TR("This copy was started without the single-file exe (a developer run), so there is no exe to make portable.");
            make->setToolTip(why);
            note(why);
        }
        connect(make, &QPushButton::clicked, this, &SettingsPage::makePortable);
        ph->addWidget(make, 0, Qt::AlignTop);
    }
    if (at.overridden && !qEnvironmentVariable("FW_DATA_DIR").isEmpty())
        note(FW_TR("FW_DATA_DIR names the data folder for this run and wins over portable mode while it is set."));
    card->body()->addWidget(prow);
    // Right under the section's first card (the folders one chooses), not below every other setting.
    m_cardsLayout->insertWidget(std::min(1, int(m_cardsLayout->count())), card);
}

Sheet* SettingsPage::buildPortableSheet(const portable::Plan& plan, QCheckBox*& copy)
{
    auto* s = new Sheet(window(), FW_TR("Make this copy portable?"), Sheet::Icon::Info, size::dialogWide);
    s->addText(FW_TR("This creates %1 next to %2.").arg(Sheet::monoSpan(QStringLiteral("portable.txt")), Sheet::monoSpan(nativePath(plan.exePath))));
    if (plan.unpacks)
        s->addText(FW_TR("From the next start the app unpacks itself to %1 and keeps settings, queue, history and logs in %2.")
                       .arg(Sheet::monoSpan(nativePath(plan.programDir)), Sheet::monoSpan(nativePath(plan.dataDir))));
    else
        s->addText(FW_TR("From the next start the app keeps settings, queue, history and logs in %1.").arg(Sheet::monoSpan(nativePath(plan.dataDir))));
    copy = s->addCheckbox(FW_TR("Copy my settings and queue"));
    copy->setChecked(true);
    copy->setToolTip(FW_TR("Settings, queue, history and the library list are copied; a file that is already in the new folder is kept."));
    s->addText(FW_TR("With the box ticked, your current settings, queue and history are copied there. The old folders are left "
                     "in place and can be deleted later. The app must be restarted to switch."));
    s->addButton(FW_TR("Cancel"), Sheet::Role::Safe);
    s->addButton(FW_TR("Make portable"), Sheet::Role::Primary);
    return s;
}

void SettingsPage::makePortable()
{
    const QString exe = Paths::launcherExe();
    portable::Plan plan;
    if (!exe.isEmpty())
        plan = portable::planFor(exe, Paths::dataDir());
    else if (portable::isFolderEdition(QCoreApplication::applicationDirPath()))
        plan = portable::planForFolderEdition(QCoreApplication::applicationFilePath(), Paths::dataDir());
    else
        return;
    QCheckBox* copy = nullptr;
    Sheet* s = buildPortableSheet(plan, copy);
    if (s->exec() != 1)
        return;
    const bool copyData = copy->isChecked(); // the sheet's widgets live until the event loop runs again
    (void)m_ctx.settings().save(); // what is copied is what is on disk
    const auto done = portable::makePortable(plan, copyData);
    if (!done.ok()) {
        Log(QStringLiteral("app")).warn(QStringLiteral("Portable mode was not set up: %1").arg(done.error().toLogString()));
        Sheet::message(window(), FW_TR("This copy was not made portable"), done.error().message, Sheet::Icon::Error);
        return;
    }
    Log(QStringLiteral("app")).info(QStringLiteral("Portable mode set up: %1 (%2 file(s) copied, %3 kept)")
                                        .arg(nativePath(plan.markerFile)).arg(done->copied.size()).arg(done->kept.size()));
    rebuild(); // the row now says "After restart"
    QString text = plan.unpacks ? FW_TR("Close the app and start %1 again. It unpacks itself next to the exe once (a few seconds) and uses the folders there from then on.")
                                      .arg(Sheet::monoSpan(nativePath(plan.exePath)))
                                : FW_TR("Close the app and start %1 again. It uses the folders there from then on.").arg(Sheet::monoSpan(nativePath(plan.exePath)));
    if (!done->copied.isEmpty())
        text += FW_TR("<br><br>Copied to %1: %2.").arg(Sheet::monoSpan(nativePath(plan.dataDir)), Sheet::monoSpan(done->copied.join(QStringLiteral(", "))));
    if (!done->kept.isEmpty())
        text += FW_TR("<br><br>Already there and kept as they were: %1.").arg(Sheet::monoSpan(done->kept.join(QStringLiteral(", "))));
    if (plan.unpacks)
        text += FW_TR("<br><br>The old folders are still in place: %1 and %2. Delete them once the portable copy works for you.")
                    .arg(Sheet::monoSpan(nativePath(plan.currentDataDir)), Sheet::monoSpan(nativePath(Paths::programDir())));
    else
        text += FW_TR("<br><br>The old folder is still in place: %1. Delete it once the portable copy works for you.")
                    .arg(Sheet::monoSpan(nativePath(plan.currentDataDir)));
    Sheet::message(window(), FW_TR("Portable from the next start"), text, Sheet::Icon::Info);
}

void SettingsPage::addLanguageExtras(QWidget* card, QVBoxLayout* layout)
{
    // Under the Language row: where the user's own catalogues go.
    auto* row = new QWidget(card);
    row->setObjectName(QStringLiteral("language-folder"));
    auto* h = hbox(row, space::lg, QMargins(0, 4, 0, 6));
    auto* col = new QVBoxLayout;
    col->setSpacing(2);
    col->addWidget(label(FW_TR("Your translation files"), TextStyle::Body, color::fg_1, row));
    auto* path = new ElideLabel(nativePath(i18n::userDir()), Qt::ElideMiddle, row);
    path->setFont(font::mono());
    setTextColor(path, color::fg_3);
    path->setToolTip(plainTip(nativePath(i18n::userDir())));
    col->addWidget(path);
    h->addLayout(col, 1);
    auto* show = button(FW_TR("Show in folder"), ButtonVariant::Subtle, QStringLiteral("folder_open"), row);
    show->setObjectName(QStringLiteral("language-folder-show"));
    setButtonSmall(show, true);
    show->setAccessibleName(FW_TR("Show the folder of your translation files"));
    show->setToolTip(FW_TR("Add a language or correct a translation: README.txt there says how."));
    connect(show, &QPushButton::clicked, this, [this] {
        const auto folder = language::prepareUserFolder(i18n::userDir());
        if (!folder.ok()) {
            emit toastRequested(Tone::Warn, FW_TR("The folder could not be prepared"), folder.error().message);
            return;
        }
        if (!openInFileManager(folder.value()))
            emit toastRequested(Tone::Warn, FW_TR("The folder is not there"), nativePath(folder.value()));
    });
    h->addWidget(show, 0, Qt::AlignVCenter);
    layout->addWidget(row);
    m_langBar = new InfoBar(Tone::Info, card);
    m_langBar->setObjectName(QStringLiteral("language-restart-bar"));
    connect(m_langBar->addLink(FW_TR("Restart now")), &Link::activated, this, &SettingsPage::restartForLanguage, Qt::QueuedConnection);
    layout->addWidget(m_langBar);
    updateLanguageBar();
}

void SettingsPage::updateLanguageBar()
{
    if (!m_langBar)
        return;
    // FW_LANG (tests, screenshots) wins over the setting: the setting as it
    // was when the page was made is then what this run would have used.
    const QString running = qEnvironmentVariableIsSet("FW_LANG") ? m_startLanguage : i18n::currentCode();
    const QString chosen = m_ctx.settings().value(general::keys::kLanguage).toString();
    if (chosen.compare(running, Qt::CaseInsensitive) == 0) {
        m_langBar->hide();
        return;
    }
    QString name = chosen;
    if (const SettingDef* d = m_ctx.schema().find(general::keys::kLanguage))
        for (const auto& c : d->choices)
            if (c.value == chosen)
                name = c.label; // the language's own name, never translated
    m_langBar->setTone(Tone::Info);
    m_langBar->setTitle(FW_TR("Restart the app to use %1").arg(name));
    m_langBar->setMessage({});
    m_langBar->show();
}

void SettingsPage::restartForLanguage()
{
    if (m_ctx.queue().isBusy()) {
        // The choice is saved already: it applies at the next start.
        if (m_langBar) {
            m_langBar->setTone(Tone::Warn);
            m_langBar->setMessage(FW_TR("Jobs are running, so the app does not restart now. The language is saved and is used the next time the app starts."));
        }
        return;
    }
    (void)m_ctx.settings().save();
    if (!language::restart(window()) && m_langBar) {
        m_langBar->setTone(Tone::Warn);
        m_langBar->setMessage(FW_TR("The app did not restart. The language is saved and is used the next time the app starts."));
    }
}

void SettingsPage::updateTempUsage()
{
    if (!m_tempUsage)
        return;
    QPointer<SettingsPage> self(this);
    const QString custom = m_ctx.settings().value(general::keys::kTempFolder).toString().trimmed();
    (void)QtConcurrent::run([self, custom] {
        // In the chosen folder only the job folders the app created count.
        const qint64 bytes = Paths::tempUsageBytes() + (custom.isEmpty() ? 0 : Paths::tempUsageBytesIn(custom));
        QMetaObject::invokeMethod(qApp, [self, bytes, custom] {
            if (!self || !self->m_tempUsage)
                return;
            if (custom != self->m_ctx.settings().value(general::keys::kTempFolder).toString().trimmed())
                return; // the folder was changed meanwhile: its own measurement follows
            const QString where = custom.isEmpty() ? nativePath(Paths::tempRoot())
                                                   : FW_TR("%1 and %2").arg(nativePath(Paths::tempRoot()), nativePath(custom));
            self->m_tempUsage->setText(bytes > 0 ? FW_TR("%1 in %2").arg(formatBytes(bytes), where)
                                                 : FW_TR("Nothing to clear in %1").arg(where));
            self->m_tempClear->setEnabled(bytes > 0);
        }, Qt::QueuedConnection);
    });
}

namespace {
// "Key label: reason" -> "reason"; a message without that form is shown whole.
QString refusalText(const QString& message)
{
    const QString reason = shownValidationReason(message);
    return reason.isEmpty() ? i18n::translate(message) : reason;
}
} // namespace

void SettingsPage::applyValue(const QString& key, const QVariant& value)
{
    SettingRow* row = m_rows.value(key);
    auto st = m_ctx.settings().setValue(key, value);
    if (!st.ok()) {
        m_errors.insert(key, st.error().message);
        if (row)
            row->setMessage(refusalText(st.error().message), Tone::Err);
        emit attentionChanged();
        emit statusContextChanged();
        return;
    }
    m_errors.remove(key);
    if (row)
        row->setMessage({}, Tone::Neutral);
    if (key == general::keys::kLanguage) {
        m_restartNeeded = true;
        updateLanguageBar(); // says so itself, with "Restart now"
    } else if (const SettingDef* d = m_ctx.schema().find(key); d && d->restartRequired) {
        m_restartNeeded = true;
        emit toastRequested(Tone::Info, FW_TR("Takes effect after a restart"), shownLabel(*d));
    }
    emit attentionChanged();
    emit statusContextChanged();
}

void SettingsPage::refreshRow(const QString& key)
{
    SettingRow* row = m_rows.value(key);
    if (!row)
        return;
    row->editor()->setValue(m_ctx.settings().value(key));
    if (m_errors.contains(key))
        row->setMessage(refusalText(m_errors.value(key)), Tone::Err);
}

void SettingsPage::refreshDependencyStates()
{
    for (auto it = m_rows.begin(); it != m_rows.end(); ++it) {
        const SettingDef* d = m_ctx.schema().find(it.key());
        if (!d || d->enabledWhenKey.isEmpty())
            continue;
        const QVariant actual = m_ctx.settings().value(d->enabledWhenKey);
        const bool enabled = actual == d->enabledWhenValue || actual.toString() == d->enabledWhenValue.toString();
        if (enabled)
            it.value()->setDisabledReason({});
        else {
            const SettingDef* other = m_ctx.schema().find(d->enabledWhenKey);
            it.value()->setDisabledReason(FW_TR("Only available when \"%1\" is %2.")
                                              .arg(other ? shownLabel(*other) : d->enabledWhenKey,
                                                   other ? inSentence(SettingRow::displayValue(*other, d->enabledWhenValue)) : d->enabledWhenValue.toString()));
        }
    }
}

void SettingsPage::updateSectionHeader()
{
    if (!m_search->text().trimmed().isEmpty())
        return;
    m_secTitle->setText(shownSection(m_section));
    int differ = 0;
    for (const SettingDef* d : m_ctx.schema().inSection(m_section))
        if (!m_ctx.settings().isDefault(d->key))
            ++differ;
    m_secCaption->setText(sectionCaption(m_section));
    m_secLink->setVisible(m_section == QLatin1String("Tools"));
    m_secChanged->setText(FW_TR("%1 changed").arg(differ));
    m_secChanged->setToolTip(differ == 1 ? FW_TR("%1 setting in this section differs from the defaults").arg(differ)
                                         : FW_TR("%1 settings in this section differ from the defaults").arg(differ));
    m_secChanged->setVisible(differ > 0);
    m_resetSection->setVisible(differ > 0);
    updateAdvancedSummary();
}

void SettingsPage::updateAdvancedSummary()
{
    if (!m_advancedFold)
        return;
    int differ = 0;
    for (const QString& key : std::as_const(m_advancedKeys))
        if (!m_ctx.settings().isDefault(key))
            ++differ;
    if (differ > 0)
        m_advancedFold->setSummary(FW_TR("%1 changed").arg(differ), Tone::Info);
    else
        m_advancedFold->setSummary(m_advancedKeys.size() == 1 ? FW_TR("%1 setting").arg(m_advancedKeys.size())
                                                              : FW_TR("%1 settings").arg(m_advancedKeys.size()));
}

void SettingsPage::updateSectionBadges()
{
    int problems = 0;
    for (const auto& info : m_ctx.dependencies().lastAll())
        if (info.status == DependencyStatus::Missing || info.status == DependencyStatus::Outdated || info.status == DependencyStatus::Error)
            ++problems;
    if (auto* b = m_sectionBadges.value(QStringLiteral("Tools"))) {
        b->setText(QString::number(problems));
        b->setVisible(problems > 0);
        if (auto* btn = m_sectionButtons.value(QStringLiteral("Tools"))) {
            static_cast<SectionButton*>(btn)->placeBadge();
            btn->update();
        }
    }
    emit statusContextChanged();
}

int SettingsPage::attentionCount() const
{
    // The status bar only nags about real problems: values that were refused,
    // and tools that are set up but broken or too old. Optional tools that are
    // simply not installed (e.g. Sony Publishing Tools for FPKG) are listed in
    // Settings › Tools and on the page that needs them, not here.
    int n = m_errors.size();
    for (const auto& info : m_ctx.dependencies().lastAll())
        if (info.status == DependencyStatus::Outdated || info.status == DependencyStatus::Error
            || (info.status == DependencyStatus::Missing && info.required))
            ++n;
    return n;
}

void SettingsPage::importLegacy()
{
    QString start = QDir::homePath();
    const QString legacy = QDir::home().filePath(QStringLiteral(".exfat_builder_settings.json"));
    if (QFile::exists(legacy))
        start = legacy;
    const QString path = QFileDialog::getOpenFileName(window(), FW_TR("Choose the legacy settings.json"), start, FW_TR("JSON files") + QStringLiteral(" (*.json)"));
    if (path.isEmpty())
        return;
    importLegacyFrom(path);
}

void SettingsPage::importLegacyFrom(const QString& path)
{
    QVariantMap before;
    for (const SettingDef* d : m_ctx.schema().all())
        before.insert(d->key, m_ctx.settings().value(d->key));
    auto r = m_ctx.settings().importLegacy(path);
    if (!r.ok()) {
        Sheet::message(window(), FW_TR("Nothing was imported"), r.error().message, Sheet::Icon::Error);
        return;
    }
    (void)m_ctx.settings().save();
    QStringList changed;
    for (const SettingDef* d : m_ctx.schema().all())
        if (m_ctx.settings().value(d->key) != before.value(d->key))
            changed << QStringLiteral("%1 › %2: %3").arg(shownSection(d->section), shownLabel(*d), SettingRow::displayValue(*d, m_ctx.settings().value(d->key)));
    rebuild();
    const QString title = r.value() == 1 ? FW_TR("%1 value imported").arg(r.value()) : FW_TR("%1 values imported").arg(r.value());
    if (changed.isEmpty())
        Sheet::message(window(), title, FW_TR("Every imported value already matched the current settings, so nothing changed."), Sheet::Icon::Info);
    else
    {
        // A long list is cut so the button stays on screen.
        const int kShown = 16;
        QString list = Sheet::monoLines(changed.mid(0, kShown)); // escapes the values
        if (changed.size() > kShown)
            list += FW_TR("<br>… and %1 more").arg(changed.size() - kShown);
        Sheet::message(window(), title, FW_TR("Changed:<br>%1").arg(list), Sheet::Icon::Info);
    }
}

void SettingsPage::resetAll()
{
    if (!Sheet::confirm(window(), FW_TR("Reset every setting?"),
                        FW_TR("All sections go back to their defaults. Tool paths you entered are cleared too. Jobs already in the queue keep their settings."),
                        FW_TR("Keep settings"), FW_TR("Reset all"), true, Sheet::Icon::Warning))
        return;
    m_ctx.settings().resetAll();
    (void)m_ctx.settings().save();
    m_errors.clear();
    rebuild();
    emit toastRequested(Tone::Ok, FW_TR("Settings reset"), FW_TR("Every setting is back to its default."));
}

void SettingsPage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("section")))
        showSection(args.value(QStringLiteral("section")).toString());
    const QString key = args.value(QStringLiteral("key")).toString();
    const QString how = args.value(QStringLiteral("howToFix")).toString();
    if (!key.isEmpty()) {
        if (const SettingDef* d = m_ctx.schema().find(key)) {
            if (d->section != m_section)
                showSection(d->section);
            if (d->advanced && !m_advanced)
                setShowAdvanced(true);
        }
    }
    if (!how.isEmpty()) {
        if (!m_fixBar) {
            m_fixBar = new InfoBar(Tone::Warn, m_cards);
            m_fixBar->setDismissable(true);
            m_cardsLayout->insertWidget(0, m_fixBar);
        }
        m_fixBar->setTitle(FW_TR("How to fix"));
        m_fixBar->setMessage(how);
        m_fixBar->show();
    }
    if (args.value(QStringLiteral("show")).toString() == QLatin1String("locations") && m_locationsCard) {
        // Once the new cards have their places.
        QTimer::singleShot(0, this, [this] {
            if (m_locationsCard)
                scrollArea()->ensureWidgetVisible(m_locationsCard, 0, 40);
        });
    }
    if (SettingRow* row = m_rows.value(key)) {
        scrollArea()->ensureWidgetVisible(row, 0, 80);
        window()->setAttribute(Qt::WA_KeyboardFocusChange, true);
        row->editor()->widget()->setFocus(Qt::TabFocusReason);
    }
}

void SettingsPage::refresh()
{
    m_ctx.checkDependenciesAsync();
    rebuild();
}

QString SettingsPage::statusContext() const
{
    return nativePath(m_ctx.settings().filePath());
}

QStringList SettingsPage::demoStates() const
{
    QStringList out;
    for (const QString& s : m_sections)
        out << QStringLiteral("section-") + s.toLower().replace(u' ', u'-');
    out << QStringLiteral("portable-sheet") << QStringLiteral("reset-dialog");
    return out;
}

void SettingsPage::applyDemoState(const QString& name)
{
    if (name == QLatin1String("portable-sheet")) {
        showSection(QStringLiteral("Storage"));
        QCheckBox* copy = nullptr;
        Sheet* s = buildPortableSheet(portable::planFor(QStringLiteral("E:/Apps/exFAT Image Builder.exe"),
                                                        QStringLiteral("C:/Users/you/AppData/Roaming/kerrdec97/exFAT Image Builder Farewell")),
                                      copy);
        s->open();
        m_demoSheet = s;
        return;
    }
    if (name == QLatin1String("reset-dialog")) {
        auto* s = new Sheet(window(), FW_TR("Reset every setting?"), Sheet::Icon::Warning);
        s->addText(FW_TR("All sections go back to their defaults. Tool paths you entered are cleared too. Jobs already in the queue keep their settings."));
        s->addButton(FW_TR("Keep settings"), Sheet::Role::Safe);
        s->addButton(FW_TR("Reset all"), Sheet::Role::Destructive);
        s->open();
        m_demoSheet = s;
        return;
    }
    for (const QString& sct : m_sections)
        if (name == QStringLiteral("section-") + QString(sct).toLower().replace(u' ', u'-')) {
            showSection(sct);
            // General shows the Advanced fold open; the other sections as people first see them.
            setShowAdvanced(sct == QLatin1String("General"));
        }
}

void SettingsPage::clearDemoState()
{
    if (auto* s = qobject_cast<Sheet*>(m_demoSheet.data()))
        s->close(0);
}

} // namespace fw::pages
