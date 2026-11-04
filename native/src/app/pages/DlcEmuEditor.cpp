#include "app/pages/DlcEmuEditor.h"
#include "app/i18n/I18n.h"

#include "app/ui/Sheet.h"
#include "core/FileUtil.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

namespace fw::pages {

using namespace ui;
using namespace theme;
namespace dlc = tools::dlcemu;

namespace {
const QStringList kColumns = {QStringLiteral("content_id"), QStringLiteral("label"), QStringLiteral("mount_point"),
                              QStringLiteral("download_status"), QStringLiteral("entitlement_key")};
enum Col { SectionCol = 0, ContentIdCol, LabelCol, MountCol, StatusCol, KeyCol, ColumnCount };
} // namespace

DlcEmuEditor::DlcEmuEditor(QWidget* parent) : QWidget(parent)
{
    setProperty("fwNoRing", true);
    auto* l = vbox(this, space::lg);
    auto* pick = new Card(this);
    m_badge = new Badge(tr("No folder"), Tone::Neutral, pick);
    m_badge->setIconName({});
    pick->setTitle(tr("DLC emulator (dlc_emu.ini)"), m_badge);
    pick->body()->addWidget(caption(tr("One entry per add-on the emulator should present to the game. The file lives in the game's app0 folder; it may hold at most 1,024 entries and 32 KiB."), pick));
    m_folder = new PathField(PathField::Mode::Directory, pick);
    m_folder->setDialogTitle(tr("Choose the game folder (app0)"));
    pick->body()->addWidget(field(tr("Game folder (app0)"), m_folder, {}, pick));
    l->addWidget(pick);
    connect(m_folder, &PathField::pathChanged, this, [this](const QString&) { load(); });

    auto* card = new Card(this, Card::Variant::Table);
    auto* h = card->header();
    h->addWidget(heading(tr("Entries"), TextStyle::Subtitle, card));
    h->addStretch(1);
    m_add = button(tr("Add entry"), ButtonVariant::Secondary, QStringLiteral("plus"), card);
    m_duplicate = button(tr("Duplicate"), ButtonVariant::Secondary, QStringLiteral("copy"), card);
    m_remove = button(tr("Remove"), ButtonVariant::Secondary, QStringLiteral("trash"), card);
    m_find = button(tr("Find DLC entries…"), ButtonVariant::Secondary, QStringLiteral("search"), card);
    m_save = button(tr("Save…"), ButtonVariant::Primary, QStringLiteral("save"), card);
    for (auto* b : {m_add, m_duplicate, m_remove, m_find, m_save})
        h->addWidget(b);
    m_table = new QTableWidget(0, ColumnCount, card);
    m_table->setHorizontalHeaderLabels({tr("Section"), QStringLiteral("content_id"), QStringLiteral("label"), QStringLiteral("mount_point"), QStringLiteral("download_status"), QStringLiteral("entitlement_key")});
    m_table->horizontalHeader()->setFixedHeight(32);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_table->horizontalHeader()->setSectionResizeMode(ContentIdCol, QHeaderView::Stretch);
    m_table->setColumnWidth(SectionCol, 110);
    m_table->setColumnWidth(LabelCol, 140);
    m_table->setColumnWidth(MountCol, 130);
    m_table->setColumnWidth(StatusCol, 170);
    m_table->setColumnWidth(KeyCol, 280);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(size::row);
    m_table->setShowGrid(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setMinimumHeight(8 * size::row + 40);
    card->body()->addWidget(m_table);
    m_empty = new EmptyState(QStringLiteral("database"), tr("No dlc_emu.ini yet"),
                             tr("Choose the game folder, then add entries by hand or let Find DLC entries look through the dump."), card);
    card->body()->addWidget(m_empty);
    auto* f = card->footer();
    m_validation = new MessageRow(card);
    f->addWidget(m_validation, 1);
    m_bytes = label({}, TextStyle::MonoSmall, color::fg_3, card);
    f->addWidget(m_bytes);
    l->addWidget(card);
    l->addStretch(1);

    connect(m_add, &QPushButton::clicked, this, &DlcEmuEditor::addEntry);
    connect(m_duplicate, &QPushButton::clicked, this, &DlcEmuEditor::duplicateRow);
    connect(m_remove, &QPushButton::clicked, this, &DlcEmuEditor::removeRow);
    connect(m_find, &QPushButton::clicked, this, &DlcEmuEditor::findEntries);
    connect(m_save, &QPushButton::clicked, this, &DlcEmuEditor::save);
    connect(m_table, &QTableWidget::itemChanged, this, [this](QTableWidgetItem*) { m_dirty = true; revalidate(); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, &DlcEmuEditor::updateRowControls);
    setEntries({});
}

QString DlcEmuEditor::folder() const { return m_folder->path(); }

void DlcEmuEditor::openFolder(const QString& app0)
{
    m_folder->setPath(app0);
}

void DlcEmuEditor::load()
{
    const QString app0 = m_folder->path();
    if (app0.isEmpty()) {
        setEntries({});
        m_badge->set(tr("No folder"), Tone::Neutral);
        return;
    }
    if (!QFileInfo(app0).isDir()) {
        m_folder->setError(tr("The folder does not exist."));
        return;
    }
    m_folder->setError({});
    QPointer<DlcEmuEditor> self(this);
    (void)QtConcurrent::run([self, app0] {
        auto r = dlc::load(app0);
        QMetaObject::invokeMethod(self.data(), [self, r, app0] {
            if (!self || self->m_folder->path() != app0)
                return;
            if (!r.ok()) {
                self->m_folder->setError(r.error().message);
                self->setEntries({});
                return;
            }
            self->setEntries(r.value());
            self->m_dirty = false;
            const bool exists = QFile::exists(QDir(app0).filePath(QStringLiteral("dlc_emu.ini")));
            self->m_badge->set(exists ? tr("%1 entr%2 loaded").arg(r->size()).arg(r->size() == 1 ? tr("y") : tr("ies")) : tr("New file"), exists ? Tone::Ok : Tone::Info);
        }, Qt::QueuedConnection);
    });
}

void DlcEmuEditor::setEntries(const std::vector<dlc::Entry>& entries)
{
    m_loaded = entries;
    m_table->blockSignals(true);
    m_table->setRowCount(0);
    for (const auto& e : entries)
        addRow(e);
    m_table->blockSignals(false);
    revalidate();
    updateRowControls();
}

void DlcEmuEditor::addRow(const dlc::Entry& e)
{
    const int r = m_table->rowCount();
    m_table->insertRow(r);
    auto* section = new QComboBox(m_table);
    section->addItems(dlc::sectionTypes());
    section->setCurrentText(e.section);
    section->setProperty("fwNoRing", false);
    connect(section, &QComboBox::currentIndexChanged, this, [this](int) { m_dirty = true; revalidate(); });
    m_table->setCellWidget(r, SectionCol, section);
    auto* anchor = new QTableWidgetItem;
    anchor->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    anchor->setData(Qt::UserRole, r); // index into m_loaded for unknown keys
    m_table->setItem(r, SectionCol, anchor);
    for (int c = ContentIdCol; c < ColumnCount; ++c) {
        const QString key = kColumns[c - ContentIdCol];
        if (c == StatusCol) {
            auto* status = new QComboBox(m_table);
            status->addItem(QString());
            status->addItems(dlc::downloadStatuses());
            status->setCurrentText(e.get(key));
            connect(status, &QComboBox::currentIndexChanged, this, [this](int) { m_dirty = true; revalidate(); });
            m_table->setCellWidget(r, c, status);
            auto* it = new QTableWidgetItem;
            it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            m_table->setItem(r, c, it);
        } else {
            auto* it = new QTableWidgetItem(e.get(key));
            it->setFont(font::mono());
            m_table->setItem(r, c, it);
        }
    }
    if (m_loaded.size() <= size_t(r))
        m_loaded.push_back(e);
}

std::vector<dlc::Entry> DlcEmuEditor::entries() const
{
    std::vector<dlc::Entry> out;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        dlc::Entry e;
        const int src = m_table->item(r, SectionCol) ? m_table->item(r, SectionCol)->data(Qt::UserRole).toInt() : -1;
        if (src >= 0 && src < int(m_loaded.size()))
            e = m_loaded[size_t(src)]; // keeps extra keys and their order
        e.confidence.clear();
        e.source.clear();
        e.placeholder = false;
        if (auto* sec = qobject_cast<QComboBox*>(m_table->cellWidget(r, SectionCol)))
            e.section = sec->currentText();
        for (int c = ContentIdCol; c < ColumnCount; ++c) {
            const QString key = kColumns[c - ContentIdCol];
            QString value;
            if (auto* combo = qobject_cast<QComboBox*>(m_table->cellWidget(r, c)))
                value = combo->currentText();
            else if (auto* it = m_table->item(r, c))
                value = it->text().trimmed();
            e.set(key, value);
        }
        out.push_back(e);
    }
    return out;
}

void DlcEmuEditor::addEntry()
{
    dlc::Entry e;
    e.section = QStringLiteral("PSAC");
    e.set(QStringLiteral("download_status"), dlc::downloadStatuses().last());
    m_table->blockSignals(true);
    addRow(e);
    m_table->blockSignals(false);
    m_dirty = true;
    m_table->selectRow(m_table->rowCount() - 1);
    m_table->editItem(m_table->item(m_table->rowCount() - 1, ContentIdCol));
    revalidate();
}

void DlcEmuEditor::duplicateRow()
{
    const int r = m_table->currentRow();
    if (r < 0)
        return;
    const auto all = entries();
    if (r >= int(all.size()))
        return;
    m_table->blockSignals(true);
    m_loaded.push_back(all[size_t(r)]);
    addRow(all[size_t(r)]);
    m_table->item(m_table->rowCount() - 1, SectionCol)->setData(Qt::UserRole, int(m_loaded.size() - 1));
    m_table->blockSignals(false);
    m_dirty = true;
    revalidate();
}

void DlcEmuEditor::removeRow()
{
    const int r = m_table->currentRow();
    if (r < 0)
        return;
    m_table->removeRow(r);
    m_dirty = true;
    revalidate();
    updateRowControls();
}

void DlcEmuEditor::updateRowControls()
{
    const bool any = m_table->currentRow() >= 0 && m_table->rowCount() > 0;
    m_duplicate->setEnabled(any);
    m_remove->setEnabled(any);
}

void DlcEmuEditor::revalidate()
{
    const auto all = entries();
    const bool anyRows = !all.empty();
    m_table->setVisible(anyRows);
    m_empty->setVisible(!anyRows);
    const dlc::Validation v = dlc::validate(all, m_folder->path());
    const int bytes = dlc::byteSize(all);
    m_bytes->setText(tr("%1 entr%2 · %3 of %4 KiB").arg(all.size()).arg(all.size() == 1 ? tr("y") : tr("ies")).arg(QString::number(bytes / 1024.0, 'f', 1)).arg(dlc::kMaxIniBytes / 1024));
    setTextColor(m_bytes, bytes > dlc::kMaxIniBytes || int(all.size()) > dlc::kMaxEntries ? color::danger_hi : color::fg_3);
    if (!v.errors.isEmpty())
        m_validation->set(tr("%1 error%2: %3").arg(v.errors.size()).arg(v.errors.size() == 1 ? QString() : tr("s")).arg(v.errors.first()), Tone::Err);
    else if (!v.warnings.isEmpty())
        m_validation->set(tr("%1 warning%2: %3").arg(v.warnings.size()).arg(v.warnings.size() == 1 ? QString() : tr("s")).arg(v.warnings.first()), Tone::Warn);
    else if (anyRows)
        m_validation->set(tr("Valid%1").arg(m_dirty ? tr(" · unsaved changes") : QString()), Tone::Ok);
    else
        m_validation->clear();
    m_validation->setToolTip((v.errors + v.warnings).join(u'\n'));
    m_save->setEnabled(v.ok() && !m_folder->path().isEmpty() && anyRows);
    m_find->setEnabled(!m_folder->path().isEmpty());
}

void DlcEmuEditor::findEntries()
{
    const QString app0 = m_folder->path();
    if (app0.isEmpty())
        return;
    m_find->setEnabled(false);
    const auto existing = entries();
    QPointer<DlcEmuEditor> self(this);
    (void)QtConcurrent::run([self, app0, existing] {
        const dlc::Suggestions s = dlc::autoFind(app0, true, existing);
        QMetaObject::invokeMethod(self.data(), [self, s] {
            if (!self)
                return;
            self->m_find->setEnabled(true);
            if (s.entries.empty()) {
                Sheet::message(self->window(), tr("No add-on entries found"),
                               (s.notes.isEmpty() ? tr("Nothing in the dump looks like an add-on content ID.") : s.notes.join(u' ')).toHtmlEscaped(), Sheet::Icon::Info);
                return;
            }
            auto* sheet = new Sheet(self->window(), tr("Found %1 possible entr%2").arg(s.entries.size()).arg(s.entries.size() == 1 ? tr("y") : tr("ies")), Sheet::Icon::None, 900);
            QStringList intro;
            intro << tr("Prefix %1%2.").arg(Sheet::monoSpan(s.prefix), s.prefixIsPlaceholder ? tr(" (placeholder: the dump's own content ID was not found; check every ID)") : QString());
            if (!s.addcont.isEmpty())
                intro << tr("Add-on folders: %1.").arg(Sheet::monoSpan(s.addcont.join(QStringLiteral(", "))));
            for (const QString& n : s.notes)
                intro << n.toHtmlEscaped();
            sheet->addText(intro.join(u' '));
            auto* table = new QTableWidget(int(s.entries.size()), 4, sheet);
            table->setHorizontalHeaderLabels({tr("Add"), tr("Confidence"), QStringLiteral("content_id"), tr("Found in")});
            table->horizontalHeader()->setFixedHeight(32);
            table->horizontalHeader()->setHighlightSections(false);
            table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
            table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            table->setColumnWidth(0, 60);
            table->setColumnWidth(1, 130);
            table->setColumnWidth(2, 340);
            table->verticalHeader()->hide();
            table->verticalHeader()->setDefaultSectionSize(size::row);
            table->setShowGrid(false);
            table->setEditTriggers(QAbstractItemView::NoEditTriggers);
            table->setMinimumHeight(std::min(10, int(s.entries.size())) * size::row + 40);
            for (int r = 0; r < int(s.entries.size()); ++r) {
                const dlc::Entry& e = s.entries[size_t(r)];
                auto* add = new QTableWidgetItem;
                add->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable);
                add->setCheckState(e.confidence == QLatin1String("high") && !e.placeholder ? Qt::Checked : Qt::Unchecked);
                table->setItem(r, 0, add);
                auto* wrap = new QWidget(table);
                auto* wh = hbox(wrap, 0, QMargins(8, 0, 8, 0));
                const Tone tone = e.confidence == QLatin1String("high") ? Tone::Ok : e.confidence == QLatin1String("medium") ? Tone::Warn : Tone::Neutral;
                auto* badge = new Badge(e.placeholder ? QStringLiteral("placeholder") : e.confidence, tone, wrap);
                badge->setIconName({});
                wh->addWidget(badge);
                wh->addStretch(1);
                table->setCellWidget(r, 1, wrap);
                auto* id = new QTableWidgetItem(e.get(QStringLiteral("content_id")));
                id->setFont(font::mono());
                id->setForeground(e.placeholder ? color::warn_hi : color::fg_0);
                table->setItem(r, 2, id);
                auto* src = new QTableWidgetItem(e.source);
                src->setForeground(color::fg_3);
                table->setItem(r, 3, src);
            }
            sheet->body()->addWidget(table);
            sheet->addButton(tr("Cancel"), Sheet::Role::Safe);
            sheet->addButton(tr("Add ticked entries"), Sheet::Role::Primary);
            if (sheet->exec() != 1)
                return;
            self->m_table->blockSignals(true);
            int added = 0;
            for (int r = 0; r < table->rowCount(); ++r)
                if (table->item(r, 0)->checkState() == Qt::Checked) {
                    dlc::Entry e = s.entries[size_t(r)];
                    self->m_loaded.push_back(e);
                    self->addRow(e);
                    self->m_table->item(self->m_table->rowCount() - 1, SectionCol)->setData(Qt::UserRole, int(self->m_loaded.size() - 1));
                    ++added;
                }
            self->m_table->blockSignals(false);
            if (added > 0)
                self->m_dirty = true;
            self->revalidate();
            emit self->toastRequested(Tone::Ok, tr("%1 entr%2 added").arg(added).arg(added == 1 ? tr("y") : tr("ies")), tr("Review the IDs, then save."));
        }, Qt::QueuedConnection);
    });
}

void DlcEmuEditor::save()
{
    const QString app0 = m_folder->path();
    const auto all = entries();
    const dlc::Validation v = dlc::validate(all, app0);
    if (!v.ok()) {
        m_validation->set(v.errors.first(), Tone::Err);
        return;
    }
    const QString file = QDir(app0).filePath(QStringLiteral("dlc_emu.ini"));
    const bool exists = QFile::exists(file);
    if (!Sheet::confirm(window(), tr("Write dlc_emu.ini?"),
                        tr("%1 entr%2 are written to %3.%4").arg(all.size()).arg(all.size() == 1 ? tr("y") : tr("ies")).arg(Sheet::monoSpan(nativePath(file)),
                                                                                                               exists ? tr(" The current file is kept as dlc_emu.ini.bak.") : QString()),
                        tr("Cancel"), tr("Save"), false, Sheet::Icon::Info))
        return;
    m_save->setEnabled(false);
    QPointer<DlcEmuEditor> self(this);
    (void)QtConcurrent::run([self, app0, all] {
        auto st = dlc::save(app0, all);
        QMetaObject::invokeMethod(self.data(), [self, st, all] {
            if (!self)
                return;
            self->m_save->setEnabled(true);
            if (!st.ok()) {
                emit self->toastRequested(Tone::Err, tr("dlc_emu.ini not saved"), st.error().message);
                return;
            }
            self->m_dirty = false;
            self->m_loaded = all;
            self->revalidate();
            self->m_badge->set(tr("%1 entr%2 saved").arg(all.size()).arg(all.size() == 1 ? tr("y") : tr("ies")), Tone::Ok);
            emit self->toastRequested(Tone::Ok, tr("dlc_emu.ini saved"), nativePath(self->m_folder->path()));
        }, Qt::QueuedConnection);
    });
}

} // namespace fw::pages
