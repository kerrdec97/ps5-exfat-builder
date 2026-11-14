#include "app/ui/SettingRow.h"

#include "app/AppContext.h"
#include "app/ui/Icons.h"
#include "core/Tr.h"
#include "engines/general/Module.h"

#include <QResizeEvent>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPainter>
#include <QVBoxLayout>

#include <algorithm>

namespace fw::ui {

using namespace theme;

// ---- schema texts as shown ----------------------------------------------------

QString shownLabel(const SettingDef& def) { return i18n::translate(def.label); }
QString shownDescription(const SettingDef& def) { return i18n::translate(def.description); }
QString shownChoiceLabel(const SettingDef& def, const SettingChoice& choice)
{
    if (def.key == general::keys::kLanguage)
        return choice.label; // "Deutsch (German)": a language's own name
    return i18n::translate(choice.label);
}
QString shownChoiceDescription(const SettingDef& def, const SettingChoice& choice)
{
    if (def.key == general::keys::kLanguage)
        return choice.description;
    return i18n::translate(choice.description);
}
QString shownSection(const QString& sectionOrGroup) { return i18n::translate(sectionOrGroup); }
QString shownValidationReason(const QString& message)
{
    return i18n::translate(message.section(QStringLiteral(": "), 1));
}
QString inSentence(const QString& shownValue)
{
    return i18n::currentCode() == u"en" ? shownValue.toLower() : shownValue;
}

// ---- editor -----------------------------------------------------------------

SettingEditor::SettingEditor(const SettingDef& def, QWidget* parent) : QObject(parent), m_def(def)
{
    build(parent);
}

static bool useSegmented(const SettingDef& def)
{
    if (def.type != SettingType::Enum || def.choices.size() < 2 || def.choices.size() > 3)
        return false;
    for (const auto& c : def.choices)
        if (shownChoiceLabel(def, c).size() > 12) // per language: a long translation gets the drop-down
            return false;
    return true;
}

// A StringList setting holds folders (Library folders) or wildcard patterns
// ("Leave out"). Only a folder list gets the folder picker and native
// separators; a pattern is shown and stored exactly as it is written.
static bool isFolderList(const SettingDef& def)
{
    return def.type == SettingType::StringList && def.folderList;
}

static QString listText(const QStringList& items) { return items.join(QStringLiteral("; ")); }

// What 0 means for the settings where it is not a quantity: the field says
// so beside the number instead of showing a bare "0" that reads like an error.
QString SettingEditor::zeroMeaning(const SettingDef& def)
{
    if (def.key == general::keys::kWorkerThreads || def.key == QLatin1String("pfs.threads")
        || def.key == QLatin1String("exfat.imageSizeGiB"))
        return FW_TR("Automatic");
    if (def.key == general::keys::kAutoRetryCount || def.key == QLatin1String("dlc.sdkOverride"))
        return FW_TR("Off");
    return {};
}

QString SettingEditor::numberText(const QString& typed, const QLocale& locale)
{
    const QString sep = locale.decimalPoint();
    QString t = typed.trimmed();
    // "1,5" where the comma is the decimal separator. A text that already
    // has a dot, or more than one separator, is left for the check to refuse.
    if (sep != QLatin1String(".") && !sep.isEmpty() && !t.contains(u'.') && t.count(sep) == 1)
        t.replace(sep, QStringLiteral("."));
    return t;
}

void SettingEditor::build(QWidget* parent)
{
    switch (m_def.type) {
    case SettingType::Bool: {
        auto* sw = new Switch(parent);
        sw->setAccessibleName(shownLabel(m_def));
        connect(sw, &QAbstractButton::toggled, this, [this](bool on) {
            if (!m_updating)
                emit edited(on);
        });
        m_widget = m_control = sw;
        break;
    }
    case SettingType::Enum: {
        if (useSegmented(m_def)) {
            auto* seg = new Segmented(parent);
            for (const auto& c : m_def.choices)
                seg->addSegment(shownChoiceLabel(m_def, c), {}, c.value);
            seg->setAccessibleName(shownLabel(m_def));
            connect(seg, &Segmented::currentChanged, this, [this, seg](int) {
                if (!m_updating)
                    emit edited(seg->currentValue());
            });
            m_widget = m_control = seg;
        } else {
            auto* combo = dropdown(parent);
            for (const auto& c : m_def.choices) {
                const QString name = shownChoiceLabel(m_def, c);
                combo->addItem(c.description.isEmpty() ? name : QStringLiteral("%1 · %2").arg(name, shownChoiceDescription(m_def, c)), c.value);
                // What the closed field shows when the whole text does not fit.
                if (!c.description.isEmpty())
                    combo->setItemData(combo->count() - 1, name, kShortTextRole);
            }
            // As wide as its longest choice (dropdown() adjusts to its
            // contents), from 160 to 320 px; a choice wider than that shows
            // its name alone, with the whole text as the tooltip.
            combo->setMinimumWidth(160);
            combo->setMaximumWidth(320);
            combo->setAccessibleName(shownLabel(m_def));
            auto tip = [combo] {
                const QString text = combo->currentText();
                const bool cut = QFontMetrics(combo->font()).horizontalAdvance(text) > combo->maximumWidth() - 44;
                combo->setToolTip(cut ? plainTip(text) : QString());
            };
            connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, tip](int) {
                tip();
                if (!m_updating)
                    emit edited(combo->currentData());
            });
            tip();
            m_widget = m_control = combo;
        }
        break;
    }
    case SettingType::Int:
    case SettingType::Double:
    case SettingType::String: {
        auto* wrap = new QWidget(parent);
        auto* h = hbox(wrap, space::sm);
        auto* edit = textInput({}, false, wrap);
        edit->setAccessibleName(shownLabel(m_def));
        const bool numeric = m_def.type != SettingType::String;
        edit->setFixedWidth(numeric ? 160 : 260);
        if (numeric)
            edit->setAlignment(Qt::AlignRight);
        h->addWidget(edit);
        // The unit inside the field: every control of a column ends on one line.
        if (!m_def.unit.isEmpty())
            setInputSuffix(edit, i18n::translate(m_def.unit));
        if (const QString zero = zeroMeaning(m_def); numeric && !zero.isEmpty()) {
            auto note = [edit, zero] {
                bool ok = false;
                const double v = numberText(edit->text()).toDouble(&ok);
                setInputNote(edit, ok && v == 0.0 ? zero : QString());
            };
            connect(edit, &QLineEdit::textChanged, edit, note);
            note();
        }
        connect(edit, &QLineEdit::editingFinished, this, [this, edit, numeric] {
            if (m_updating)
                return;
            const QString t = numeric ? numberText(edit->text()) : edit->text().trimmed();
            if (t == edit->property("fwLast").toString())
                return;
            edit->setProperty("fwLast", t);
            emit edited(t);
        });
        m_widget = wrap;
        m_control = edit;
        wrap->setFocusProxy(edit);
        break;
    }
    case SettingType::DirPath:
    case SettingType::FilePath: {
        auto* pf = new PathField(m_def.type == SettingType::DirPath ? PathField::Mode::Directory : PathField::Mode::OpenFile, parent);
        pf->setFilter(i18n::translate(m_def.fileFilter));
        pf->setDialogTitle(shownLabel(m_def));
        pf->setFixedWidth(360);
        pf->setPlaceholder(m_def.type == SettingType::DirPath ? FW_TR("Default") : FW_TR("Not set"));
        pf->edit()->setAccessibleName(shownLabel(m_def));
        connect(pf, &PathField::pathChanged, this, [this](const QString& p) {
            if (!m_updating)
                emit edited(p);
        });
        m_widget = pf;
        m_control = pf->edit();
        break;
    }
    case SettingType::StringList: {
        auto* wrap = new QWidget(parent);
        auto* h = hbox(wrap, space::sm);
        auto* edit = textInput(FW_TR("None"), true, wrap);
        edit->setAccessibleName(shownLabel(m_def));
        edit->setToolTip(FW_TR("Separate entries with ;"));
        h->addWidget(edit, 1);
        wrap->setFixedWidth(360);
        connect(edit, &QLineEdit::editingFinished, this, [this, edit] {
            if (m_updating)
                return;
            // Only a real change: leaving the field untouched is not an edit.
            const QString t = listText(value().toStringList());
            if (t == edit->property("fwLast").toString())
                return;
            edit->setProperty("fwLast", t);
            emit edited(value());
        });
        if (isFolderList(m_def)) {
            auto* add = button(FW_TR("Add…"), ButtonVariant::Secondary, QStringLiteral("folder_plus"), wrap);
            h->addWidget(add);
            connect(add, &QPushButton::clicked, this, [this, edit, wrap] {
                QPointer<SettingEditor> self(this);
                const QString dir = QFileDialog::getExistingDirectory(wrap->window(), shownLabel(m_def));
                if (!self || dir.isEmpty()) // the row can be rebuilt while the dialog is open
                    return;
                QStringList items = value().toStringList();
                if (!items.contains(QDir::toNativeSeparators(dir)) && !items.contains(dir))
                    items << QDir::toNativeSeparators(dir);
                m_updating = true;
                edit->setText(listText(items));
                m_updating = false;
                edit->setProperty("fwLast", listText(items));
                emit edited(items);
            });
        }
        m_widget = wrap;
        m_control = edit;
        wrap->setFocusProxy(edit);
        break;
    }
    }
}

QVariant SettingEditor::value() const
{
    switch (m_def.type) {
    case SettingType::Bool: return static_cast<Switch*>(m_control)->isChecked();
    case SettingType::Enum:
        if (auto* seg = qobject_cast<Segmented*>(m_control))
            return seg->currentValue();
        return static_cast<QComboBox*>(m_control)->currentData();
    case SettingType::Int:
    case SettingType::Double: return numberText(static_cast<QLineEdit*>(m_control)->text());
    case SettingType::String: return static_cast<QLineEdit*>(m_control)->text().trimmed();
    case SettingType::DirPath:
    case SettingType::FilePath: return static_cast<PathField*>(m_widget)->path();
    case SettingType::StringList: {
        QStringList out;
        for (const QString& s : static_cast<QLineEdit*>(m_control)->text().split(u';', Qt::SkipEmptyParts))
            if (!s.trimmed().isEmpty())
                out << s.trimmed();
        return out;
    }
    }
    return {};
}

void SettingEditor::setValue(const QVariant& v)
{
    m_updating = true;
    switch (m_def.type) {
    case SettingType::Bool: static_cast<Switch*>(m_control)->setChecked(v.toBool()); break;
    case SettingType::Enum:
        if (auto* seg = qobject_cast<Segmented*>(m_control))
            seg->setCurrentValue(v.toString());
        else {
            auto* combo = static_cast<QComboBox*>(m_control);
            const int i = combo->findData(v.toString());
            combo->setCurrentIndex(std::max(0, i));
        }
        break;
    case SettingType::Int:
    case SettingType::Double:
    case SettingType::String: {
        auto* e = static_cast<QLineEdit*>(m_control);
        e->setText(v.toString());
        if (!e->hasFocus())
            e->setCursorPosition(0); // a long text shows its start (and "…" where it is cut), not its tail
        e->setProperty("fwLast", v.toString());
        break;
    }
    case SettingType::DirPath:
    case SettingType::FilePath: static_cast<PathField*>(m_widget)->setPath(v.toString()); break;
    case SettingType::StringList: {
        QStringList items = v.toStringList();
        if (isFolderList(m_def))
            for (QString& s : items)
                s = QDir::toNativeSeparators(s);
        auto* e = static_cast<QLineEdit*>(m_control);
        e->setText(listText(items));
        if (!e->hasFocus())
            e->setCursorPosition(0); // a long list shows its start, not its tail
        e->setProperty("fwLast", listText(value().toStringList()));
        break;
    }
    }
    m_updating = false;
}

void SettingEditor::setEnabled(bool enabled) { m_widget->setEnabled(enabled); }

void SettingEditor::setError(bool error)
{
    if (auto* e = qobject_cast<QLineEdit*>(m_control))
        setInputError(e, error);
}

// ---- row ---------------------------------------------------------------------

QString SettingRow::displayValue(const SettingDef& def, const QVariant& value)
{
    switch (def.type) {
    case SettingType::Bool: return value.toBool() ? FW_TR("On") : FW_TR("Off");
    case SettingType::Enum:
        for (const auto& c : def.choices)
            if (c.value == value.toString())
                return shownChoiceLabel(def, c);
        return value.toString();
    case SettingType::Int:
    case SettingType::Double:
        if (const QString zero = SettingEditor::zeroMeaning(def); !zero.isEmpty() && value.toDouble() == 0.0)
            return zero; // "Default is Automatic."
        return def.unit.isEmpty() ? value.toString() : QStringLiteral("%1 %2").arg(value.toString(), i18n::translate(def.unit));
    case SettingType::DirPath:
    case SettingType::FilePath:
        return value.toString().isEmpty() ? FW_TR("Default") : QDir::toNativeSeparators(value.toString());
    case SettingType::StringList: {
        const auto l = value.toStringList();
        return l.isEmpty() ? FW_TR("None") : l.join(QStringLiteral("; "));
    }
    case SettingType::String: return value.toString().isEmpty() ? FW_TR("(empty)") : value.toString();
    }
    return value.toString();
}

// The first sentence of a description, so rows stay one or two lines; the
// rest is in the tooltip. Runs on the shown (translated) text: the full stops
// of Chinese and Japanese end a sentence without a space; the danda of Hindi,
// Bengali, Marathi and Nepali (। ॥), the Arabic question mark (؟), the Urdu
// full stop (۔) and the Armenian one (։) end one like ". ".
QString SettingRow::conciseHelp(const QString& text)
{
    const QString t = text.simplified();
    constexpr int kLong = 140; // a first sentence up to this is shown whole
    auto endsWithSpace = [](QChar c) {
        return c == u'.' || c == u'!' || c == u'?' || c == QChar(0x0964) || c == QChar(0x0965) || c == QChar(0x061F)
            || c == QChar(0x06D4) || c == QChar(0x0589);
    };
    int end = -1;
    for (int i = 0; i < t.size(); ++i) {
        const QChar c = t.at(i);
        if (c == QChar(0x3002) || c == QChar(0xFF01) || c == QChar(0xFF1F)) { // 。！？
            end = i + 1;
            break;
        }
        if (!endsWithSpace(c))
            continue;
        if (i + 1 < t.size() && t.at(i + 1) != u' ')
            continue; // 1.5, .bin, e.g.x
        const QString before = t.left(i + 1).right(5).toLower();
        if (before.endsWith(QStringLiteral("e.g.")) || before.endsWith(QStringLiteral("i.e.")) || before.endsWith(QStringLiteral("etc."))
            || before.endsWith(QStringLiteral("vs.")))
            continue;
        end = i + 1;
        break;
    }
    QString first = end > 0 ? t.left(end) : t;
    if (first.size() <= kLong)
        return first;
    // A long first sentence: up to the last clause break before the limit
    // ("; ", ": ", ", " and their CJK and Arabic forms), outside brackets, so
    // the row ends on a whole clause; else the whole sentence, which wraps,
    // unless it is very long. Never in the middle of a word.
    auto isClauseBreak = [](QChar c) {
        return c == u';' || c == u':' || c == u',' || c == QChar(0x3001) || c == QChar(0xFF0C) || c == QChar(0xFF1B)
            || c == QChar(0xFF1A) || c == QChar(0x060C) || c == QChar(0x061B);
    };
    int depth = 0, cut = -1, space = -1;
    for (int i = 0; i < std::min<int>(int(first.size()), kLong); ++i) {
        const QChar c = first.at(i);
        if (c == u'(' || c == u'[' || c == QChar(0xFF08)) {
            if (depth == 0 && i >= kLong / 3)
                space = i; // before the bracket, not inside it
            ++depth;
        } else if ((c == u')' || c == u']' || c == QChar(0xFF09)) && depth > 0) {
            --depth;
        } else if (depth == 0 && i >= kLong / 3) {
            if (isClauseBreak(c))
                cut = i;
            else if (c == u' ')
                space = i;
        }
    }
    if (cut > 0)
        return first.left(cut).trimmed() + QChar(0x2026);
    if (first.size() <= 2 * kLong)
        return first;
    return first.left(space > 0 ? space : kLong).trimmed() + QChar(0x2026);
}

SettingRow::SettingRow(const SettingDef& def, QWidget* parent) : QWidget(parent), m_def(def)
{
    setProperty("fwNoRing", true);
    auto* h = hbox(this, space::lg, QMargins(0, 6, 0, 6));
    m_layout = h;
    h->setAlignment(Qt::AlignVCenter);
    auto* left = new QWidget(this);
    auto* col = vbox(left, 2);
    auto* labelRow = new QHBoxLayout;
    labelRow->setSpacing(space::sm);
    m_label = label(shownLabel(def), TextStyle::Body, color::fg_1, left);
    // One line: a wrapping label gets a narrow width hint in a row and folds
    // even short labels; labels are short by design.
    m_label->setWordWrap(false);
    m_label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    labelRow->addWidget(m_label, 0);
    m_info = new QLabel(left);
    m_info->setPixmap(iconPixmap(QStringLiteral("info"), 14, color::fg_5, devicePixelRatioF()));
    m_info->setFixedSize(16, 16);
    m_info->setCursor(Qt::WhatsThisCursor);
    m_info->hide();
    labelRow->addWidget(m_info, 0, Qt::AlignVCenter);
    m_badge = new Badge(FW_TR("Overridden for this job"), Tone::Neutral, left);
    m_badge->setIconName(QStringLiteral("advanced"));
    m_badge->hide();
    labelRow->addWidget(m_badge);
    labelRow->addStretch(1);
    col->addLayout(labelRow);
    auto* helpRow = new QHBoxLayout;
    helpRow->setSpacing(4);
    m_help = caption({}, left);
    m_help->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_useDefault = new Link(FW_TR("Use default"), left);
    m_useDefault->setFont(font::caption());
    m_useDefault->hide();
    connect(m_useDefault, &Link::activated, this, &SettingRow::useDefaultRequested);
    helpRow->addWidget(m_help, 1);
    helpRow->addWidget(m_useDefault, 0, Qt::AlignTop);
    helpRow->addStretch(0);
    col->addLayout(helpRow);
    left->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    h->addWidget(left, 1);
    auto* right = new QWidget(this);
    m_right = right;
    auto* rcol = vbox(right, space::xs);
    rcol->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_editor = new SettingEditor(def, right);
    rcol->addWidget(m_editor->widget(), 0, Qt::AlignRight);
    m_message = new MessageRow(right);
    rcol->addWidget(m_message);
    h->addWidget(right, 0, Qt::AlignRight | Qt::AlignVCenter);
    m_editor->widget()->setAccessibleName(shownLabel(def));
    // The full description is otherwise only in a mouse tooltip.
    m_editor->widget()->setAccessibleDescription(shownDescription(def));
    updateHelp();
}

void SettingRow::updateHelp()
{
    const QString description = shownDescription(m_def);
    QStringList parts;
    if (!m_disabledReason.isEmpty())
        parts << m_disabledReason;
    else if (m_overridden && !m_defaultText.isEmpty())
        parts << FW_TR("Default is %1.").arg(m_defaultText);
    else if (!description.isEmpty())
        parts << conciseHelp(description);
    if (m_restart)
        parts << FW_TR("Takes effect after a restart.");
    m_help->setText(parts.join(u' '));
    m_help->setVisible(!parts.isEmpty());
    // The full description is one hover away; the row itself stays short.
    const bool shortened = !description.isEmpty() && conciseHelp(description) != description;
    const QString tip = description.isEmpty() ? QString()
                                              : QStringLiteral("<p style='white-space:normal'>%1</p>").arg(description.toHtmlEscaped());
    m_info->setVisible(shortened);
    m_info->setToolTip(tip);
    m_label->setToolTip(tip);
    m_help->setToolTip(shortened ? tip : QString());
    m_useDefault->setVisible(m_overridden);
    const bool disabled = !m_disabledReason.isEmpty();
    setTextColor(m_label, disabled ? color::fg_5 : color::fg_1);
    setTextColor(m_help, disabled ? color::fg_5 : color::fg_3);
}

void SettingRow::setMessage(const QString& text, Tone tone)
{
    m_message->set(text, tone);
    m_editor->setError(!text.isEmpty() && tone == Tone::Err);
}

QString SettingRow::messageText() const { return m_message->isVisible() || m_message->isVisibleTo(parentWidget()) ? m_message->text() : QString(); }

void SettingRow::setDisabledReason(const QString& reason)
{
    m_disabledReason = reason;
    m_editor->setEnabled(reason.isEmpty());
    updateHelp();
}

void SettingRow::setOverridden(bool on, const QString& defaultText)
{
    m_overridden = on;
    m_defaultText = defaultText;
    m_badge->setVisible(on);
    updateHelp();
}

void SettingRow::setDivider(bool on)
{
    m_divider = on;
    update();
}

void SettingRow::setRestartNote(bool on)
{
    m_restart = on;
    updateHelp();
}

void SettingRow::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    // The children already have their final widths here. A caption that only
    // just wraps can be planned as one line by the layouts above (the page
    // height is worked out before the scroll bar takes its width), which cuts
    // both lines: ask for the missing height.
    // Asked a few times per width at most: a row that cannot be given the
    // height is left alone, and this can never become a resize loop.
    if (m_helpFitWidth != width()) {
        if (m_helpFitWidth != -1)
            setMinimumHeight(0); // another width: let the layouts decide again
        m_helpFitWidth = width();
        m_helpFitTries = 0;
    }
    if (m_helpFitTries < 3 && isVisible() && m_help && m_help->isVisibleTo(this) && m_help->width() > 0) {
        const int missing = m_help->heightForWidth(m_help->width()) - m_help->height();
        if (missing > 0 && missing < 200) {
            ++m_helpFitTries;
            setMinimumHeight(height() + missing);
        }
    }
    // Narrow containers (the 400 px aside): label above the control.
    const bool narrow = width() < 520;
    if (narrow == m_narrow || !m_layout)
        return;
    m_narrow = narrow;
    m_layout->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    m_layout->setSpacing(narrow ? space::sm : space::lg);
    m_layout->setAlignment(m_right, narrow ? Qt::AlignLeft : (Qt::AlignRight | Qt::AlignVCenter));
    if (auto* rl = qobject_cast<QBoxLayout*>(m_right->layout()))
        rl->setAlignment(narrow ? Qt::AlignLeft : (Qt::AlignRight | Qt::AlignVCenter));
}

void SettingRow::paintEvent(QPaintEvent*)
{
    if (!m_divider)
        return;
    QPainter p(this);
    p.fillRect(QRect(0, height() - 1, width(), 1), color::border_1);
}

// ---- job options panel ---------------------------------------------------------

JobOptionsPanel::JobOptionsPanel(AppContext& ctx, const QStringList& sections, const QStringList& excludeKeys,
                                 QWidget* parent)
    : QWidget(parent), m_ctx(ctx), m_sections(sections), m_exclude(excludeKeys)
{
    setProperty("fwNoRing", true);
    auto* l = vbox(this, space::lg);
    Q_UNUSED(l);
}

void JobOptionsPanel::build()
{
    if (m_built)
        return;
    m_built = true;
    auto* l = static_cast<QVBoxLayout*>(layout());
    const auto& schema = m_ctx.schema();
    // Group by (section, group) in schema order.
    struct Group {
        QString title; // English: the key
        QString shown;
        std::vector<const SettingDef*> defs;
    };
    std::vector<Group> groups;
    for (const QString& section : m_sections) {
        for (const SettingDef* d : schema.inSection(section)) {
            if (!d->perJob || m_exclude.contains(d->key))
                continue;
            if (!m_include.isEmpty() && std::none_of(m_include.cbegin(), m_include.cend(), [d](const QString& k) {
                    return d->key == k || (k.endsWith(u'.') && d->key.startsWith(k));
                }))
                continue;
            const QString title = m_sections.size() > 1 && !d->group.isEmpty() && d->group != section
                                      ? QStringLiteral("%1 · %2").arg(section, d->group)
                                      : (d->group.isEmpty() ? section : d->group);
            const QString shown = m_sections.size() > 1 && !d->group.isEmpty() && d->group != section
                                      ? QStringLiteral("%1 · %2").arg(shownSection(section), shownSection(d->group))
                                      : shownSection(d->group.isEmpty() ? section : d->group);
            auto it = std::find_if(groups.begin(), groups.end(), [&](const Group& g) { return g.title == title; });
            if (it == groups.end()) {
                groups.push_back({title, shown, {}});
                it = groups.end() - 1;
            }
            it->defs.push_back(d);
        }
    }
    if (groups.empty()) {
        auto* c = caption(FW_TR("No per-job options are registered for this workflow in this build."), this);
        l->addWidget(c);
        return;
    }
    for (const Group& g : groups) {
        QVBoxLayout* target = l;
        Card* card = nullptr;
        if (m_cards) {
            card = new Card(this);
            card->body()->setSpacing(0);
            card->setTitle(g.shown);
            target = card->body();
            l->addWidget(card);
        }
        for (size_t i = 0; i < g.defs.size(); ++i) {
            const SettingDef* d = g.defs[i];
            auto* row = new SettingRow(*d, card ? static_cast<QWidget*>(card) : this);
            row->setDivider(i + 1 < g.defs.size());
            row->editor()->setValue(m_ctx.settings().value(d->key));
            m_rows.insert(d->key, row);
            m_order << d->key;
            const QString key = d->key;
            connect(row->editor(), &SettingEditor::edited, this, [this, key](const QVariant& v) { onEdited(key, v); });
            connect(row, &SettingRow::useDefaultRequested, this, [this, key] {
                m_overrides.remove(key);
                clearError(key);
                refreshRow(key);
                refreshDependencies();
                emit overridesChanged();
            });
            target->addWidget(row);
        }
    }
    // Follow global defaults for rows that are not overridden.
    connect(&m_ctx.settings(), &SettingsStore::changed, this, [this](const QString& key, const QVariant&) {
        if (m_rows.contains(key) && !m_overrides.contains(key))
            refreshRow(key);
        refreshDependencies();
    });
    connect(&m_ctx.settings(), &SettingsStore::resetDone, this, [this] {
        for (const QString& k : m_order)
            if (!m_overrides.contains(k))
                refreshRow(k);
        refreshDependencies();
    });
    refreshDependencies();
}

QVariant JobOptionsPanel::effectiveValue(const QString& key) const
{
    return m_overrides.contains(key) ? m_overrides.value(key) : m_ctx.settings().value(key);
}

void JobOptionsPanel::onEdited(const QString& key, const QVariant& value)
{
    const SettingDef* d = m_ctx.schema().find(key);
    if (!d)
        return;
    SettingRow* row = m_rows.value(key);
    auto norm = m_ctx.schema().normalize(key, value);
    if (!norm.ok()) {
        m_errors.insert(key, norm.error().message);
        row->setMessage(shownValidationReason(norm.error().message), Tone::Err);
        emit overridesChanged();
        return;
    }
    clearError(key);
    const QVariant def = m_ctx.settings().value(key);
    if (norm.value() == def) {
        m_overrides.remove(key);
    } else {
        m_overrides.insert(key, norm.value());
    }
    refreshRow(key);
    refreshDependencies();
    emit overridesChanged();
}

void JobOptionsPanel::setOverride(const QString& key, const QVariant& value)
{
    if (!m_rows.contains(key))
        return;
    auto norm = m_ctx.schema().normalize(key, value);
    if (!norm.ok())
        return;
    if (norm.value() == m_ctx.settings().value(key))
        m_overrides.remove(key);
    else
        m_overrides.insert(key, norm.value());
    clearError(key); // the value is valid now, whatever was typed before
    refreshRow(key);
    refreshDependencies();
    emit overridesChanged();
}

// Forgets the validation error of a row and takes its message away. A note a
// page put under the row (SettingRow::setMessage) is left alone: only a row
// with an error of this panel is cleared.
void JobOptionsPanel::clearError(const QString& key)
{
    if (m_errors.remove(key) == 0)
        return;
    if (SettingRow* row = m_rows.value(key))
        row->setMessage({}, Tone::Neutral);
}

void JobOptionsPanel::clearOverrides()
{
    m_overrides.clear();
    for (const QString& k : m_order) {
        clearError(k);
        refreshRow(k);
    }
    refreshDependencies();
    emit overridesChanged();
}

void JobOptionsPanel::refreshRow(const QString& key)
{
    SettingRow* row = m_rows.value(key);
    if (!row)
        return;
    const SettingDef* d = m_ctx.schema().find(key);
    const QVariant def = m_ctx.settings().value(key);
    if (m_overrides.contains(key)) {
        row->editor()->setValue(m_overrides.value(key));
        row->setOverridden(true, SettingRow::displayValue(*d, def));
    } else {
        // Back to the global value: what was typed (and its error, if it had
        // one) is gone from the field, so it must not block the form either.
        row->editor()->setValue(def);
        row->setOverridden(false);
        clearError(key);
    }
}

void JobOptionsPanel::refreshDependencies()
{
    for (const QString& key : m_order) {
        const SettingDef* d = m_ctx.schema().find(key);
        SettingRow* row = m_rows.value(key);
        if (!d || !row || d->enabledWhenKey.isEmpty())
            continue;
        const QVariant actual = effectiveValue(d->enabledWhenKey);
        const bool enabled = actual == d->enabledWhenValue || actual.toString() == d->enabledWhenValue.toString();
        if (enabled) {
            row->setDisabledReason({});
        } else {
            // A disabled row cannot be corrected: drop an invalid entry with its error.
            if (m_errors.contains(key)) {
                clearError(key);
                refreshRow(key);
            }
            const SettingDef* other = m_ctx.schema().find(d->enabledWhenKey);
            const QString otherLabel = other ? shownLabel(*other) : d->enabledWhenKey;
            const QString want = other ? inSentence(SettingRow::displayValue(*other, d->enabledWhenValue)) : d->enabledWhenValue.toString();
            row->setDisabledReason(FW_TR("Only available when \"%1\" is %2.").arg(otherLabel, want));
        }
    }
}

} // namespace fw::ui
