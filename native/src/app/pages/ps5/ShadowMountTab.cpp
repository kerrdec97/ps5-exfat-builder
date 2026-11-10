#include "app/pages/ps5/ShadowMountTab.h"

#include "app/AppContext.h"
#include "app/services/JobSubmit.h"
#include "app/ui/Components.h"
#include "app/ui/Icons.h"
#include "app/ui/Sheet.h"
#include "connect/ps5/ConsoleBackup.h"
#include "connect/ps5/Module.h"
#include "connect/ps5/SmpApi.h"
#include "jobs/JobQueue.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QSpinBox>
#include <QTableWidget>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;
using ps5::MountConfigValues;
using ps5::SmpKey;
using ps5::SmpKeyType;

namespace {

const QStringList& groupOrder()
{
    static const QStringList g = {QStringLiteral("General"), QStringLiteral("Scanning"), QStringLiteral("Mounting"),
                                  QStringLiteral("Library"), QStringLiteral("Fakelib"), QStringLiteral("Emulators"),
                                  QStringLiteral("Fan"), QStringLiteral("API"), QStringLiteral("Sectors"), QStringLiteral("Rules")};
    return g;
}

QString groupTitle(const QString& g)
{
    if (g == u"Rules")
        return QStringLiteral("Per-game and per-image rules");
    if (g == u"Sectors")
        return QStringLiteral("Sector sizes");
    if (g == u"API")
        return QStringLiteral("HTTP API and web UI");
    if (g == u"Fan")
        return QStringLiteral("Fan");
    return g;
}

QString groupCaption(const QString& g)
{
    if (g == u"General")
        return QStringLiteral("Logging, notifications, language");
    if (g == u"Scanning")
        return QStringLiteral("Where and how often games are found");
    if (g == u"Mounting")
        return QStringLiteral("How images are mounted");
    if (g == u"Library")
        return QStringLiteral("Registering and removing games");
    if (g == u"Fakelib")
        return QStringLiteral("Library overlays for backported games");
    if (g == u"Emulators")
        return QStringLiteral("Emulator and AMPR updates");
    if (g == u"Rules")
        return QStringLiteral("profiles.ini in the v2 format");
    if (g == u"Sectors")
        return QStringLiteral("Only change these when a guide says so");
    return {};
}

// What the form shows for a key that is not in the file (a switch without an
// upstream default reads as off).
QString rowDefault(const SmpKey& k)
{
    return k.defaultValue.isEmpty() && k.type == SmpKeyType::Bool ? QStringLiteral("0") : k.defaultValue;
}

QString shortValue(const QString& v)
{
    const QString one = QString(v).replace(u'\n', QStringLiteral(", "));
    return one.isEmpty() ? QStringLiteral("(default)") : elideMiddle(one, 48);
}

} // namespace

ShadowMountTab::ShadowMountTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent)
    : ConsoleToolTab(ctx, std::move(endpointFn), QStringLiteral("mount"), parent)
{
    auto* l = vbox(this, space::lg);

    // ---- Who is running, which format; Load / Push.
    auto* head = new Card(this);
    auto* hh = new QHBoxLayout;
    hh->setSpacing(space::md);
    m_tile = new IconTile(QStringLiteral("layers"), 44, head);
    hh->addWidget(m_tile, 0, Qt::AlignTop);
    auto* names = new QVBoxLayout;
    names->setSpacing(3);
    auto* tr = new QHBoxLayout;
    tr->setSpacing(space::sm);
    tr->addWidget(label(QStringLiteral("ShadowMount+"), TextStyle::Subtitle, color::fg_0, head));
    m_formatBadge = new Badge(QStringLiteral("Not loaded"), Tone::Neutral, head);
    m_formatBadge->setIconName({});
    tr->addWidget(m_formatBadge);
    tr->addStretch(1);
    names->addLayout(tr);
    m_meta = caption(QStringLiteral("Load the config to see what the console has."), head);
    m_meta->setWordWrap(true);
    names->addWidget(m_meta);
    hh->addLayout(names, 1);
    m_load = button(QStringLiteral("Load from console"), ButtonVariant::Secondary, QStringLiteral("download"), head);
    m_push = button(QStringLiteral("Push changes"), ButtonVariant::Primary, QStringLiteral("upload"), head);
    m_push->setEnabled(false);
    hh->addWidget(m_load, 0, Qt::AlignTop);
    hh->addWidget(m_push, 0, Qt::AlignTop);
    head->body()->addLayout(hh);
    m_status = new MessageRow(head);
    head->body()->addWidget(m_status);
    l->addWidget(head);

    // ---- Drive actions.
    auto* actions = new QWidget(this);
    auto* ag = new QGridLayout(actions);
    ag->setContentsMargins(0, 0, 0, 0);
    ag->setSpacing(space::md);
    m_stop = new ActionCard(QStringLiteral("eject"), QStringLiteral("Safe to unplug"),
                            QStringLiteral("Stops ShadowMount+ and waits until it has released every image."), actions);
    m_eject = new ActionCard(QStringLiteral("usb"), QStringLiteral("Eject USB drive"),
                             QStringLiteral("Unmounts one USB drive and keeps ShadowMount+ running (1.7)."), actions);
    m_loader = new ActionCard(QStringLiteral("rocket"), QStringLiteral("Send the loader"),
                              QStringLiteral("Sends shadowmountplus.elf to the console's ELF loader."), actions);
    m_stop->setAccessibleName(QStringLiteral("Safe to unplug"));
    m_eject->setAccessibleName(QStringLiteral("Eject USB drive"));
    ag->addWidget(m_stop, 0, 0);
    ag->addWidget(m_eject, 0, 1);
    ag->addWidget(m_loader, 0, 2);
    for (int c = 0; c < 3; ++c)
        ag->setColumnStretch(c, 1);
    l->addWidget(actions);
    connect(m_stop, &QAbstractButton::clicked, this, &ShadowMountTab::safeToUnplug);
    connect(m_eject, &QAbstractButton::clicked, this, &ShadowMountTab::ejectUsb);
    connect(m_loader, &QAbstractButton::clicked, this, &ShadowMountTab::sendLoader);
    m_stop->setEnabled(jobs::isAvailable(m_ctx, ps5::jobkinds::kShadowMountStop));
    m_eject->setEnabled(jobs::isAvailable(m_ctx, ps5::jobkinds::kShadowMountEject));
    m_loader->setEnabled(jobs::isAvailable(m_ctx, ps5::jobkinds::kPayloadSend));

    // ---- The settings, one folded section per group.
    m_problems = new InfoBar(Tone::Warn, this);
    m_problems->hide();
    l->addWidget(m_problems);
    for (const QString& g : groupOrder()) {
        auto* d = new Disclosure(groupTitle(g), groupCaption(g), this);
        d->setObjectName(QStringLiteral("smp-group-%1").arg(g.toLower()));
        auto* grid = new QWidget(d);
        auto* gl = vbox(grid, 0);
        bool any = false;
        for (const SmpKey& k : ps5::smpKeys()) {
            if (k.group != g)
                continue;
            gl->addWidget(buildRow(k, grid));
            any = true;
        }
        if (!any) {
            delete d;
            continue;
        }
        d->body()->addWidget(grid);
        d->setExpanded(g == u"General" || g == u"Scanning" || g == u"Mounting");
        m_groups << d;
        l->addWidget(d);
    }
    m_unknown = new Disclosure(QStringLiteral("Other lines in config.ini"), QStringLiteral("Kept exactly as they are when you push"), this);
    m_unknownText = label({}, TextStyle::Mono, color::fg_2, m_unknown);
    m_unknownText->setWordWrap(true);
    m_unknownText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_unknown->body()->addWidget(m_unknownText);
    m_unknown->hide();
    l->addWidget(m_unknown);

    // ---- MicroMount (archived upstream).
    auto* mm = new Disclosure(QStringLiteral("MicroMount (archived)"),
                              QStringLiteral("Legacy: ShadowMount+ 1.6test15-fix2 and later mount .ffpfsc too"), this);
    mm->body()->addWidget(buildMicroMount(mm));
    mm->setSummary(QStringLiteral("Legacy"), Tone::Neutral);
    l->addWidget(mm);
    l->addStretch(1);

    connect(m_load, &QPushButton::clicked, this, [this] {
        // Loading replaces the form: ask when it holds changes that were not pushed.
        if (m_hasLoaded && !changes().isEmpty()
            && !Sheet::confirm(window(), QStringLiteral("Discard the changes?"),
                               QStringLiteral("The form has changes that were not pushed to the console. Loading replaces them with what the console has."),
                               QStringLiteral("Keep editing"), QStringLiteral("Load"), true, Sheet::Icon::Warning))
            return;
        load();
    });
    connect(m_push, &QPushButton::clicked, this, [this] { push(false); });
    // Load and Push are for an idle tab: one piece of console work at a time.
    connect(this, &ConsoleToolTab::busyChanged, this, [this](bool busy) {
        m_load->setEnabled(!busy);
        refreshMarks();
    });
    populate({});
    updateHeader();
}

QWidget* ShadowMountTab::buildRow(const SmpKey& k, QWidget* parent)
{
    Row r;
    r.def = &k;
    auto* box = new QWidget(parent);
    r.box = box;
    box->setObjectName(QStringLiteral("smp-%1").arg(k.key));
    const bool tall = k.repeatable;
    auto* h = new QGridLayout(box);
    h->setContentsMargins(0, space::sm, 0, space::sm);
    h->setHorizontalSpacing(space::md);
    h->setVerticalSpacing(space::xs);
    r.label = label(k.label, TextStyle::Body, color::fg_1, box);
    const QString tip = QStringLiteral("%1\n\nconfig.ini: %2%3%4")
                            .arg(k.help, k.key, k.section.isEmpty() ? QStringLiteral(" (profiles.ini in v2)") : QStringLiteral(" · v2 [%1] %2").arg(k.section, k.v2Key),
                                 k.defaultValue.isEmpty() ? QString() : QStringLiteral("\nDefault: %1").arg(k.defaultValue));
    r.label->setToolTip(tip);
    QWidget* control = nullptr;
    switch (k.type) {
    case SmpKeyType::Bool:
        r.toggle = new Switch(box);
        control = r.toggle;
        connect(r.toggle, &QAbstractButton::toggled, this, [this, key = k.key](bool) {
            if (!m_populating) {
                m_reset.remove(key);
                refreshMarks();
            }
        });
        break;
    case SmpKeyType::Int:
        r.spin = new QSpinBox(box);
        r.spin->setRange(int(k.minimum), int(k.maximum));
        r.spin->setFixedWidth(150);
        if (k.key.endsWith(QLatin1String("_seconds")))
            r.spin->setSuffix(QStringLiteral(" s"));
        control = r.spin;
        connect(r.spin, &QSpinBox::valueChanged, this, [this, key = k.key](int) {
            if (!m_populating) {
                m_reset.remove(key);
                refreshMarks();
            }
        });
        break;
    case SmpKeyType::Enum:
    case SmpKeyType::PowerOfTwo:
    case SmpKeyType::Temperature: {
        r.combo = dropdown(box);
        r.combo->setFixedWidth(150);
        if (k.type == SmpKeyType::Enum)
            for (const QString& c : k.choices)
                r.combo->addItem(c, c);
        else if (k.type == SmpKeyType::PowerOfTwo)
            for (qint64 n = 512; n <= 1048576; n *= 2)
                r.combo->addItem(QString::number(n), QString::number(n));
        else {
            r.combo->addItem(QStringLiteral("System"), QStringLiteral("system"));
            for (int t = 50; t <= 91; ++t)
                r.combo->addItem(QStringLiteral("%1 °C").arg(t), QString::number(t));
        }
        control = r.combo;
        connect(r.combo, &QComboBox::currentIndexChanged, this, [this, key = k.key](int) {
            if (!m_populating) {
                m_reset.remove(key);
                refreshMarks();
            }
        });
        break;
    }
    case SmpKeyType::PathList:
    case SmpKeyType::TitleIdList:
    case SmpKeyType::FakelibRules:
    case SmpKeyType::ImageNames:
    case SmpKeyType::ImageSectors:
        r.text = new QPlainTextEdit(box);
        r.text->setFont(font::mono());
        r.text->setFixedHeight(76);
        r.text->setPlaceholderText(k.help);
        r.text->setAccessibleName(k.label);
        control = r.text;
        connect(r.text, &QPlainTextEdit::textChanged, this, [this, key = k.key] {
            if (!m_populating) {
                m_reset.remove(key);
                refreshMarks();
            }
        });
        break;
    default:
        r.edit = textInput(k.defaultValue, k.type != SmpKeyType::Text, box);
        r.edit->setMinimumWidth(220);
        control = r.edit;
        connect(r.edit, &QLineEdit::textEdited, this, [this, key = k.key](const QString&) {
            m_reset.remove(key);
            refreshMarks();
        });
        break;
    }
    control->setAccessibleName(k.label);
    control->setToolTip(tip);
    r.reset = new IconButton(QStringLiteral("undo"), QStringLiteral("Back to the default (removes the line)"), IconButton::Size::Tiny, box);
    connect(r.reset, &QToolButton::clicked, this, [this, key = k.key] { resetToDefault(key); });
    if (tall) {
        h->addWidget(r.label, 0, 0);
        h->addWidget(r.reset, 0, 1, Qt::AlignRight);
        h->addWidget(control, 1, 0, 1, 2);
        h->setColumnStretch(0, 1);
    } else {
        h->addWidget(r.label, 0, 0, Qt::AlignVCenter);
        h->addWidget(control, 0, 1, Qt::AlignRight | Qt::AlignVCenter);
        h->addWidget(r.reset, 0, 2, Qt::AlignVCenter);
        h->setColumnStretch(0, 1);
    }
    m_rows.insert(k.key, r);
    return box;
}

QString ShadowMountTab::rowValue(const Row& r) const
{
    if (r.toggle)
        return r.toggle->isChecked() ? QStringLiteral("1") : QStringLiteral("0");
    if (r.spin)
        return QString::number(r.spin->value());
    if (r.combo)
        return r.combo->currentData().toString();
    if (r.text) {
        QStringList lines;
        for (const QString& raw : r.text->toPlainText().split(u'\n'))
            if (!raw.trimmed().isEmpty())
                lines << raw.trimmed();
        return lines.join(u'\n');
    }
    if (r.edit)
        return r.edit->text().trimmed().isEmpty() ? r.def->defaultValue : r.edit->text().trimmed();
    return {};
}

void ShadowMountTab::setRowValue(Row& r, const QString& v)
{
    if (r.toggle)
        r.toggle->setChecked(v.trimmed() == u"1");
    else if (r.spin)
        r.spin->setValue(v.toInt());
    else if (r.combo) {
        int i = r.combo->findData(v.trimmed().toLower() == u"system" ? QStringLiteral("system") : v.trimmed());
        if (i < 0 && !v.isEmpty()) {
            r.combo->addItem(v, v); // an unusual value from the console: shown as it is
            i = r.combo->count() - 1;
        }
        r.combo->setCurrentIndex(std::max(0, i));
    } else if (r.text)
        r.text->setPlainText(v);
    else if (r.edit)
        r.edit->setText(v);
}

void ShadowMountTab::populate(const MountConfigValues& values)
{
    m_populating = true;
    for (auto it = m_rows.begin(); it != m_rows.end(); ++it) {
        const QString v = values.contains(it.key()) ? values.value(it.key()) : rowDefault(*it->def);
        setRowValue(*it, v);
    }
    m_populating = false;
    m_reset.clear();
    // Lines the form does not show: unknown keys (kept on push).
    QStringList other;
    for (auto it = values.cbegin(); it != values.cend(); ++it)
        if (!m_rows.contains(it.key()))
            other << QStringLiteral("%1 = %2").arg(it.key(), QString(it.value()).replace(u'\n', QStringLiteral(" · ")));
    m_unknownText->setText(other.join(u'\n'));
    m_unknown->setVisible(!other.isEmpty());
    m_unknown->setSummary(QStringLiteral("%1 kept").arg(other.size()), Tone::Neutral);
    refreshMarks();
}

MountConfigValues ShadowMountTab::editedValues() const
{
    MountConfigValues out = m_loaded; // unknown and obsolete keys stay as loaded
    for (auto it = m_rows.cbegin(); it != m_rows.cend(); ++it) {
        const QString& key = it.key();
        if (m_reset.contains(key)) {
            out.remove(key);
            continue;
        }
        const QString v = rowValue(*it);
        const QString before = m_loaded.contains(key) ? m_loaded.value(key) : rowDefault(*it->def);
        if (ps5::changedMountValues({{key, before}}, {{key, v}}).isEmpty())
            continue; // unchanged: as loaded (or absent)
        if (v.isEmpty())
            out.remove(key);
        else
            out.insert(key, v);
    }
    return out;
}

void ShadowMountTab::refreshMarks()
{
    const MountConfigValues edited = editedValues();
    const MountConfigValues ch = ps5::changedMountValues(m_loaded, edited);
    for (auto it = m_rows.begin(); it != m_rows.end(); ++it) {
        const bool changed = ch.contains(it.key());
        const QString current = rowValue(*it);
        const bool atDefault = !m_loaded.contains(it.key()) && !changed;
        it->label->setText(changed ? QStringLiteral("● %1").arg(it->def->label) : it->def->label);
        setTextColor(it->label, changed ? color::accent_hi : color::fg_1);
        it->reset->setVisible(!atDefault || (changed && current != rowDefault(*it->def)));
    }
    // Problems and warnings for what would be pushed.
    const QStringList problems = ps5::validateMountConfig(ps5::MountTool::ShadowMountPlus, edited, m_hasLoaded && m_format != ps5::SmpConfigFormat::V2);
    const QStringList warnings = ps5::mountConfigWarnings(ps5::MountTool::ShadowMountPlus, edited);
    if (!problems.isEmpty()) {
        m_problems->setTone(Tone::Err);
        m_problems->setTitle(QStringLiteral("Fix before pushing"));
        m_problems->setMessage(problems.join(u'\n'));
        m_problems->show();
    } else if (!warnings.isEmpty()) {
        m_problems->setTone(Tone::Warn);
        m_problems->setTitle({});
        m_problems->setMessage(warnings.join(u'\n'));
        m_problems->show();
    } else {
        m_problems->hide();
    }
    m_push->setEnabled(m_hasLoaded && !ch.isEmpty() && problems.isEmpty() && !isBusy());
    m_push->setText(ch.isEmpty() ? QStringLiteral("Push changes") : QStringLiteral("Push %1 change%2").arg(ch.size()).arg(ch.size() == 1 ? QString() : QStringLiteral("s")));
}

void ShadowMountTab::setValue(const QString& key, const QString& value)
{
    if (auto it = m_rows.find(key); it != m_rows.end()) {
        setRowValue(*it, value);
        m_reset.remove(key);
        refreshMarks();
    }
}

QString ShadowMountTab::value(const QString& key) const
{
    const auto it = m_rows.find(key);
    return it == m_rows.end() ? QString() : rowValue(*it);
}

void ShadowMountTab::resetToDefault(const QString& key)
{
    auto it = m_rows.find(key);
    if (it == m_rows.end())
        return;
    m_populating = true;
    setRowValue(*it, rowDefault(*it->def));
    m_populating = false;
    if (m_loaded.contains(key))
        m_reset.insert(key);
    refreshMarks();
}

void ShadowMountTab::setStatus(const QString& text, Tone tone)
{
    m_status->set(text, tone);
}

QString ShadowMountTab::statusText() const
{
    return m_status->text();
}

void ShadowMountTab::setApiVersion(const QString& version)
{
    m_apiVersion = version;
    updateHeader();
}

void ShadowMountTab::updateHeader()
{
    // Keys that only exist in the v2 format show only for a v2 config.
    for (auto it = m_rows.begin(); it != m_rows.end(); ++it)
        it->box->setVisible(!it->def->v2Only || (m_hasLoaded && m_format == ps5::SmpConfigFormat::V2));
    const QString version = !m_apiVersion.isEmpty() ? m_apiVersion : m_logVersion;
    QStringList meta;
    meta << (version.isEmpty() ? QStringLiteral("Version unknown") : QStringLiteral("Version %1").arg(version));
    if (!m_apiVersion.isEmpty())
        meta << QStringLiteral("HTTP API on port %1").arg(endpoint().smpApiPort);
    if (m_hasLoaded)
        meta << (m_configExists ? ps5::mountConfigRemotePath(ps5::MountTool::ShadowMountPlus)
                                : QStringLiteral("no config.ini yet: defaults shown"));
    else
        meta << QStringLiteral("load the config to edit it");
    m_meta->setText(meta.join(QStringLiteral(" · ")));
    m_tile->setTone(version.isEmpty() ? Tone::Info : Tone::Ok);
    if (m_hasLoaded) {
        m_formatBadge->set(QStringLiteral("Config %1").arg(ps5::smpConfigFormatName(m_format)), m_format == ps5::SmpConfigFormat::Flat16 ? Tone::Warn : Tone::Info);
        m_formatBadge->setToolTip(m_format == ps5::SmpConfigFormat::Flat16 ? QStringLiteral("1.6 or unknown version: kstuff_* lines are kept")
                                                                           : QStringLiteral("Obsolete kstuff_* lines are removed on push"));
    }
    m_eject->setEnabled(jobs::isAvailable(m_ctx, ps5::jobkinds::kShadowMountEject) && (m_apiVersion.isEmpty() || ps5::smpVersionAtLeast(m_apiVersion, QStringLiteral("1.7"))));
}

void ShadowMountTab::load()
{
    const QString apiVersion = m_apiVersion;
    const quint16 apiPort = endpoint().smpApiPort;
    const QString consoleHost = endpoint().host; // the console this load asks, whatever the address field says when it answers
    setStatus(QStringLiteral("Loading config.ini, profiles.ini and the version…"), Tone::Info);
    struct Loaded {
        ps5::SmpRemoteState state;
        QString apiVersion;
    };
    const bool started = runOnConsole<Loaded>(
        [apiVersion, apiPort](ps5::FtpClient& c, const CancellationToken& t) -> Result<Loaded> {
            Loaded out;
            out.apiVersion = apiVersion;
            if (out.apiVersion.isEmpty()) {
                ps5::smp::ApiOptions api;
                api.connectTimeoutMs = 1500;
                api.replyTimeoutMs = 3000;
                if (auto v = ps5::smp::version(c.host(), apiPort, api, t); v.ok())
                    out.apiVersion = v->version;
            }
            FW_TRY_ASSIGN(out.state, ps5::loadSmpState(c, out.apiVersion, t));
            return out;
        },
        [this, consoleHost](const Result<Loaded>& r) {
            if (!r.ok()) {
                setStatus(r.error().message, r.error().isCancelled() ? Tone::Neutral : Tone::Err);
                refreshMarks();
                return;
            }
            if (!r->apiVersion.isEmpty())
                m_apiVersion = r->apiVersion;
            finishLoad(r->state, consoleHost);
        });
    if (!started)
        setStatus(whyNotStarted(), Tone::Warn);
}

void ShadowMountTab::finishLoad(const ps5::SmpRemoteState& s, const QString& consoleHost)
{
    m_config = s.config;
    m_profiles = s.profiles;
    m_configExists = s.configExists;
    m_profilesExists = s.profilesExists;
    m_logVersion = s.logVersion;
    m_format = s.format;
    m_loaded = ps5::parseMountConfig(ps5::MountTool::ShadowMountPlus, s.config, s.profiles);
    m_hasLoaded = true;
    m_loadedHost = consoleHost; // Push is for this console only
    populate(m_loaded);
    updateHeader();
    setStatus(s.configExists ? QStringLiteral("Loaded. Only the settings you change are written back.")
                             : QStringLiteral("No config.ini on the console yet; the defaults are shown."),
              Tone::Ok);
    emit loaded();
}

void ShadowMountTab::push(bool confirmed)
{
    if (!m_hasLoaded) {
        setStatus(QStringLiteral("Load the config from the console first."), Tone::Warn);
        return;
    }
    if (!m_loadedHost.isEmpty() && m_loadedHost != endpoint().host) {
        setStatus(QStringLiteral("This config was loaded from %1. Load it from this console first.").arg(m_loadedHost), Tone::Warn);
        return;
    }
    const MountConfigValues edited = editedValues();
    const QStringList problems = ps5::validateMountConfig(ps5::MountTool::ShadowMountPlus, edited, m_hasLoaded && m_format != ps5::SmpConfigFormat::V2);
    if (!problems.isEmpty()) {
        setStatus(problems.first(), Tone::Err);
        return;
    }
    const MountConfigValues ch = ps5::changedMountValues(m_loaded, edited);
    if (ch.isEmpty()) {
        setStatus(QStringLiteral("Nothing changed."), Tone::Neutral);
        return;
    }
    const ps5::MountConfigFiles files = ps5::applyMountConfigChanges(ps5::MountTool::ShadowMountPlus, m_config, m_profiles, ch, m_format);
    // What the new files were made from: the push is refused when the console's differ (also when a
    // load finishes while the question below is open).
    const std::optional<QString> expectConfig = m_configExists ? m_config : QString();
    const std::optional<QString> expectProfiles = m_profilesExists ? m_profiles : QString();
    if (!confirmed) {
        QStringList lines;
        for (auto it = ch.cbegin(); it != ch.cend(); ++it)
            lines << QStringLiteral("%1: %2 → %3").arg(it.key(), shortValue(m_loaded.value(it.key())), shortValue(it.value()));
        QString where = ps5::mountConfigRemotePath(ps5::MountTool::ShadowMountPlus);
        if (files.profilesChanged)
            where += QStringLiteral(" and ") + ps5::smpProfilesRemotePath();
        const QString debugOff = edited.value(QStringLiteral("debug"), QStringLiteral("1")) == u"0"
                                     ? QStringLiteral("<br><br>debug is off: Safe to unplug cannot confirm the cleanup then.")
                                     : QString();
        if (!Sheet::confirm(window(), QStringLiteral("Write %1 change%2 to the console?").arg(ch.size()).arg(ch.size() == 1 ? QString() : QStringLiteral("s")),
                            QStringLiteral("%1<br><br>Only these lines change in %2; everything else stays. The current files are saved to this PC first.%3")
                                .arg(Sheet::monoLines(lines), Sheet::monoSpan(where), debugOff),
                            QStringLiteral("Cancel"), QStringLiteral("Push"), false, Sheet::Icon::Info))
            return;
    }
    const QString backupDir = ps5::newConsoleBackupDir();
    const QString apiVersion = m_apiVersion;
    const QString consoleHost = endpoint().host;
    setStatus(QStringLiteral("Pushing…"), Tone::Info);
    m_push->setEnabled(false);
    const bool started = runOnConsole<ps5::SmpRemoteState>(
        [files, backupDir, expectConfig, expectProfiles, apiVersion](ps5::FtpClient& c, const CancellationToken& t) -> Result<ps5::SmpRemoteState> {
            if (files.configChanged)
                FW_TRY(ps5::pushMountConfig(c, ps5::MountTool::ShadowMountPlus, files.config, backupDir, t, expectConfig));
            if (files.profilesChanged)
                FW_TRY(ps5::pushSmpProfiles(c, files.profiles, backupDir, t, expectProfiles));
            return ps5::loadSmpState(c, apiVersion, t);
        },
        [this, n = ch.size(), consoleHost](const Result<ps5::SmpRemoteState>& r) {
            if (!r.ok()) {
                setStatus(r.error().message, Tone::Err);
                emit toastRequested(Tone::Err, QStringLiteral("Not pushed"), r.error().message);
                refreshMarks();
                return;
            }
            finishLoad(r.value(), consoleHost);
            setStatus(QStringLiteral("%1 change%2 written; ShadowMount+ reloads its config by itself.").arg(n).arg(n == 1 ? QString() : QStringLiteral("s")), Tone::Ok);
            emit toastRequested(Tone::Ok, QStringLiteral("Config pushed"), QStringLiteral("%1 setting%2 changed").arg(n).arg(n == 1 ? QString() : QStringLiteral("s")));
        });
    if (!started) {
        setStatus(whyNotStarted(), Tone::Warn); // nothing runs: not "Pushing…"
        refreshMarks();
    }
}

void ShadowMountTab::safeToUnplug()
{
    const ps5::ConsoleEndpoint ep = endpoint();
    if (ep.host.isEmpty()) {
        emit toastRequested(Tone::Warn, QStringLiteral("No console address"), QStringLiteral("Enter the console's IP address at the top of the page first."));
        return;
    }
    if (m_hasLoaded && m_loaded.value(QStringLiteral("debug"), QStringLiteral("1")) == u"0"
        && !Sheet::confirm(window(), QStringLiteral("debug is off"),
                           QStringLiteral("With debug=0 ShadowMount+ logs nothing, so the cleanup cannot be confirmed: the result will "
                                          "stay amber. Send STOP anyway?"),
                           QStringLiteral("Cancel"), QStringLiteral("Send STOP"), false, Sheet::Icon::Warning))
        return;
    auto r = jobs::submitShadowMountStop(m_ctx, ep.host, ep.ftpPort);
    if (!r.ok()) {
        emit toastRequested(Tone::Err, QStringLiteral("Not started"), r.error().message);
        return;
    }
    m_ctx.queue().startNow(r.value()); // this job only; a paused queue stays paused
    setStatus(QStringLiteral("STOP sent. Wait for the green confirmation on the Queue page before unplugging."), Tone::Info);
    emit toastRequested(Tone::Info, QStringLiteral("Stopping ShadowMount+"), QStringLiteral("Wait for the green confirmation before unplugging."));
}

void ShadowMountTab::ejectUsb()
{
    const ps5::ConsoleEndpoint ep = endpoint();
    if (ep.host.isEmpty()) {
        emit toastRequested(Tone::Warn, QStringLiteral("No console address"), QStringLiteral("Enter the console's IP address at the top of the page first."));
        return;
    }
    setStatus(QStringLiteral("Asking ShadowMount+ which drives can be ejected…"), Tone::Info);
    const bool started = runLocal<std::vector<ps5::smp::StorageItem>>(
        [ep](const CancellationToken& t) { return ps5::smp::storage(ep.host, ep.smpApiPort, {}, t); },
        [this, ep](const Result<std::vector<ps5::smp::StorageItem>>& r) {
            if (!r.ok()) {
                setStatus(QStringLiteral("%1 Eject needs ShadowMount+ 1.7 with its API on; use Safe to unplug instead.").arg(r.error().message), Tone::Warn);
                return;
            }
            QStringList drives;
            for (const auto& i : r.value())
                if (i.ejectable)
                    drives << i.mountPoint;
            if (drives.isEmpty()) {
                setStatus(QStringLiteral("ShadowMount+ reports no USB drive that can be ejected."), Tone::Neutral);
                return;
            }
            QString path = drives.front();
            if (drives.size() > 1) {
                auto* s = new Sheet(window(), QStringLiteral("Eject which drive?"), Sheet::Icon::None);
                auto* pick = dropdown(s);
                pick->addItems(drives);
                s->body()->addWidget(field(QStringLiteral("Drive"), pick, {}, s));
                s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
                s->addButton(QStringLiteral("Eject"), Sheet::Role::Primary);
                if (s->exec() != 1)
                    return;
                path = pick->currentText();
            } else if (!Sheet::confirm(window(), QStringLiteral("Eject %1?").arg(path),
                                       QStringLiteral("ShadowMount+ unmounts the drive and keeps running. Close games that run from it first."),
                                       QStringLiteral("Cancel"), QStringLiteral("Eject"), false, Sheet::Icon::Info)) {
                return;
            }
            auto id = jobs::submitShadowMountEject(m_ctx, ep.host, int(ep.smpApiPort), path);
            if (!id.ok()) {
                setStatus(id.error().message, Tone::Err);
                return;
            }
            m_ctx.queue().startNow(id.value()); // this job only; a paused queue stays paused
            setStatus(QStringLiteral("Ejecting %1. Unplug it when the Queue says it was ejected.").arg(path), Tone::Info);
            emit toastRequested(Tone::Info, QStringLiteral("Ejecting %1").arg(path), QStringLiteral("Unplug it when the queue says it was ejected."));
        });
    if (!started)
        setStatus(whyNotStarted(), Tone::Neutral);
}

void ShadowMountTab::sendLoader()
{
    const ps5::ConsoleEndpoint ep = endpoint();
    if (ep.host.isEmpty()) {
        emit toastRequested(Tone::Warn, QStringLiteral("No console address"), QStringLiteral("Enter the console's IP address at the top of the page first."));
        return;
    }
    const QString file = QFileDialog::getOpenFileName(window(), QStringLiteral("Choose shadowmountplus.elf"), {},
                                                      QStringLiteral("ELF payloads (*.elf *.bin);;All files (*)"));
    if (file.isEmpty())
        return;
    auto r = jobs::submitPayload(m_ctx, ep.host, ep.shadowMountPort, file);
    if (!r.ok()) {
        emit toastRequested(Tone::Err, QStringLiteral("Payload not sent"), r.error().message);
        return;
    }
    m_ctx.queue().startNow(r.value()); // this job only; a paused queue stays paused
    setStatus(QStringLiteral("Sending %1 to %2:%3.").arg(QFileInfo(file).fileName(), ep.host).arg(ep.shadowMountPort), Tone::Info);
}

// ---- MicroMount (archived) --------------------------------------------------------------------

QWidget* ShadowMountTab::buildMicroMount(QWidget* parent)
{
    auto* box = new QWidget(parent);
    auto* l = vbox(box, space::md);
    auto* note = new InfoBar(Tone::Info, box);
    note->setMessage(QStringLiteral("MicroMount is archived upstream. Do not run it together with ShadowMount+: both would mount the same images."));
    l->addWidget(note);
    m_mmTable = new QTableWidget(0, 2, box);
    styleToolTable(m_mmTable, 8);
    m_mmTable->setHorizontalHeaderLabels({QStringLiteral("Key"), QStringLiteral("Value")});
    m_mmTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_mmTable->setColumnWidth(0, 220);
    m_mmTable->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    l->addWidget(m_mmTable);
    auto* row = new QWidget(box);
    auto* rh = hbox(row, space::sm);
    m_mmStatus = caption(QStringLiteral("Defaults shown. List values are one per line in config.ini; separate them with ; here."), row);
    m_mmStatus->setWordWrap(true);
    rh->addWidget(m_mmStatus, 1);
    auto* load = button(QStringLiteral("Load"), ButtonVariant::Secondary, QStringLiteral("download"), row);
    auto* defaults = button(QStringLiteral("Defaults"), ButtonVariant::Subtle, QStringLiteral("undo"), row);
    m_mmPush = button(QStringLiteral("Push"), ButtonVariant::Secondary, QStringLiteral("upload"), row);
    rh->addWidget(defaults);
    rh->addWidget(load);
    rh->addWidget(m_mmPush);
    l->addWidget(row);
    auto show = [this](const MountConfigValues& v) {
        m_mmTable->setRowCount(0);
        QStringList keys = ps5::mountConfigKeys(ps5::MountTool::MicroMount);
        for (auto it = v.cbegin(); it != v.cend(); ++it)
            if (!keys.contains(it.key()))
                keys << it.key();
        for (const QString& k : keys) {
            const int r = m_mmTable->rowCount();
            m_mmTable->insertRow(r);
            auto* key = toolItem(k, true);
            key->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            m_mmTable->setItem(r, 0, key);
            m_mmTable->setItem(r, 1, toolItem(QString(v.value(k)).replace(u'\n', QStringLiteral("; ")), true, color::fg_0));
        }
    };
    auto values = [this] {
        MountConfigValues out;
        for (int r = 0; r < m_mmTable->rowCount(); ++r) {
            QString v = m_mmTable->item(r, 1) ? m_mmTable->item(r, 1)->text().trimmed() : QString();
            v.replace(QStringLiteral("; "), QStringLiteral("\n")).replace(u';', u'\n');
            if (!v.isEmpty())
                out.insert(m_mmTable->item(r, 0)->text(), v);
        }
        return out;
    };
    show(ps5::mountConfigDefaults(ps5::MountTool::MicroMount));
    connect(defaults, &QPushButton::clicked, this, [show] { show(ps5::mountConfigDefaults(ps5::MountTool::MicroMount)); });
    connect(load, &QPushButton::clicked, this, [this, show] {
        m_mmStatus->setText(QStringLiteral("Loading %1…").arg(ps5::mountConfigRemotePath(ps5::MountTool::MicroMount)));
        const bool started = runOnConsole<QString>([](ps5::FtpClient& c, const CancellationToken& t) { return ps5::loadMountConfig(c, ps5::MountTool::MicroMount, t); },
                              [this, show](const Result<QString>& r) {
                                  if (r.ok()) {
                                      show(ps5::parseMountConfig(ps5::MountTool::MicroMount, r.value()));
                                      m_mmStatus->setText(QStringLiteral("Loaded from the console."));
                                  } else {
                                      m_mmStatus->setText(r.error().message);
                                  }
                              });
        if (!started)
            m_mmStatus->setText(whyNotStarted());
    });
    connect(m_mmPush, &QPushButton::clicked, this, [this, values] {
        const MountConfigValues v = values();
        const QStringList problems = ps5::validateMountConfig(ps5::MountTool::MicroMount, v);
        if (!problems.isEmpty()) {
            m_mmStatus->setText(problems.join(u' '));
            return;
        }
        if (!Sheet::confirm(window(), QStringLiteral("Write %1 to the console?").arg(ps5::mountConfigRemotePath(ps5::MountTool::MicroMount)),
                            QStringLiteral("The current file on the PS5 is saved to this PC first, then the new one is swapped in."),
                            QStringLiteral("Cancel"), QStringLiteral("Push"), false, Sheet::Icon::Info))
            return;
        const QString text = ps5::renderMountConfig(ps5::MountTool::MicroMount, v);
        const QString backupDir = ps5::newConsoleBackupDir();
        runOnConsole<ps5::MountPushResult>(
            [text, backupDir](ps5::FtpClient& c, const CancellationToken& t) {
                return ps5::pushMountConfig(c, ps5::MountTool::MicroMount, text, backupDir, t);
            },
            [this](const Result<ps5::MountPushResult>& r) {
                m_mmStatus->setText(r.ok() ? QStringLiteral("Written to the console.") : r.error().message);
            });
    });
    return box;
}

// ---- demo ---------------------------------------------------------------------------------

void ShadowMountTab::applyDemo()
{
    ps5::SmpRemoteState s;
    s.configExists = true;
    s.config = QStringLiteral("[config]\nversion = 2\n\n[general]\ndebug = on\nlanguage = en-US\n\n[scan]\ninterval_seconds = 30\n"
                              "path = /data/etaHEN/games\npath = /mnt/usb0/games\n\n[mount]\nmode = ro\npersistent_images = off\n\n"
                              "[smb:NAS]\npath = smb://nas/games\n");
    s.profilesExists = true;
    s.profiles = QStringLiteral("[config]\nversion = 2\n\n[game:PPSA01342]\nfakelib = emulators\n");
    s.logVersion = QStringLiteral("1.7beta11");
    s.format = ps5::SmpConfigFormat::V2;
    m_apiVersion = QStringLiteral("1.7beta11");
    finishLoad(s, endpoint().host);
    setValue(QStringLiteral("scan_interval_seconds"), QStringLiteral("15"));
    setValue(QStringLiteral("auto_update_ampr"), QStringLiteral("1"));
    setStatus(QStringLiteral("Example values, not from a console."), Tone::Info);
}

void ShadowMountTab::clearDemo()
{
    m_loaded.clear();
    m_config.clear();
    m_profiles.clear();
    m_configExists = m_profilesExists = false;
    m_hasLoaded = false;
    m_apiVersion.clear();
    m_logVersion.clear();
    m_formatBadge->set(QStringLiteral("Not loaded"), Tone::Neutral);
    populate({});
    updateHeader();
    m_status->clear();
}

} // namespace fw::pages
