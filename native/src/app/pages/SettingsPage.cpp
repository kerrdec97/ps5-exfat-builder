#include "app/pages/SettingsPage.h"

#include "app/AppContext.h"
#include "app/ui/Components.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "core/Paths.h"
#include "engines/general/Module.h"

#include <QAction>
#include <QButtonGroup>
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
            p.setBrush(accentGradient(QRectF(0, (height() - 16) / 2.0, 3, 16)));
            p.drawRoundedRect(QRectF(0, (height() - 16) / 2.0, 3, 16), 1.5, 1.5);
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
};

} // namespace

SettingsPage::SettingsPage(AppContext& ctx, QWidget* parent)
    : Page(ctx, QStringLiteral("settings"), QStringLiteral("Settings"),
           QStringLiteral("Defaults for new jobs; a job page can override any of them."), parent)
{
    m_search = textInput(QStringLiteral("Search all settings"), false, this);
    m_search->setFixedWidth(340);
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(QStringLiteral("Search settings"));
    m_search->setToolTip(QStringLiteral("Search every section (Ctrl+F)"));
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
        auto* b = new SectionButton(s, sectionIcon(s), wrap);
        auto* badge = new Badge(QString(), Tone::Warn, b);
        badge->setCount(true);
        badge->setIconName({});
        badge->hide();
        b->setBadge(badge);
        wl->addWidget(b);
        group->addButton(b);
        m_sectionButtons.insert(s, b);
        m_sectionBadges.insert(s, badge);
        m_sectionLayout->addWidget(wrap);
        connect(b, &QAbstractButton::clicked, this, [this, s] { showSection(s); });
    }
    // Whole-app actions live under the list, away from the section's own.
    m_sectionLayout->addSpacing(space::lg);
    m_sectionLayout->addWidget(hairline(m_sectionList));
    m_sectionLayout->addSpacing(space::sm);
    auto* import = m_importBtn = button(QStringLiteral("Import legacy settings…"), ButtonVariant::Subtle, QStringLiteral("file_down"), m_sectionList);
    import->setToolTip(QStringLiteral("Take over the values of the Python edition's settings.json"));
    import->setStyleSheet(QStringLiteral("text-align:left;"));
    auto* resetAllBtn = m_resetAllBtn = button(QStringLiteral("Reset all settings…"), ButtonVariant::Subtle, QStringLiteral("undo"), m_sectionList);
    resetAllBtn->setStyleSheet(QStringLiteral("text-align:left;"));
    resetAllBtn->setToolTip(QStringLiteral("Put every setting back to its default"));
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
    sh->addLayout(scol, 1);
    m_resetSection = button(QStringLiteral("Reset section"), ButtonVariant::Subtle, QStringLiteral("undo"), secHeader);
    m_resetSection->setToolTip(QStringLiteral("Put this section's settings back to their defaults"));
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
        m_ctx.settings().resetSection(m_section);
        (void)m_ctx.settings().save();
        rebuild(); // also forgets the refused values of the rows it replaces
        emit toastRequested(Tone::Ok, QStringLiteral("%1 reset").arg(m_section), QStringLiteral("Defaults are back for this section."));
    });

    connect(&m_ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant&) {
        refreshRow(key);
        refreshDependencyStates();
        updateSectionHeader();
        if (key == general::keys::kTempFolder)
            updateTempUsage(); // "Temporary files" names and measures that folder too
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
        {QStringLiteral("General"), QStringLiteral("How the app behaves.")},
        {QStringLiteral("Build"), QStringLiteral("Defaults for Build image jobs.")},
        {QStringLiteral("FPKG"), QStringLiteral("Defaults for FPKG packages.")},
        {QStringLiteral("PFS"), QStringLiteral("Defaults for PFS and FFPFSC images.")},
        {QStringLiteral("PlayGo"), QStringLiteral("PlayGo chunk layout and AMPR index.")},
        {QStringLiteral("DLC"), QStringLiteral("DLC packages from DLC INIs and DLC .pkg files.")},
        {QStringLiteral("Firmware and SDK"), QStringLiteral("Firmware targets and SDK versions.")},
        {QStringLiteral("Tools"), QStringLiteral("External tools the app uses. Nothing is downloaded; a private build includes your own copies.")},
        {QStringLiteral("Performance"), QStringLiteral("Threads, buffers and the queue.")},
        {QStringLiteral("Storage"), QStringLiteral("Folders and free-space safety.")},
        {QStringLiteral("Verification"), QStringLiteral("What happens after a build.")},
        {QStringLiteral("PS5 connection"), QStringLiteral("How the app reaches the console.")},
        {QStringLiteral("Logging"), QStringLiteral("Log files and problem reports.")},
        {QStringLiteral("Advanced"), QStringLiteral("Troubleshooting; change these only when asked to.")},
    };
    return captions.value(section, QStringLiteral("Defaults for %1.").arg(section));
}

void SettingsPage::showSection(const QString& section)
{
    if (!m_sections.contains(section))
        return;
    m_section = section;
    if (auto* b = m_sectionButtons.value(section))
        b->setChecked(true);
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
    return Page::eventFilter(watched, event);
}

void SettingsPage::setCompactList(bool compact)
{
    if (compact == m_compactList)
        return;
    m_compactList = compact;
    m_sectionList->setFixedWidth(compact ? 44 : size::settingsList + 16);
    for (QAbstractButton* b : std::as_const(m_sectionButtons))
        static_cast<SectionButton*>(b)->setCompact(compact);
    m_importBtn->setText(compact ? QString() : QStringLiteral("Import legacy settings…"));
    m_resetAllBtn->setText(compact ? QString() : QStringLiteral("Reset all settings…"));
    m_importBtn->setToolTip(compact ? QStringLiteral("Import legacy settings…") : QStringLiteral("Take over the values of the Python edition's settings.json"));
    m_resetAllBtn->setToolTip(compact ? QStringLiteral("Reset all settings…") : QStringLiteral("Put every setting back to its default"));
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
        m_secTitle->setText(QStringLiteral("Search results"));
        m_secChanged->hide();
        m_resetSection->hide();
        int n = 0;
        for (const QString& s : m_sections) {
            std::vector<const SettingDef*> hits;
            for (const SettingDef* d : m_ctx.schema().inSection(s))
                if (d->label.contains(query, Qt::CaseInsensitive) || d->description.contains(query, Qt::CaseInsensitive) || d->key.contains(query, Qt::CaseInsensitive))
                    hits.push_back(d);
            if (hits.empty())
                continue;
            auto* card = new Card(m_cards);
            card->body()->setSpacing(0);
            auto* open = new Link(QStringLiteral("Open section"), card);
            card->setTitle(s, open);
            connect(open, &Link::activated, this, [this, s] { showSection(s); }, Qt::QueuedConnection);
            for (size_t i = 0; i < hits.size(); ++i) {
                addRow(*hits[i], card, card->body(), i + 1 < hits.size());
                ++n;
            }
            m_cardsLayout->addWidget(card);
        }
        if (n == 0)
            m_cardsLayout->addWidget(new EmptyState(QStringLiteral("search"), QStringLiteral("No setting matches"),
                                                    QStringLiteral("Try a shorter word, or browse the sections."), m_cards));
        m_secCaption->setText(n == 0 ? QStringLiteral("Nothing matches “%1”.").arg(query)
                                     : QStringLiteral("%1 setting%2 match “%3”.").arg(n).arg(n == 1 ? QString() : QStringLiteral("s")).arg(query));
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
        card->setTitle(g.isEmpty() ? section : g);
        for (size_t i = 0; i < rows.size(); ++i)
            addRow(*rows[i], card, card->body(), i + 1 < rows.size());
        m_cardsLayout->addWidget(card);
    }
    if (section == QLatin1String("Storage")) {
        // Temporary files: the app's own temp root and, when one is chosen,
        // the temporary folder of this section (jobs run there then).
        auto* card = new Card(m_cards);
        card->body()->setSpacing(0);
        card->setTitle(QStringLiteral("Temporary files"));
        auto* trow = new QWidget(card);
        auto* th = hbox(trow, space::lg, QMargins(0, 6, 0, 6));
        auto* tcol = new QVBoxLayout;
        tcol->setSpacing(2);
        tcol->addWidget(label(QStringLiteral("Files left by jobs"), TextStyle::Body, color::fg_1, trow));
        m_tempUsage = caption(QStringLiteral("Measuring…"), trow);
        tcol->addWidget(m_tempUsage);
        th->addLayout(tcol, 1);
        m_tempClear = button(QStringLiteral("Clear"), ButtonVariant::Secondary, QStringLiteral("trash"), trow);
        m_tempClear->setEnabled(false);
        connect(m_tempClear, &QPushButton::clicked, this, [this] {
            if (m_ctx.queue().isBusy()) {
                emit toastRequested(Tone::Warn, QStringLiteral("Jobs are running"), QStringLiteral("Temporary files are cleared when the queue is idle."));
                return;
            }
            int n = Paths::sweepStaleTemp(0);
            if (const QString custom = m_ctx.settings().value(general::keys::kTempFolder).toString().trimmed(); !custom.isEmpty())
                n += Paths::sweepStaleTempIn(custom, 0);
            emit toastRequested(Tone::Ok, QStringLiteral("Temporary files cleared"), QStringLiteral("%1 folder%2 removed").arg(n).arg(n == 1 ? QString() : QStringLiteral("s")));
            updateTempUsage();
        });
        th->addWidget(m_tempClear);
        card->body()->addWidget(trow);
        m_cardsLayout->addWidget(card);
        updateTempUsage();
    }
    if (section == QLatin1String("Advanced")) {
        auto* card = new Card(m_cards);
        card->setTitle(QStringLiteral("Import settings from the old app"));
        auto* note = caption(QStringLiteral("Takes over every value this app understands. Nothing is deleted."), card);
        note->setToolTip(QStringLiteral("Reads the Python edition's settings.json; values the native app does not use are skipped."));
        card->body()->addWidget(note);
        auto* pf = new PathField(PathField::Mode::OpenFile, card);
        pf->setFilter(QStringLiteral("JSON files (*.json)"));
        pf->setDialogTitle(QStringLiteral("Choose the old settings.json"));
        const QString legacy = QDir::home().filePath(QStringLiteral(".exfat_builder_settings.json"));
        if (QFile::exists(legacy))
            pf->setPath(legacy);
        else
            pf->setPlaceholder(nativePath(legacy));
        card->body()->addWidget(field(QStringLiteral("Old settings file"), pf, {}, card));
        auto* row = new QWidget(card);
        auto* rh = hbox(row, space::sm);
        rh->addStretch(1);
        auto* go = button(QStringLiteral("Import"), ButtonVariant::Secondary, QStringLiteral("file_down"), row);
        connect(go, &QPushButton::clicked, this, [this, pf] {
            if (pf->path().isEmpty()) {
                pf->setError(QStringLiteral("Choose the old settings file first."));
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
        auto* badge = new Badge(QStringLiteral("Checking…"), Tone::Neutral, card);
        auto* row = card->setTitle(QStringLiteral("Components and tools"), badge);
        auto* again = button(QStringLiteral("Check again"), ButtonVariant::Subtle, QStringLiteral("refresh"), card);
        setButtonSmall(again, true);
        auto* copyReport = new IconButton(QStringLiteral("copy"), QStringLiteral("Copy report"), IconButton::Size::Small, card);
        copyReport->setToolTip(QStringLiteral("Copy the tool report (JSON)"));
        row->addWidget(again);
        row->addWidget(copyReport);
        m_deps = new DependencyList(m_ctx, {}, card);
        card->body()->addWidget(m_deps);
        if (m_deps->isEmpty())
            card->body()->addWidget(caption(QStringLiteral("No external tools are registered in this build."), card));
        connect(again, &QPushButton::clicked, m_deps, &DependencyList::checkAgain);
        connect(copyReport, &QToolButton::clicked, this, [this] {
            QGuiApplication::clipboard()->setText(m_deps->report());
            emit toastRequested(Tone::Ok, QStringLiteral("Report copied"), QStringLiteral("Dependency statuses are on the clipboard as JSON."));
        });
        connect(m_deps, &DependencyList::changed, this, [this, badge] {
            if (m_deps->isEmpty()) {
                badge->set(QStringLiteral("None"), Tone::Neutral);
                return;
            }
            const int ready = m_deps->readyCount(), total = m_deps->total();
            badge->set(QStringLiteral("%1 of %2 ready").arg(ready).arg(total), ready == total ? Tone::Ok : Tone::Warn);
        });
        connect(m_deps, &DependencyList::fixRequested, this, [this](const QString&, const QString& how, const QString& settingKey) {
            navigateWith({{QStringLiteral("key"), settingKey}, {QStringLiteral("howToFix"), how}});
        });
        m_deps->refresh();
        m_cardsLayout->insertWidget(0, card);
    } else if (!depIds.isEmpty()) {
        auto* card = new Card(m_cards);
        card->body()->setSpacing(0);
        auto* again = button(QStringLiteral("Detect again"), ButtonVariant::Subtle, QStringLiteral("refresh"), card);
        setButtonSmall(again, true);
        card->setTitle(QStringLiteral("Tools this section uses"), again);
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
            names << entry.first;
        auto* fold = new Disclosure(QStringLiteral("Advanced"), elideMiddle(names.join(QStringLiteral(", ")), 80), m_cards);
        fold->setObjectName(QStringLiteral("settings-advanced"));
        fold->body()->setSpacing(0);
        for (size_t gi = 0; gi < advanced.size(); ++gi) {
            const auto& [g, list] = advanced[gi];
            if (advanced.size() > 1) {
                auto* head = label(g, TextStyle::Overline, color::fg_4, fold);
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
        m_cardsLayout->addWidget(new EmptyState(QStringLiteral("settings"), QStringLiteral("Nothing to set here yet"),
                                                QStringLiteral("The modules that use this section are not part of this build."), m_cards));
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
                                                   : QStringLiteral("%1 and %2").arg(nativePath(Paths::tempRoot()), nativePath(custom));
            self->m_tempUsage->setText(bytes > 0 ? QStringLiteral("%1 in %2").arg(formatBytes(bytes), where)
                                                 : QStringLiteral("Nothing to clear in %1").arg(where));
            self->m_tempClear->setEnabled(bytes > 0);
        }, Qt::QueuedConnection);
    });
}

namespace {
// "Key label: reason" -> "reason"; a message without that form is shown whole.
QString refusalText(const QString& message)
{
    const QString reason = message.section(QStringLiteral(": "), 1);
    return reason.isEmpty() ? message : reason;
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
    if (const SettingDef* d = m_ctx.schema().find(key); d && d->restartRequired) {
        m_restartNeeded = true;
        emit toastRequested(Tone::Info, QStringLiteral("Takes effect after a restart"), d->label);
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
            it.value()->setDisabledReason(QStringLiteral("Only available when \"%1\" is %2.")
                                              .arg(other ? other->label : d->enabledWhenKey,
                                                   other ? SettingRow::displayValue(*other, d->enabledWhenValue).toLower() : d->enabledWhenValue.toString()));
        }
    }
}

void SettingsPage::updateSectionHeader()
{
    if (!m_search->text().trimmed().isEmpty())
        return;
    m_secTitle->setText(m_section);
    int differ = 0;
    for (const SettingDef* d : m_ctx.schema().inSection(m_section))
        if (!m_ctx.settings().isDefault(d->key))
            ++differ;
    m_secCaption->setText(sectionCaption(m_section));
    m_secChanged->setText(QStringLiteral("%1 changed").arg(differ));
    m_secChanged->setToolTip(QStringLiteral("%1 setting%2 in this section differ%3 from the defaults")
                                 .arg(differ).arg(differ == 1 ? QString() : QStringLiteral("s")).arg(differ == 1 ? QStringLiteral("s") : QString()));
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
        m_advancedFold->setSummary(QStringLiteral("%1 changed").arg(differ), Tone::Info);
    else
        m_advancedFold->setSummary(QStringLiteral("%1 setting%2").arg(m_advancedKeys.size()).arg(m_advancedKeys.size() == 1 ? QString() : QStringLiteral("s")));
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
    const QString path = QFileDialog::getOpenFileName(window(), QStringLiteral("Choose the legacy settings.json"), start, QStringLiteral("JSON files (*.json)"));
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
        Sheet::message(window(), QStringLiteral("Nothing was imported"), r.error().message, Sheet::Icon::Error);
        return;
    }
    (void)m_ctx.settings().save();
    QStringList changed;
    for (const SettingDef* d : m_ctx.schema().all())
        if (m_ctx.settings().value(d->key) != before.value(d->key))
            changed << QStringLiteral("%1 › %2: %3").arg(d->section, d->label, SettingRow::displayValue(*d, m_ctx.settings().value(d->key)));
    rebuild();
    const QString title = QStringLiteral("%1 value%2 imported").arg(r.value()).arg(r.value() == 1 ? QString() : QStringLiteral("s"));
    if (changed.isEmpty())
        Sheet::message(window(), title, QStringLiteral("Every imported value already matched the current settings, so nothing changed."), Sheet::Icon::Info);
    else
    {
        // A long list is cut so the button stays on screen.
        const int kShown = 16;
        QString list = Sheet::monoLines(changed.mid(0, kShown)); // escapes the values
        if (changed.size() > kShown)
            list += QStringLiteral("<br>… and %1 more").arg(changed.size() - kShown);
        Sheet::message(window(), title, QStringLiteral("Changed:<br>%1").arg(list), Sheet::Icon::Info);
    }
}

void SettingsPage::resetAll()
{
    if (!Sheet::confirm(window(), QStringLiteral("Reset every setting?"),
                        QStringLiteral("All sections go back to their defaults. Tool paths you entered are cleared too. Jobs already in the queue keep their settings."),
                        QStringLiteral("Keep settings"), QStringLiteral("Reset all"), true, Sheet::Icon::Warning))
        return;
    m_ctx.settings().resetAll();
    (void)m_ctx.settings().save();
    m_errors.clear();
    rebuild();
    emit toastRequested(Tone::Ok, QStringLiteral("Settings reset"), QStringLiteral("Every setting is back to its default."));
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
        m_fixBar->setTitle(QStringLiteral("How to fix"));
        m_fixBar->setMessage(how);
        m_fixBar->show();
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
    out << QStringLiteral("reset-dialog");
    return out;
}

void SettingsPage::applyDemoState(const QString& name)
{
    if (name == QLatin1String("reset-dialog")) {
        auto* s = new Sheet(window(), QStringLiteral("Reset every setting?"), Sheet::Icon::Warning);
        s->addText(QStringLiteral("All sections go back to their defaults. Tool paths you entered are cleared too. Jobs already in the queue keep their settings."));
        s->addButton(QStringLiteral("Keep settings"), Sheet::Role::Safe);
        s->addButton(QStringLiteral("Reset all"), Sheet::Role::Destructive);
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
