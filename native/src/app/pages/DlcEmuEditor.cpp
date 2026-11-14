#include "app/pages/DlcEmuEditor.h"

#include "app/pages/Page.h"
#include "app/ui/Sheet.h"
#include "core/FileUtil.h"
#include "core/Tr.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QPointer>
#include <QtConcurrent/QtConcurrentRun>

// Each table row carries its whole entry (Qt::UserRole of its first cell), so
// the keys that have no column travel with the row whatever is added, removed
// or saved around it.
Q_DECLARE_METATYPE(fw::tools::dlcemu::Entry)

namespace fw::pages {

using namespace ui;
using namespace theme;
namespace dlc = tools::dlcemu;

namespace {
const QStringList kColumns = {QStringLiteral("content_id"), QStringLiteral("label"), QStringLiteral("mount_point"),
                              QStringLiteral("download_status"), QStringLiteral("entitlement_key")};
enum Col { SectionCol = 0, ContentIdCol, LabelCol, MountCol, StatusCol, KeyCol, ColumnCount };

// The finder's confidence ("high", "medium", "low": data, compared as such) as shown.
QString confidenceLabel(const QString& c)
{
    if (c == QLatin1String("high"))
        return FW_TR("high");
    if (c == QLatin1String("medium"))
        return FW_TR("medium");
    if (c == QLatin1String("low"))
        return FW_TR("low");
    return c;
}
} // namespace

DlcEmuEditor::DlcEmuEditor(QWidget* parent) : QWidget(parent)
{
    setProperty("fwNoRing", true);
    auto* l = vbox(this, space::lg);
    auto* pick = new Card(this);
    m_badge = new Badge(FW_TR("No folder"), Tone::Neutral, pick);
    m_badge->setIconName({});
    pick->setTitle(FW_TR("Game folder"), m_badge);
    m_folder = new PathField(PathField::Mode::Directory, pick);
    m_folder->setDialogTitle(FW_TR("Choose the game folder (app0)"));
    m_folder->setPlaceholder(FW_TR("The game's app0 folder, where dlc_emu.ini lives"));
    m_folder->edit()->setAccessibleName(FW_TR("Game folder"));
    pick->body()->addWidget(m_folder);
    auto* help = caption(FW_TR("dlc_emu.ini lists the add-ons the DLC emulator presents to the game."), pick);
    help->setToolTip(FW_TR("One entry per add-on. The file lives in the game's app0 folder and may hold at most 1,024 entries and 32 KiB."));
    pick->body()->addWidget(help);
    l->addWidget(pick);
    // Queued: the question about unsaved changes must not open inside the
    // field's own edit, browse or drop handling.
    connect(m_folder, &PathField::pathChanged, this, [this](const QString&) { folderChanged(); }, Qt::QueuedConnection);

    auto* card = new Card(this, Card::Variant::Table);
    auto* h = card->header();
    h->addWidget(heading(FW_TR("Entries"), TextStyle::Subtitle, card));
    h->addStretch(1);
    // An action that looks through the dump, not a search box: no magnifier.
    m_find = button(FW_TR("Find entries…"), ButtonVariant::Secondary, QStringLiteral("wand"), card);
    m_find->setToolTip(FW_TR("Look through the dump for add-on content IDs"));
    m_add = button(FW_TR("Add entry"), ButtonVariant::Secondary, QStringLiteral("plus"), card);
    auto* dup = new IconButton(QStringLiteral("copy"), FW_TR("Duplicate"), IconButton::Size::Regular, card);
    dup->setToolTip(FW_TR("Duplicate the selected entry"));
    auto* rem = new IconButton(QStringLiteral("trash"), FW_TR("Remove"), IconButton::Size::Regular, card);
    rem->setToolTip(FW_TR("Remove the selected entry"));
    for (IconButton* b : {dup, rem})
        outlineIconButton(b); // in one row with the labelled buttons: the same plates
    m_duplicate = dup;
    m_remove = rem;
    m_save = button(FW_TR("Save…"), ButtonVariant::Primary, QStringLiteral("save"), card);
    for (QWidget* b : std::initializer_list<QWidget*>{m_find, m_add, m_duplicate, m_remove, m_save})
        h->addWidget(b);
    m_table = new QTableWidget(0, ColumnCount, card);
    m_table->setAccessibleName(FW_TR("dlc_emu.ini entries"));
    m_table->setHorizontalHeaderLabels({FW_TR("Section"), QStringLiteral("content_id"), QStringLiteral("label"), QStringLiteral("mount_point"), QStringLiteral("download_status"), QStringLiteral("entitlement_key")});
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
    m_empty = new EmptyState(QStringLiteral("database"), FW_TR("No dlc_emu.ini yet"),
                             FW_TR("Choose the game folder, then add or find entries."), card);
    // The first step as a button, while there is no folder yet.
    m_emptyChoose = m_empty->addAction(FW_TR("Choose the game folder…"));
    connect(m_emptyChoose, &QPushButton::clicked, this, [this] { m_folder->browseButton()->click(); });
    card->body()->addWidget(m_empty);
    auto* f = card->footer();
    m_validation = new MessageRow(card);
    f->addWidget(m_validation, 1);
    m_bytes = label({}, TextStyle::MonoSmall, color::fg_3, card);
    f->addWidget(m_bytes);
    l->addWidget(card);
    l->addStretch(1);

    connect(m_add, &QPushButton::clicked, this, &DlcEmuEditor::addEntry);
    connect(m_duplicate, &QAbstractButton::clicked, this, &DlcEmuEditor::duplicateRow);
    connect(m_remove, &QAbstractButton::clicked, this, &DlcEmuEditor::removeRow);
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

void DlcEmuEditor::folderChanged()
{
    const QString app0 = m_folder->path();
    if (app0 == m_shownFolder) {
        m_folder->setError({}); // Return in the field, or the same folder again: the table is current
        return;
    }
    if (m_asking)
        return; // the field reported again under the question (it lost the focus to it)
    if (m_dirty) {
        QPointer<DlcEmuEditor> self(this);
        m_asking = true;
        const bool discard = Sheet::confirm(window(), FW_TR("Discard the unsaved changes?"),
                                            FW_TR("The changes to dlc_emu.ini in %1 have not been saved.")
                                                .arg(Sheet::monoSpan(nativePath(m_shownFolder))),
                                            FW_TR("Keep editing"), FW_TR("Discard changes"), true, Sheet::Icon::Warning);
        if (!self)
            return;
        m_asking = false;
        if (!discard) {
            m_folder->setPath(m_shownFolder);
            return;
        }
    }
    load(); // whatever folder the field names by now
}

void DlcEmuEditor::load()
{
    const QString app0 = m_folder->path();
    if (app0.isEmpty()) {
        m_shownFolder.clear();
        m_dirty = false;
        setEntries({});
        m_badge->set(FW_TR("No folder"), Tone::Neutral);
        return;
    }
    if (!QFileInfo(app0).isDir()) {
        m_folder->setError(FW_TR("The folder does not exist."));
        return;
    }
    m_folder->setError({});
    QPointer<DlcEmuEditor> self(this);
    (void)QtConcurrent::run([self, app0] {
        auto r = dlc::load(app0);
        QMetaObject::invokeMethod(qApp, [self, r, app0] {
            if (!self || self->m_folder->path() != app0)
                return;
            self->m_shownFolder = app0;
            self->m_dirty = false;
            if (!r.ok()) {
                self->m_folder->setError(r.error().message);
                self->setEntries({});
                return;
            }
            self->setEntries(r.value());
            const bool exists = QFile::exists(QDir(app0).filePath(QStringLiteral("dlc_emu.ini")));
            self->m_badge->set(exists ? (r->size() == 1 ? FW_TR("%1 entry loaded") : FW_TR("%1 entries loaded")).arg(r->size()) : FW_TR("New file"), exists ? Tone::Ok : Tone::Info);
        }, Qt::QueuedConnection);
    });
}

void DlcEmuEditor::setEntries(const std::vector<dlc::Entry>& entries)
{
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
    // A type the file has but the list does not: kept as it is, so it is not
    // silently turned into another one and the validation can name it.
    if (!e.section.isEmpty() && section->findText(e.section) < 0) {
        section->addItem(e.section);
        section->setItemData(section->count() - 1, FW_TR("Not a known section type"), Qt::ToolTipRole);
    }
    section->setCurrentText(e.section);
    section->setProperty("fwNoRing", false);
    section->setAccessibleName(FW_TR("Section type"));
    connect(section, &QComboBox::currentIndexChanged, this, [this](int) { m_dirty = true; revalidate(); });
    m_table->setCellWidget(r, SectionCol, section);
    auto* anchor = new QTableWidgetItem;
    anchor->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    anchor->setData(Qt::UserRole, QVariant::fromValue(e)); // the entry itself: keys without a column, and their order
    m_table->setItem(r, SectionCol, anchor);
    for (int c = ContentIdCol; c < ColumnCount; ++c) {
        const QString key = kColumns[c - ContentIdCol];
        if (c == StatusCol) {
            auto* status = new QComboBox(m_table);
            status->setAccessibleName(QStringLiteral("download_status"));
            status->addItem(QString());
            status->addItems(dlc::downloadStatuses());
            if (!e.get(key).isEmpty() && status->findText(e.get(key)) < 0) {
                status->addItem(e.get(key));
                status->setItemData(status->count() - 1, FW_TR("Not a known download status"), Qt::ToolTipRole);
            }
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
}

std::vector<dlc::Entry> DlcEmuEditor::entries() const
{
    std::vector<dlc::Entry> out;
    for (int r = 0; r < m_table->rowCount(); ++r) {
        dlc::Entry e;
        if (const QTableWidgetItem* anchor = m_table->item(r, SectionCol))
            e = anchor->data(Qt::UserRole).value<dlc::Entry>(); // keeps extra keys and their order
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
    addRow(all[size_t(r)]);
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
    m_emptyChoose->setVisible(m_folder->path().isEmpty());
    const dlc::Validation v = dlc::validate(all, m_folder->path());
    const int bytes = dlc::byteSize(all);
    // "0 of 32 KiB" for an empty list, not "0.0 of 32 KiB"; "1.4 of 32 KiB" once there is something.
    const QString used = bytes == 0 ? QStringLiteral("0") : QString::number(bytes / 1024.0, 'f', 1);
    m_bytes->setText((all.size() == 1 ? FW_TR("%1 entry · %2 of %3 KiB") : FW_TR("%1 entries · %2 of %3 KiB")).arg(all.size()).arg(used).arg(dlc::kMaxIniBytes / 1024));
    setTextColor(m_bytes, bytes > dlc::kMaxIniBytes || int(all.size()) > dlc::kMaxEntries ? color::danger_hi : color::fg_3);
    if (!v.errors.isEmpty())
        m_validation->set((v.errors.size() == 1 ? FW_TR("%1 error: %2") : FW_TR("%1 errors: %2")).arg(v.errors.size()).arg(v.errors.first()), Tone::Err);
    else if (!v.warnings.isEmpty())
        m_validation->set((v.warnings.size() == 1 ? FW_TR("%1 warning: %2") : FW_TR("%1 warnings: %2")).arg(v.warnings.size()).arg(v.warnings.first()), Tone::Warn);
    else if (anyRows)
        m_validation->set(m_dirty ? FW_TR("Valid · unsaved changes") : FW_TR("Valid"), Tone::Ok);
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
        QMetaObject::invokeMethod(qApp, [self, s, app0] {
            if (!self)
                return;
            self->m_find->setEnabled(!self->m_folder->path().isEmpty());
            if (self->m_folder->path() != app0)
                return; // another game was opened meanwhile: these IDs are not its IDs
            if (s.entries.empty()) {
                Sheet::message(self->window(), FW_TR("No add-on entries found"),
                               s.notes.isEmpty() ? FW_TR("Nothing in the dump looks like an add-on content ID.") : s.notes.join(u' '), Sheet::Icon::Info);
                return;
            }
            auto* sheet = new Sheet(self->window(), (s.entries.size() == 1 ? FW_TR("Found %1 possible entry") : FW_TR("Found %1 possible entries")).arg(s.entries.size()), Sheet::Icon::None, 900);
            QStringList intro;
            intro << (s.prefixIsPlaceholder ? FW_TR("Prefix %1 (placeholder: the dump's own content ID was not found; check every ID).") : FW_TR("Prefix %1.")).arg(Sheet::monoSpan(s.prefix));
            if (!s.addcont.isEmpty())
                intro << FW_TR("Add-on folders: %1.").arg(Sheet::monoSpan(s.addcont.join(QStringLiteral(", "))));
            for (const QString& n : s.notes)
                intro << n.toHtmlEscaped();
            sheet->addText(intro.join(u' '));
            auto* table = new QTableWidget(int(s.entries.size()), 4, sheet);
            table->setHorizontalHeaderLabels({FW_TR("Add"), FW_TR("Confidence"), QStringLiteral("content_id"), FW_TR("Found in")});
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
                auto* badge = new Badge(e.placeholder ? FW_TR("placeholder") : confidenceLabel(e.confidence), tone, wrap);
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
            sheet->addButton(FW_TR("Cancel"), Sheet::Role::Safe);
            sheet->addButton(FW_TR("Add ticked entries"), Sheet::Role::Primary);
            // The sheet and its table are alive until control returns to the
            // event loop; the editor may have gone, or moved to another game.
            const int choice = sheet->exec();
            if (choice != 1 || !self || self->m_folder->path() != app0)
                return;
            self->m_table->blockSignals(true);
            int added = 0;
            for (int r = 0; r < table->rowCount(); ++r)
                if (table->item(r, 0)->checkState() == Qt::Checked) {
                    self->addRow(s.entries[size_t(r)]);
                    ++added;
                }
            self->m_table->blockSignals(false);
            if (added == 0)
                return;
            self->m_dirty = true;
            self->revalidate();
            emit self->toastRequested(Tone::Ok, (added == 1 ? FW_TR("%1 entry added") : FW_TR("%1 entries added")).arg(added), FW_TR("Review the IDs, then save."));
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
    if (!Sheet::confirm(window(), FW_TR("Write dlc_emu.ini?"),
                        (all.size() == 1 ? FW_TR("%1 entry is written to %2.") : FW_TR("%1 entries are written to %2.")).arg(all.size()).arg(Sheet::monoSpan(nativePath(file)))
                            + (exists ? QStringLiteral(" ") + FW_TR("The current file is kept as dlc_emu.ini.bak.") : QString()),
                        FW_TR("Cancel"), FW_TR("Save"), false, Sheet::Icon::Info))
        return;
    m_save->setEnabled(false);
    QPointer<DlcEmuEditor> self(this);
    (void)QtConcurrent::run([self, app0, all] {
        auto st = dlc::save(app0, all);
        QMetaObject::invokeMethod(qApp, [self, st, all, app0] {
            if (!self)
                return;
            self->m_save->setEnabled(true);
            if (!st.ok()) {
                emit self->toastRequested(Tone::Err, FW_TR("dlc_emu.ini not saved"), st.error().message);
                return;
            }
            if (self->m_folder->path() != app0)
                return; // saved, but the editor shows another game by now
            self->m_dirty = false;
            self->revalidate();
            self->m_badge->set((all.size() == 1 ? FW_TR("%1 entry saved") : FW_TR("%1 entries saved")).arg(all.size()), Tone::Ok);
            emit self->toastRequested(Tone::Ok, FW_TR("dlc_emu.ini saved"), nativePath(self->m_folder->path()));
        }, Qt::QueuedConnection);
    });
}

} // namespace fw::pages
