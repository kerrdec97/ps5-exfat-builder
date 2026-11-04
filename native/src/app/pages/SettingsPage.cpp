#include "app/pages/SettingsPage.h"

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
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>

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
    : Page(ctx, QStringLiteral("settings"), QStringLiteral("Settings"),
           QStringLiteral("Defaults for every new job. A job page can override any of them; the override shows a marker on that job only."), parent)
{
    m_search = textInput(QStringLiteral("Search settings"), false, this);
    m_search->setFixedWidth(260);
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(QStringLiteral("Search settings"));
    header()->addAction(m_search);
    auto* import = button(QStringLiteral("Import legacy settings…"), ButtonVariant::Secondary, QStringLiteral("file_down"), this);
    header()->addAction(import);
    auto* resetAll = button(QStringLiteral("Reset all…"), ButtonVariant::Secondary, QStringLiteral("undo"), this);
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
        auto* b = new SectionButton(s, wrap);
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
    sh->addWidget(label(QStringLiteral("Show advanced"), TextStyle::Body, color::fg_1, secHeader), 0, Qt::AlignVCenter);
    m_advancedSwitch = new Switch(secHeader);
    m_advancedSwitch->setAccessibleName(QStringLiteral("Show advanced settings"));
    sh->addWidget(m_advancedSwitch, 0, Qt::AlignVCenter);
    m_resetSection = button(QStringLiteral("Reset section"), ButtonVariant::Subtle, QStringLiteral("undo"), secHeader);
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
        emit toastRequested(Tone::Ok, QStringLiteral("%1 reset").arg(m_section), QStringLiteral("Defaults are back for this section."));
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
        {QStringLiteral("General"), QStringLiteral("How the app behaves.")},
        {QStringLiteral("Build"), QStringLiteral("Defaults for Build image jobs.")},
        {QStringLiteral("FPKG"), QStringLiteral("Defaults for FPKG packages.")},
        {QStringLiteral("PFS"), QStringLiteral("Defaults for PFS and FFPFSC images.")},
        {QStringLiteral("PlayGo"), QStringLiteral("PlayGo chunk layout and AMPR index.")},
        {QStringLiteral("Backport"), QStringLiteral("Defaults for Backport jobs.")},
        {QStringLiteral("Firmware and SDK"), QStringLiteral("Firmware targets and SDK versions.")},
        {QStringLiteral("Tools"), QStringLiteral("External tools the app can use. Nothing is downloaded or bundled.")},
        {QStringLiteral("Performance"), QStringLiteral("Threads, buffers and the queue.")},
        {QStringLiteral("Storage"), QStringLiteral("Folders and free-space safety.")},
        {QStringLiteral("Verification"), QStringLiteral("What happens after a build.")},
        {QStringLiteral("PS5 connection"), QStringLiteral("How the app reaches the console.")},
        {QStringLiteral("Logging"), QStringLiteral("Log files and problem reports.")},
        {QStringLiteral("Advanced"), QStringLiteral("Troubleshooting. Change these only when asked to.")},
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
        m_secTitle->setText(QStringLiteral("Search results"));
        m_resetSection->hide();
        int n = 0;
        for (const QString& s : m_sections) {
            Card* card = nullptr;
            std::vector<const SettingDef*> hits;
            for (const SettingDef* d : m_ctx.schema().inSection(s))
                if (d->label.contains(query, Qt::CaseInsensitive) || d->description.contains(query, Qt::CaseInsensitive) || d->key.contains(query, Qt::CaseInsensitive))
                    hits.push_back(d);
            if (hits.empty())
                continue;
            card = new Card(m_cards);
            card->body()->setSpacing(0);
            card->setTitle(s);
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
        m_secCaption->setText(n == 0 ? QStringLiteral("No setting matches \"%1\".").arg(query) : QStringLiteral("%1 setting%2 match \"%3\".").arg(n).arg(n == 1 ? QString() : QStringLiteral("s")).arg(query));
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
        card->setTitle(g.isEmpty() ? section : g);
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
        if (section == QLatin1String("Storage") && g == groups.last()) {
            // Temporary files row (Paths::tempUsageBytes / sweepStaleTemp).
            card->body()->addWidget(hairline(card));
            auto* trow = new QWidget(card);
            auto* th = hbox(trow, space::lg, QMargins(0, 6, 0, 6));
            auto* tcol = new QVBoxLayout;
            tcol->setSpacing(2);
            tcol->addWidget(label(QStringLiteral("Temporary files"), TextStyle::Body, color::fg_1, trow));
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
                const int n = Paths::sweepStaleTemp(0);
                emit toastRequested(Tone::Ok, QStringLiteral("Temporary files cleared"), QStringLiteral("%1 folder%2 removed").arg(n).arg(n == 1 ? QString() : QStringLiteral("s")));
                updateTempUsage();
            });
            th->addWidget(m_tempClear);
            card->body()->addWidget(trow);
            updateTempUsage();
        }
        m_cardsLayout->addWidget(card);
    }
    if (section == QLatin1String("Tools")) {
        auto* card = new Card(m_cards);
        card->body()->setSpacing(0);
        auto* badge = new Badge(QStringLiteral("Checking…"), Tone::Neutral, card);
        auto* row = card->setTitle(QStringLiteral("Components and tools"), badge);
        auto* again = button(QStringLiteral("Check again"), ButtonVariant::Subtle, QStringLiteral("refresh"), card);
        setButtonSmall(again, true);
        auto* copyReport = button(QStringLiteral("Copy report"), ButtonVariant::Subtle, QStringLiteral("copy"), card);
        setButtonSmall(copyReport, true);
        row->addWidget(copyReport);
        row->addWidget(again);
        m_deps = new DependencyList(m_ctx, {}, card);
        card->body()->addWidget(m_deps);
        if (m_deps->isEmpty())
            card->body()->addWidget(caption(QStringLiteral("No external tools are registered in this build."), card));
        connect(again, &QPushButton::clicked, m_deps, &DependencyList::checkAgain);
        connect(copyReport, &QPushButton::clicked, this, [this] {
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
        card->setTitle(QStringLiteral("Tools this section depends on"), again);
        auto* list = new DependencyList(m_ctx, depIds, card);
        card->body()->addWidget(list);
        connect(again, &QPushButton::clicked, list, &DependencyList::checkAgain);
        connect(list, &DependencyList::fixRequested, this, [this](const QString&, const QString& how, const QString& settingKey) {
            navigateWith({{QStringLiteral("section"), QStringLiteral("Tools")}, {QStringLiteral("key"), settingKey}, {QStringLiteral("howToFix"), how}});
        });
        m_cardsLayout->addWidget(card);
    }
    if (hiddenAdvanced > 0) {
        auto* note = caption(QStringLiteral("%1 advanced setting%2 hidden. Turn on Show advanced to see %3.")
                                 .arg(hiddenAdvanced).arg(hiddenAdvanced == 1 ? QString() : QStringLiteral("s")).arg(hiddenAdvanced == 1 ? QStringLiteral("it") : QStringLiteral("them")), m_cards);
        m_cardsLayout->addWidget(note);
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
    (void)QtConcurrent::run([self] {
        const qint64 bytes = Paths::tempUsageBytes();
        QMetaObject::invokeMethod(self.data(), [self, bytes] {
            if (!self || !self->m_tempUsage)
                return;
            self->m_tempUsage->setText(bytes > 0 ? QStringLiteral("%1 in %2").arg(formatBytes(bytes), nativePath(Paths::tempRoot()))
                                                 : QStringLiteral("Nothing to clear in %1").arg(nativePath(Paths::tempRoot())));
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
    QString cap = sectionCaption(m_section);
    if (differ > 0)
        cap += QStringLiteral(" %1 setting%2 differ%3 from the defaults.").arg(differ).arg(differ == 1 ? QString() : QStringLiteral("s")).arg(differ == 1 ? QStringLiteral("s") : QString());
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
    const QString path = QFileDialog::getOpenFileName(window(), QStringLiteral("Choose the legacy settings.json"), start, QStringLiteral("JSON files (*.json)"));
    if (path.isEmpty())
        return;
    auto r = m_ctx.settings().importLegacy(path);
    if (!r.ok()) {
        Sheet::message(window(), QStringLiteral("Nothing was imported"), r.error().message, Sheet::Icon::Error);
        return;
    }
    (void)m_ctx.settings().save();
    rebuild();
    emit toastRequested(Tone::Ok, QStringLiteral("Legacy settings imported"), QStringLiteral("%1 value%2 taken over. Values the native app does not use were skipped.").arg(r.value()).arg(r.value() == 1 ? QString() : QStringLiteral("s")));
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
