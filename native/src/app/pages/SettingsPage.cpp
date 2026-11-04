#include "app/pages/SettingsPage.h"
#include "app/i18n/I18n.h"

#include "app/AppContext.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "core/Paths.h"

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
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

namespace {

// A 32 px section row for the left list.
class SectionButton : public QAbstractButton {
public:
    SectionButton(const QString& text, QWidget* parent) : QAbstractButton(parent)
    {
        setText(text);
        setCheckable(true);
        setFixedHeight(32);
        setFocusPolicy(Qt::TabFocus);
        setAttribute(Qt::WA_Hover);
        setProperty("fwRadius", radius::sm);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setBadge(Badge* b) { m_badge = b; }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (isChecked() || underMouse()) {
            p.setPen(Qt::NoPen);
            p.setBrush(isChecked() ? color::bg_3 : color::bg_2);
            p.drawRoundedRect(rect(), radius::sm, radius::sm);
        }
        if (isChecked()) {
            p.setBrush(color::accent);
            p.drawRoundedRect(QRect(0, (height() - 16) / 2, 3, 16), radius::xs, radius::xs);
        }
        p.setPen(isChecked() ? color::fg_0 : color::fg_2);
        p.setFont(isChecked() ? font::bodyStrong() : font::body());
        const int right = m_badge && m_badge->isVisible() ? m_badge->width() + 16 : 8;
        p.drawText(rect().adjusted(12, 0, -right, 0), Qt::AlignVCenter | Qt::AlignLeft, text());
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
    Badge* m_badge = nullptr;
};

} // namespace

SettingsPage::SettingsPage(AppContext& ctx, QWidget* parent)
    : Page(ctx, QStringLiteral("settings"), tr("Settings"),
           tr("Defaults for every new job. A job page can override any of them; the override shows a marker on that job only."), parent)
{
    m_search = textInput(tr("Search settings"), false, this);
    m_search->setFixedWidth(260);
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(tr("Search settings"));
    header()->addAction(m_search);
    auto* import = button(tr("Import legacy settings…"), ButtonVariant::Secondary, QStringLiteral("file_down"), this);
    header()->addAction(import);
    auto* resetAll = button(tr("Reset all…"), ButtonVariant::Secondary, QStringLiteral("undo"), this);
    header()->addAction(resetAll);
    connect(import, &QPushButton::clicked, this, &SettingsPage::importLegacy);
    connect(resetAll, &QPushButton::clicked, this, &SettingsPage::resetAll);
    connect(m_search, &QLineEdit::textChanged, this, &SettingsPage::setSearchText);

    auto* columns = new QWidget(this);
    auto* h = hbox(columns, space::xxl);
    h->setAlignment(Qt::AlignTop);
    m_sectionList = new QWidget(columns);
    m_sectionList->setFixedWidth(size::settingsList);
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
        auto* b = new SectionButton(i18n::trs(s), wrap);
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
    m_sectionLayout->addStretch(1);
    h->addWidget(m_sectionList, 0, Qt::AlignTop);

    m_pane = new QWidget(columns);
    m_pane->setMaximumWidth(size::formMax);
    m_paneLayout = vbox(m_pane, space::lg);
    auto* secHeader = new QWidget(m_pane);
    auto* sh = hbox(secHeader, space::lg);
    sh->setAlignment(Qt::AlignTop);
    auto* scol = new QVBoxLayout;
    scol->setSpacing(2);
    m_secTitle = heading({}, TextStyle::Subtitle, secHeader);
    m_secCaption = caption({}, secHeader);
    scol->addWidget(m_secTitle);
    scol->addWidget(m_secCaption);
    sh->addLayout(scol, 1);
    sh->addWidget(label(tr("Show advanced"), TextStyle::Body, color::fg_1, secHeader), 0, Qt::AlignVCenter);
    m_advancedSwitch = new Switch(secHeader);
    m_advancedSwitch->setAccessibleName(tr("Show advanced settings"));
    sh->addWidget(m_advancedSwitch, 0, Qt::AlignVCenter);
    m_resetSection = button(tr("Reset section"), ButtonVariant::Subtle, QStringLiteral("undo"), secHeader);
    sh->addWidget(m_resetSection, 0, Qt::AlignVCenter);
    m_paneLayout->addWidget(secHeader);
    m_cards = new QWidget(m_pane);
    m_cardsLayout = vbox(m_cards, space::lg);
    m_paneLayout->addWidget(m_cards);
    h->addWidget(m_pane, 1, Qt::AlignTop);
    content()->addWidget(columns);
    connect(m_advancedSwitch, &QAbstractButton::toggled, this, &SettingsPage::setShowAdvanced);
    connect(m_resetSection, &QPushButton::clicked, this, [this] {
        if (m_section == QLatin1String("Tools") && m_ctx.schema().inSection(m_section).empty())
            return;
        m_ctx.settings().resetSection(m_section);
        (void)m_ctx.settings().save();
        rebuild();
        emit toastRequested(Tone::Ok, tr("%1 reset").arg(m_section), tr("Defaults are back for this section."));
    });

    connect(&m_ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant&) {
        refreshRow(key);
        refreshDependencyStates();
        updateSectionHeader();
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
        {QStringLiteral("General"), tr("How the app behaves.")},
        {QStringLiteral("Build"), tr("Defaults for Build image jobs.")},
        {QStringLiteral("FPKG"), tr("Defaults for FPKG packages.")},
        {QStringLiteral("PFS"), tr("Defaults for PFS and FFPFSC images.")},
        {QStringLiteral("PlayGo"), tr("PlayGo chunk layout and AMPR index.")},
        {QStringLiteral("Backport"), tr("Defaults for Backport jobs.")},
        {QStringLiteral("Firmware and SDK"), tr("Firmware targets and SDK versions.")},
        {QStringLiteral("Tools"), tr("External tools the app can use. Nothing is downloaded or bundled.")},
        {QStringLiteral("Performance"), tr("Threads, buffers and the queue.")},
        {QStringLiteral("Storage"), tr("Folders and free-space safety.")},
        {QStringLiteral("Verification"), tr("What happens after a build.")},
        {QStringLiteral("PS5 connection"), tr("How the app reaches the console.")},
        {QStringLiteral("Logging"), tr("Log files and problem reports.")},
        {QStringLiteral("Advanced"), tr("Troubleshooting. Change these only when asked to.")},
    };
    return captions.value(section, tr("Defaults for %1.").arg(i18n::trs(section)));
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
    rebuild();
}

void SettingsPage::setShowAdvanced(bool on)
{
    if (m_advanced == on)
        return;
    m_advanced = on;
    if (m_advancedSwitch->isChecked() != on)
        m_advancedSwitch->setChecked(on);
    rebuild();
}

void SettingsPage::setSearchText(const QString& text)
{
    if (m_search->text() != text)
        m_search->setText(text);
    rebuild();
}

void SettingsPage::rebuild()
{
    while (QLayoutItem* it = m_cardsLayout->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    m_rows.clear();
    m_fixBar = nullptr;
    m_deps = nullptr;
    m_tempUsage = nullptr;
    m_tempClear = nullptr;
    const QString query = m_search->text().trimmed();
    if (!query.isEmpty()) {
        m_secTitle->setText(tr("Search results"));
        m_resetSection->hide();
        int n = 0;
        for (const QString& s : m_sections) {
            Card* card = nullptr;
            std::vector<const SettingDef*> hits;
            for (const SettingDef* d : m_ctx.schema().inSection(s))
                if (d->label.contains(query, Qt::CaseInsensitive) || d->description.contains(query, Qt::CaseInsensitive) || d->key.contains(query, Qt::CaseInsensitive)
                    || i18n::trs(d->label).contains(query, Qt::CaseInsensitive) || i18n::trs(d->description).contains(query, Qt::CaseInsensitive))
                    hits.push_back(d);
            if (hits.empty())
                continue;
            card = new Card(m_cards);
            card->body()->setSpacing(0);
            card->setTitle(i18n::trs(s));
            for (size_t i = 0; i < hits.size(); ++i) {
                auto* row = new SettingRow(*hits[i], card);
                row->setDivider(i + 1 < hits.size());
                row->setRestartNote(hits[i]->restartRequired);
                m_rows.insert(hits[i]->key, row);
                const QString key = hits[i]->key;
                connect(row->editor(), &SettingEditor::edited, this, [this, key](const QVariant& v) { applyValue(key, v); });
                card->body()->addWidget(row);
                refreshRow(key);
                ++n;
            }
            m_cardsLayout->addWidget(card);
        }
        m_secCaption->setText(n == 0 ? tr("No setting matches \"%1\".").arg(query) : tr("%1 setting%2 match \"%3\".").arg(n).arg(n == 1 ? QString() : tr("s")).arg(query));
        refreshDependencyStates();
        return;
    }
    m_resetSection->show();
    buildSection(m_section);
    updateSectionHeader();
    refreshDependencyStates();
}

void SettingsPage::buildSection(const QString& section)
{
    const auto defs = m_ctx.schema().inSection(section);
    QStringList groups;
    for (const SettingDef* d : defs)
        if (!groups.contains(d->group))
            groups << d->group;
    int hiddenAdvanced = 0;
    QStringList depIds;
    for (const QString& g : groups) {
        std::vector<const SettingDef*> rows;
        for (const SettingDef* d : defs) {
            if (d->group != g)
                continue;
            if (!d->dependencyId.isEmpty() && !depIds.contains(d->dependencyId))
                depIds << d->dependencyId;
            if (d->advanced && !m_advanced) {
                ++hiddenAdvanced;
                continue;
            }
            rows.push_back(d);
        }
        if (rows.empty())
            continue;
        auto* card = new Card(m_cards);
        card->body()->setSpacing(0);
        card->setTitle(i18n::trs(g.isEmpty() ? section : g));
        for (size_t i = 0; i < rows.size(); ++i) {
            const SettingDef* d = rows[i];
            auto* row = new SettingRow(*d, card);
            row->setDivider(i + 1 < rows.size());
            row->setRestartNote(d->restartRequired);
            m_rows.insert(d->key, row);
            const QString key = d->key;
            connect(row->editor(), &SettingEditor::edited, this, [this, key](const QVariant& v) { applyValue(key, v); });
            card->body()->addWidget(row);
            refreshRow(key);
        }
        m_cardsLayout->addWidget(card);
    }
    if (section == QLatin1String("Storage")) {
        // Temporary files (Paths::tempUsageBytes / sweepStaleTemp).
        auto* card = new Card(m_cards);
        card->body()->setSpacing(0);
        card->setTitle(tr("Temporary files"));
        auto* trow = new QWidget(card);
        auto* th = hbox(trow, space::lg, QMargins(0, 6, 0, 6));
        auto* tcol = new QVBoxLayout;
        tcol->setSpacing(2);
        tcol->addWidget(label(tr("Files left by jobs"), TextStyle::Body, color::fg_1, trow));
        m_tempUsage = caption(tr("Measuring…"), trow);
        tcol->addWidget(m_tempUsage);
        th->addLayout(tcol, 1);
        m_tempClear = button(tr("Clear"), ButtonVariant::Secondary, QStringLiteral("trash"), trow);
        m_tempClear->setEnabled(false);
        connect(m_tempClear, &QPushButton::clicked, this, [this] {
            if (m_ctx.queue().isBusy()) {
                emit toastRequested(Tone::Warn, tr("Jobs are running"), tr("Temporary files are cleared when the queue is idle."));
                return;
            }
            const int n = Paths::sweepStaleTemp(0);
            emit toastRequested(Tone::Ok, tr("Temporary files cleared"), tr("%1 folder%2 removed").arg(n).arg(n == 1 ? QString() : tr("s")));
            updateTempUsage();
        });
        th->addWidget(m_tempClear);
        card->body()->addWidget(trow);
        m_cardsLayout->addWidget(card);
        updateTempUsage();
    }
    if (section == QLatin1String("Advanced")) {
        auto* card = new Card(m_cards);
        card->setTitle(tr("Import settings from the old app"));
        card->body()->addWidget(caption(tr("Reads the Python edition's settings.json and takes over every value the native app understands. Nothing is deleted; values the native app does not use are skipped."), card));
        auto* pf = new PathField(PathField::Mode::OpenFile, card);
        pf->setFilter(tr("JSON files (*.json)"));
        pf->setDialogTitle(tr("Choose the old settings.json"));
        const QString legacy = QDir::home().filePath(QStringLiteral(".exfat_builder_settings.json"));
        if (QFile::exists(legacy))
            pf->setPath(legacy);
        else
            pf->setPlaceholder(nativePath(legacy));
        card->body()->addWidget(field(tr("Old settings file"), pf, {}, card));
        auto* row = new QWidget(card);
        auto* rh = hbox(row, space::sm);
        rh->addStretch(1);
        auto* go = button(tr("Import"), ButtonVariant::Primary, QStringLiteral("file_down"), row);
        connect(go, &QPushButton::clicked, this, [this, pf] {
            if (pf->path().isEmpty()) {
                pf->setError(tr("Choose the old settings file first."));
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
        auto* badge = new Badge(tr("Checking…"), Tone::Neutral, card);
        auto* row = card->setTitle(tr("Components and tools"), badge);
        auto* again = button(tr("Check again"), ButtonVariant::Subtle, QStringLiteral("refresh"), card);
        setButtonSmall(again, true);
        auto* copyReport = button(tr("Copy report"), ButtonVariant::Subtle, QStringLiteral("copy"), card);
        setButtonSmall(copyReport, true);
        row->addWidget(copyReport);
        row->addWidget(again);
        m_deps = new DependencyList(m_ctx, {}, card);
        card->body()->addWidget(m_deps);
        if (m_deps->isEmpty())
            card->body()->addWidget(caption(tr("No external tools are registered in this build."), card));
        connect(again, &QPushButton::clicked, m_deps, &DependencyList::checkAgain);
        connect(copyReport, &QPushButton::clicked, this, [this] {
            QGuiApplication::clipboard()->setText(m_deps->report());
            emit toastRequested(Tone::Ok, tr("Report copied"), tr("Dependency statuses are on the clipboard as JSON."));
        });
        connect(m_deps, &DependencyList::changed, this, [this, badge] {
            if (m_deps->isEmpty()) {
                badge->set(tr("None"), Tone::Neutral);
                return;
            }
            const int ready = m_deps->readyCount(), total = m_deps->total();
            badge->set(tr("%1 of %2 ready").arg(ready).arg(total), ready == total ? Tone::Ok : Tone::Warn);
        });
        connect(m_deps, &DependencyList::fixRequested, this, [this](const QString&, const QString& how, const QString& settingKey) {
            navigateWith({{QStringLiteral("key"), settingKey}, {QStringLiteral("howToFix"), how}});
        });
        m_deps->refresh();
        m_cardsLayout->insertWidget(0, card);
    } else if (!depIds.isEmpty()) {
        auto* card = new Card(m_cards);
        card->body()->setSpacing(0);
        auto* again = button(tr("Detect again"), ButtonVariant::Subtle, QStringLiteral("refresh"), card);
        setButtonSmall(again, true);
        card->setTitle(tr("Tools this section depends on"), again);
        auto* list = new DependencyList(m_ctx, depIds, card);
        card->body()->addWidget(list);
        connect(again, &QPushButton::clicked, list, &DependencyList::checkAgain);
        connect(list, &DependencyList::fixRequested, this, [this](const QString&, const QString& how, const QString& settingKey) {
            navigateWith({{QStringLiteral("section"), QStringLiteral("Tools")}, {QStringLiteral("key"), settingKey}, {QStringLiteral("howToFix"), how}});
        });
        m_cardsLayout->addWidget(card);
    }
    if (hiddenAdvanced > 0) {
        auto* note = caption(tr("%1 advanced setting%2 hidden. Turn on Show advanced to see %3.")
                                 .arg(hiddenAdvanced).arg(hiddenAdvanced == 1 ? QString() : tr("s")).arg(hiddenAdvanced == 1 ? QStringLiteral("it") : QStringLiteral("them")), m_cards);
        m_cardsLayout->addWidget(note);
    }
    if (m_cardsLayout->count() == 0)
        m_cardsLayout->addWidget(new EmptyState(QStringLiteral("settings"), tr("Nothing to set here yet"),
                                                tr("The modules that use this section are not part of this build."), m_cards));
}

void SettingsPage::updateTempUsage()
{
    if (!m_tempUsage)
        return;
    QPointer<SettingsPage> self(this);
    (void)QtConcurrent::run([self] {
        const qint64 bytes = Paths::tempUsageBytes();
        QMetaObject::invokeMethod(self.data(), [self, bytes] {
            if (!self || !self->m_tempUsage)
                return;
            self->m_tempUsage->setText(bytes > 0 ? tr("%1 in %2").arg(formatBytes(bytes), nativePath(Paths::tempRoot()))
                                                 : tr("Nothing to clear in %1").arg(nativePath(Paths::tempRoot())));
            self->m_tempClear->setEnabled(bytes > 0);
        }, Qt::QueuedConnection);
    });
}

void SettingsPage::applyValue(const QString& key, const QVariant& value)
{
    SettingRow* row = m_rows.value(key);
    auto st = m_ctx.settings().setValue(key, value);
    if (!st.ok()) {
        m_errors.insert(key, st.error().message);
        if (row)
            row->setMessage(st.error().message.section(QStringLiteral(": "), 1), Tone::Err);
        emit attentionChanged();
        emit statusContextChanged();
        return;
    }
    m_errors.remove(key);
    if (row)
        row->setMessage({}, Tone::Neutral);
    if (const SettingDef* d = m_ctx.schema().find(key); d && d->restartRequired) {
        m_restartNeeded = true;
        emit toastRequested(Tone::Info, tr("Takes effect after a restart"), d->label);
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
        row->setMessage(m_errors.value(key).section(QStringLiteral(": "), 1), Tone::Err);
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
            it.value()->setDisabledReason(tr("Only available when \"%1\" is %2.")
                                              .arg(other ? other->label : d->enabledWhenKey,
                                                   other ? SettingRow::displayValue(*other, d->enabledWhenValue).toLower() : d->enabledWhenValue.toString()));
        }
    }
}

void SettingsPage::updateSectionHeader()
{
    if (!m_search->text().trimmed().isEmpty())
        return;
    m_secTitle->setText(i18n::trs(m_section));
    int differ = 0;
    for (const SettingDef* d : m_ctx.schema().inSection(m_section))
        if (!m_ctx.settings().isDefault(d->key))
            ++differ;
    QString cap = sectionCaption(m_section);
    if (differ > 0)
        cap += tr(" %1 setting%2 differ%3 from the defaults.").arg(differ).arg(differ == 1 ? QString() : tr("s")).arg(differ == 1 ? tr("s") : QString());
    m_secCaption->setText(cap);
    m_resetSection->setEnabled(differ > 0);
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
            b->adjustSize();
            b->move(btn->width() - b->width() - 8, (btn->height() - b->height()) / 2);
            btn->update();
        }
    }
    emit statusContextChanged();
}

int SettingsPage::attentionCount() const
{
    int n = m_errors.size();
    for (const auto& info : m_ctx.dependencies().lastAll())
        if (info.status == DependencyStatus::Missing || info.status == DependencyStatus::Outdated || info.status == DependencyStatus::Error)
            ++n;
    return n;
}

void SettingsPage::importLegacy()
{
    QString start = QDir::homePath();
    const QString legacy = QDir::home().filePath(QStringLiteral(".exfat_builder_settings.json"));
    if (QFile::exists(legacy))
        start = legacy;
    const QString path = QFileDialog::getOpenFileName(window(), tr("Choose the legacy settings.json"), start, tr("JSON files (*.json)"));
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
        Sheet::message(window(), tr("Nothing was imported"), r.error().message, Sheet::Icon::Error);
        return;
    }
    (void)m_ctx.settings().save();
    QStringList changed;
    for (const SettingDef* d : m_ctx.schema().all())
        if (m_ctx.settings().value(d->key) != before.value(d->key))
            changed << QStringLiteral("%1 › %2: %3").arg(d->section, d->label, SettingRow::displayValue(*d, m_ctx.settings().value(d->key))).toHtmlEscaped();
    rebuild();
    const QString title = tr("%1 value%2 imported").arg(r.value()).arg(r.value() == 1 ? QString() : tr("s"));
    if (changed.isEmpty())
        Sheet::message(window(), title, tr("Every imported value already matched the current settings, so nothing changed."), Sheet::Icon::Info);
    else
        Sheet::message(window(), title, tr("Changed:<br>%1").arg(Sheet::monoSpan(changed.join(QStringLiteral("<br>")))), Sheet::Icon::Info);
}

void SettingsPage::resetAll()
{
    if (!Sheet::confirm(window(), tr("Reset every setting?"),
                        tr("All sections go back to their defaults. Tool paths you entered are cleared too. Jobs already in the queue keep their settings."),
                        tr("Keep settings"), tr("Reset all"), true, Sheet::Icon::Warning))
        return;
    m_ctx.settings().resetAll();
    (void)m_ctx.settings().save();
    m_errors.clear();
    rebuild();
    emit toastRequested(Tone::Ok, tr("Settings reset"), tr("Every setting is back to its default."));
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
        m_fixBar->setTitle(tr("How to fix"));
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
        auto* s = new Sheet(window(), tr("Reset every setting?"), Sheet::Icon::Warning);
        s->addText(tr("All sections go back to their defaults. Tool paths you entered are cleared too. Jobs already in the queue keep their settings."));
        s->addButton(tr("Keep settings"), Sheet::Role::Safe);
        s->addButton(tr("Reset all"), Sheet::Role::Destructive);
        s->open();
        m_demoSheet = s;
        return;
    }
    for (const QString& sct : m_sections)
        if (name == QStringLiteral("section-") + QString(sct).toLower().replace(u' ', u'-')) {
            showSection(sct);
            if (sct == QLatin1String("Advanced") || sct == QLatin1String("General"))
                setShowAdvanced(true);
        }
}

void SettingsPage::clearDemoState()
{
    if (auto* s = qobject_cast<Sheet*>(m_demoSheet.data()))
        s->close(0);
}

} // namespace fw::pages
