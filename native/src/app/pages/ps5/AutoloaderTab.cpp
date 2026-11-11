#include "app/pages/ps5/AutoloaderTab.h"

#include "app/AppContext.h"
#include "app/ui/Sheet.h"
#include "connect/ps5/Module.h"
#include "core/Tr.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QSpinBox>
#include <QTextBlock>

namespace fw::pages {

using namespace ui;
using namespace theme;
namespace al = ps5::autoloader;

namespace {

struct Loaded {
    al::Loaded file;
    QStringList payloads;
};

} // namespace

AutoloaderTab::AutoloaderTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent)
    : ConsoleToolTab(ctx, std::move(endpointFn), QStringLiteral("autoloader"), parent)
{
    auto* l = vbox(this, space::lg);
    auto* info = new InfoBar(Tone::Info, this);
    info->setMessage(FW_TR("ps5_autoloader runs the payloads listed in autoload.txt after the exploit, looking on USB drives, "
                                    "then /data, then savedata. This edits a folder on this PC: copy it to the console yourself."));
    l->addWidget(info);

    auto* pick = new Card(this);
    m_badge = new Badge(FW_TR("No folder"), Tone::Neutral, pick);
    m_badge->setIconName({});
    pick->setTitle(FW_TR("ps5_autoloader folder"), m_badge);
    auto* row = new QWidget(pick);
    auto* rh = hbox(row, space::sm);
    m_folder = new PathField(PathField::Mode::Directory, row);
    m_folder->setDialogTitle(FW_TR("Choose the ps5_autoloader folder"));
    m_folder->setPlaceholder(QStringLiteral("…/ps5_autoloader"));
    m_reload = button(FW_TR("Reload"), ButtonVariant::Secondary, QStringLiteral("refresh"), row);
    rh->addWidget(m_folder, 1);
    rh->addWidget(m_reload, 0, Qt::AlignTop);
    pick->body()->addWidget(row);
    l->addWidget(pick);

    auto* edit = new Card(this, Card::Variant::Table);
    auto* h = edit->header();
    h->addWidget(heading(QStringLiteral("autoload.txt"), TextStyle::Subtitle, edit));
    auto* hint = new ElideLabel(FW_TR("One .elf or .bin per line (or /absolute/path) · !1000 waits 1 s · # comment · @sync"), Qt::ElideRight, edit);
    hint->setFont(fontFor(TextStyle::Caption));
    setTextColor(hint, color::fg_3);
    h->addWidget(hint, 1);
    m_sleep = button(FW_TR("Insert !sleep…"), ButtonVariant::Secondary, QStringLiteral("timer"), edit);
    m_save = button(FW_TR("Save autoload.txt"), ButtonVariant::Primary, QStringLiteral("save"), edit);
    h->addWidget(m_sleep);
    h->addWidget(m_save);
    m_editor = new QPlainTextEdit(edit);
    m_editor->setFont(font::mono());
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setTabChangesFocus(true);
    m_editor->setMinimumHeight(12 * font::lineMono + 2 * space::md);
    m_editor->setAccessibleName(QStringLiteral("autoload.txt"));
    m_editor->setPlaceholderText(FW_TR("Choose the ps5_autoloader folder first."));
    edit->body()->addWidget(m_editor);
    auto* f = edit->footer();
    m_validation = new MessageRow(edit);
    f->addWidget(m_validation, 1);
    m_saved = caption({}, edit);
    f->addWidget(m_saved);
    m_counts = label({}, TextStyle::MonoSmall, color::fg_3, edit);
    f->addWidget(m_counts);
    l->addWidget(edit);

    auto* files = new Card(this);
    m_add = button(FW_TR("Add payload…"), ButtonVariant::Secondary, QStringLiteral("plus"), files);
    files->setTitle(FW_TR("Payloads in the folder"), m_add);
    files->body()->addWidget(caption(FW_TR("Click a name to add it on a new line below the cursor. Add payload copies a file into the folder."), files));
    m_chips = new QWidget(files);
    m_chipLayout = new FlowLayout(m_chips, space::sm, space::sm);
    files->body()->addWidget(m_chips);
    m_noChips = caption(FW_TR("No .elf or .bin files in the folder."), files);
    files->body()->addWidget(m_noChips);
    l->addWidget(files);
    l->addStretch(1);

    connect(m_folder, &PathField::pathChanged, this, [this](const QString& p) {
        if (m_demo)
            return;
        if (m_dirty && !Sheet::confirm(window(), FW_TR("Discard the unsaved changes?"),
                                       FW_TR("autoload.txt was edited and not saved."), FW_TR("Keep editing"),
                                       FW_TR("Discard"), true, Sheet::Icon::Warning)) {
            m_folder->blockSignals(true); // keep editing the folder the text belongs to
            m_folder->setPath(m_current);
            m_folder->blockSignals(false);
            return;
        }
        openFolder(p);
    });
    connect(m_reload, &QPushButton::clicked, this, [this] {
        if (m_dirty && !Sheet::confirm(window(), FW_TR("Discard the unsaved changes?"),
                                       FW_TR("Reloading reads autoload.txt from the folder again."), FW_TR("Keep editing"),
                                       FW_TR("Reload"), true, Sheet::Icon::Warning))
            return;
        load();
    });
    connect(m_editor, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_demo) {
            m_dirty = true;
            if (m_editor->document()->isModified())
                m_badge->set(FW_TR("Unsaved changes"), Tone::Warn); // not "Saved" any more
        }
        revalidate();
    });
    connect(m_sleep, &QPushButton::clicked, this, &AutoloaderTab::askInsertSleep);
    connect(m_save, &QPushButton::clicked, this, &AutoloaderTab::save);
    connect(m_add, &QPushButton::clicked, this, &AutoloaderTab::addPayload);
    connect(this, &ConsoleToolTab::busyChanged, this, [this](bool busy) {
        // A folder that was opened while a save or copy ran is read now.
        if (!busy && m_loadPending) {
            m_loadPending = false;
            load();
            return;
        }
        revalidate();
    });

    const QString saved = al::folderFromSettings(m_ctx.settings().snapshot());
    m_current = saved;
    m_folder->blockSignals(true);
    m_folder->setPath(saved);
    m_folder->blockSignals(false);
    setFiles({});
    revalidate();
    if (!saved.isEmpty() && QFileInfo(saved).isDir())
        load(); // legacy: the saved folder opens by itself
}

QString AutoloaderTab::folder() const
{
    return m_current;
}

void AutoloaderTab::openFolder(const QString& path)
{
    m_current = path.trimmed();
    m_folder->blockSignals(true);
    m_folder->setPath(m_current);
    m_folder->blockSignals(false);
    (void)m_ctx.settings().setValue(ps5::keys::kAutoloaderFolder, m_current);
    m_dirty = false;
    load();
}

void AutoloaderTab::load()
{
    const QString dir = folder();
    // Asked here, once per load: not on every keystroke (the folder can be on a drive that has to wake up).
    m_hasFolder = !dir.isEmpty() && QFileInfo(dir).isDir();
    if (!m_hasFolder) {
        // Nothing from another folder stays in the editor.
        m_editor->blockSignals(true);
        m_editor->clear();
        m_editor->document()->setModified(false);
        m_editor->blockSignals(false);
        m_dirty = false;
        setFiles({});
        m_folder->setError(dir.isEmpty() ? QString() : FW_TR("The folder does not exist."));
        m_badge->set(dir.isEmpty() ? FW_TR("No folder") : FW_TR("Folder missing"), dir.isEmpty() ? Tone::Neutral : Tone::Err);
        m_saved->clear();
        return;
    }
    m_folder->setError({});
    const bool started = runLocal<Loaded>(
        [dir](const CancellationToken&) -> Result<Loaded> {
            FW_TRY_ASSIGN(al::Loaded file, al::load(dir));
            return Loaded{file, al::payloadFiles(dir)};
        },
        [this, dir](const Result<Loaded>& r) {
            if (dir != folder())
                return;
            if (!r.ok()) {
                m_folder->setError(r.error().message);
                m_badge->set(FW_TR("Not loaded"), Tone::Err);
                return;
            }
            setFiles(r->payloads);
            m_editor->setPlainText(r->file.text);
            m_editor->document()->setModified(false);
            m_dirty = !r->file.existed; // the header is new: saving creates the file
            m_badge->set(r->file.existed ? FW_TR("autoload.txt loaded") : FW_TR("New autoload.txt"),
                         r->file.existed ? Tone::Ok : Tone::Info);
            m_saved->setText(r->file.existed ? QString() : FW_TR("Not saved yet: the default header was created."));
            revalidate();
        });
    if (!started) {
        // Another step is running: the editor still holds the previous folder's text. Nothing of it
        // may be saved into this one; the load runs as soon as that step ends.
        m_loadPending = true;
        m_editor->blockSignals(true);
        m_editor->clear();
        m_editor->document()->setModified(false);
        m_editor->blockSignals(false);
        m_dirty = false;
        m_badge->set(FW_TR("Loading…"), Tone::Info);
        revalidate();
    }
}

void AutoloaderTab::setFiles(const QStringList& files)
{
    m_files = files;
    while (QLayoutItem* it = m_chipLayout->takeAt(0)) {
        delete it->widget();
        delete it;
    }
    for (const QString& name : files) {
        auto* chip = new Card(m_chips, Card::Variant::Inset);
        chip->layout()->setContentsMargins(space::xxs, space::xxs, space::xxs, space::xxs);
        auto* row = new QWidget(chip);
        auto* ch = hbox(row, 0);
        auto* insert = button(name, ButtonVariant::Subtle, QStringLiteral("file"), row);
        setButtonSmall(insert, true);
        insert->setFont(font::mono());
        insert->setToolTip(FW_TR("Add %1 below the cursor's line").arg(name));
        auto* del = new IconButton(QStringLiteral("trash"), FW_TR("Delete %1 from the folder").arg(name), IconButton::Size::Tiny, row);
        del->setToolTip(FW_TR("Delete %1 from the folder").arg(name));
        ch->addWidget(insert);
        ch->addWidget(del);
        chip->body()->addWidget(row);
        connect(insert, &QPushButton::clicked, this, [this, name] { insertPayloadLine(name); });
        connect(del, &QToolButton::clicked, this, [this, name] { askDelete(name); });
        m_chipLayout->addWidget(chip);
    }
    m_noChips->setVisible(files.isEmpty());
    m_chips->setVisible(!files.isEmpty());
    revalidate();
}

void AutoloaderTab::insertLine(const QString& line)
{
    const al::Insertion ins = al::insertLineAfter(m_editor->toPlainText(), m_editor->textCursor().position(), line);
    QTextCursor c(m_editor->document());
    c.select(QTextCursor::Document);
    c.insertText(ins.text); // one undo step
    QTextCursor at = m_editor->textCursor();
    at.setPosition(std::min(ins.cursor, m_editor->document()->characterCount() - 1));
    m_editor->setTextCursor(at);
    m_editor->ensureCursorVisible();
    m_editor->setFocus();
}

void AutoloaderTab::insertPayloadLine(const QString& fileName)
{
    insertLine(fileName);
}

void AutoloaderTab::insertSleepLine(int ms)
{
    insertLine(al::sleepLine(ms));
}

void AutoloaderTab::askInsertSleep()
{
    auto* s = new Sheet(window(), FW_TR("Insert a sleep"), Sheet::Icon::None);
    auto* ms = new QSpinBox(s);
    ms->setRange(100, 60000);
    ms->setSingleStep(500);
    ms->setValue(5000);
    ms->setSuffix(QStringLiteral(" ms"));
    s->body()->addWidget(field(FW_TR("Wait"), ms, FW_TR("5000 ms = 5 seconds. Added as !<ms> below the cursor's line."), s));
    s->addButton(FW_TR("Cancel"), Sheet::Role::Safe);
    s->addButton(FW_TR("Insert"), Sheet::Role::Primary);
    ms->setFocus();
    ms->selectAll();
    if (s->exec() == 1)
        insertSleepLine(ms->value());
}

void AutoloaderTab::revalidate()
{
    const bool hasFolder = m_demo || m_hasFolder;
    const QString text = m_editor->toPlainText();
    const al::Validation v = al::validate(text, m_files, hasFolder);
    if (!hasFolder && text.isEmpty())
        m_validation->clear();
    else if (!v.errors.isEmpty())
        m_validation->set((v.errors.size() == 1 ? FW_TR("%1 problem: %2") : FW_TR("%1 problems: %2")).arg(v.errors.size()).arg(v.errors.first()), Tone::Err);
    else if (!v.warnings.isEmpty())
        m_validation->set(v.warnings.size() > 1 ? FW_TR("%1 (+%2 more)").arg(v.warnings.first(), QString::number(v.warnings.size() - 1)) : v.warnings.first(), Tone::Warn);
    else
        m_validation->set(m_dirty ? FW_TR("Looks good · unsaved changes") : FW_TR("Looks good"), Tone::Ok);
    m_validation->setToolTip((v.errors + v.warnings).join(u'\n'));
    m_counts->setText(QStringLiteral("%1 · %2").arg((v.payloads == 1 ? FW_TR("%1 payload") : FW_TR("%1 payloads")).arg(v.payloads),
                                                   (v.sleeps == 1 ? FW_TR("%1 sleep") : FW_TR("%1 sleeps")).arg(v.sleeps)));
    const bool ready = hasFolder && !isBusy() && !m_loadPending;
    m_save->setEnabled(ready);
    m_sleep->setEnabled(hasFolder && !isBusy());
    m_add->setEnabled(ready);
    m_reload->setEnabled(ready);
    m_editor->setReadOnly(!hasFolder);
}

void AutoloaderTab::save()
{
    const al::Validation v = al::validate(m_editor->toPlainText(), m_files, true);
    if (!v.errors.isEmpty()) {
        // Plain text, one problem per line.
        if (!Sheet::confirm(window(), FW_TR("Save autoload.txt with problems?"),
                            FW_TR("The autoloader would stumble on these lines:\n%1").arg(v.errors.mid(0, 4).join(u'\n')),
                            FW_TR("Keep editing"), FW_TR("Save anyway"), false, Sheet::Icon::Warning))
            return;
    }
    saveNow();
}

void AutoloaderTab::saveNow()
{
    if (m_demo)
        return; // the specimen folder is not on disk
    const QString dir = folder();
    const QString text = m_editor->toPlainText();
    runLocal<al::SaveResult>([dir, text](const CancellationToken&) { return al::save(dir, text); },
                             [this, dir](const Result<al::SaveResult>& r) {
                                 if (!r.ok()) {
                                     emit toastRequested(Tone::Err, FW_TR("autoload.txt not saved"), r.error().message);
                                     return;
                                 }
                                 if (dir != folder())
                                     return;
                                 m_dirty = false;
                                 const QString norm = al::normalize(m_editor->toPlainText());
                                 if (norm != m_editor->toPlainText()) {
                                     const int pos = m_editor->textCursor().position();
                                     m_editor->setPlainText(norm); // what is on disk now (LF, blank runs collapsed)
                                     QTextCursor c = m_editor->textCursor();
                                     c.setPosition(std::min(pos, m_editor->document()->characterCount() - 1));
                                     m_editor->setTextCursor(c);
                                     m_dirty = false;
                                 }
                                 m_editor->document()->setModified(false);
                                 m_badge->set(FW_TR("Saved"), Tone::Ok);
                                 m_saved->setText(!r->changed ? FW_TR("Already up to date.")
                                                  : r->backupPath.isEmpty() ? FW_TR("Saved.")
                                                                            : FW_TR("Saved; the previous file is %1.").arg(QFileInfo(r->backupPath).fileName()));
                                 revalidate();
                                 emit toastRequested(Tone::Ok, FW_TR("autoload.txt saved"), nativePath(r->path));
                             });
}

void AutoloaderTab::addPayload()
{
    const QString dir = folder();
    const QString src = QFileDialog::getOpenFileName(window(), FW_TR("Choose a payload to add"), QString(),
                                                     FW_TR("Payloads") + QStringLiteral(" (*.elf *.bin);;") + FW_TR("All files") + QStringLiteral(" (*)"));
    if (src.isEmpty() || dir != folder())
        return; // the folder changed under the dialog: the choice was made for the other one
    const QString name = QFileInfo(src).fileName();
    if (!al::isPayloadName(name)) {
        Sheet::message(window(), FW_TR("Not a payload"), FW_TR("Only .elf and .bin files can be autoloaded; .js files are not run by the autoloader."), Sheet::Icon::Warning);
        return;
    }
    bool overwrite = false;
    if (QFileInfo::exists(QDir(dir).filePath(name))) {
        if (!Sheet::confirm(window(), FW_TR("Replace %1?").arg(name),
                            FW_TR("%1 is already in the folder. Replace it with the chosen file?").arg(Sheet::monoSpan(name)),
                            FW_TR("Cancel"), FW_TR("Replace"), true, Sheet::Icon::Warning))
            return;
        overwrite = true;
    }
    if (dir == folder())
        addPayloadFile(src, overwrite);
}

void AutoloaderTab::addPayloadFile(const QString& source, bool overwrite)
{
    if (m_demo)
        return; // the specimen folder is not on disk
    const QString dir = folder();
    runLocal<QStringList>(
        [dir, source, overwrite](const CancellationToken& token) -> Result<QStringList> {
            FW_TRY_ASSIGN(QString name, al::addPayload(dir, source, overwrite, token));
            return QStringList{name} + al::payloadFiles(dir);
        },
        [this](const Result<QStringList>& r) {
            if (!r.ok()) {
                emit toastRequested(Tone::Err, FW_TR("Payload not added"), r.error().message);
                return;
            }
            const QString name = r->first();
            setFiles(r->mid(1));
            // A payload that was replaced is in the list already: a second line would load it twice.
            bool listed = false;
            for (const al::Line& line : al::parse(m_editor->toPlainText()))
                listed = listed || line.payload.compare(name, Qt::CaseInsensitive) == 0;
            if (!listed)
                insertPayloadLine(name);
            emit toastRequested(Tone::Ok, FW_TR("%1 added").arg(name),
                                listed ? FW_TR("Copied into the folder; autoload.txt lists it already.")
                                       : FW_TR("Copied into the folder and added to autoload.txt; save to keep the line."));
        });
}

void AutoloaderTab::askDelete(const QString& fileNameIn)
{
    // Copies: the chip that owns the name can be rebuilt while the question is open.
    const QString fileName = fileNameIn;
    const QString dir = folder();
    if (!Sheet::confirm(window(), FW_TR("Delete %1?").arg(fileName),
                        FW_TR("%1 is removed from the folder (to the recycle bin when the system has one), and its lines in "
                                       "autoload.txt with the sleeps right after them are taken out of the editor.")
                            .arg(Sheet::monoSpan(fileName)),
                        FW_TR("Keep"), FW_TR("Delete"), true, Sheet::Icon::Warning))
        return;
    if (dir == folder())
        deletePayloadFile(fileName);
}

void AutoloaderTab::deletePayloadFile(const QString& fileName)
{
    if (m_demo)
        return; // the specimen folder is not on disk
    const QString dir = folder();
    struct Deleted {
        bool trashed = false;
        QStringList files;
    };
    runLocal<Deleted>(
        [dir, fileName, toRecycleBin = m_useRecycleBin](const CancellationToken&) -> Result<Deleted> {
            Deleted d;
            FW_TRY(al::deletePayload(dir, fileName, toRecycleBin, &d.trashed));
            d.files = al::payloadFiles(dir);
            return d;
        },
        [this, fileName](const Result<Deleted>& r) {
            if (!r.ok()) {
                emit toastRequested(Tone::Err, FW_TR("%1 not deleted").arg(fileName), r.error().message);
                return;
            }
            setFiles(r->files);
            const QString before = m_editor->toPlainText();
            const QString after = al::removePayloadLines(before, fileName);
            if (after != before) {
                QTextCursor c(m_editor->document());
                c.select(QTextCursor::Document);
                c.insertText(after);
            }
            emit toastRequested(Tone::Ok, FW_TR("%1 deleted").arg(fileName),
                                r->trashed ? FW_TR("Moved to the recycle bin. Save autoload.txt to keep the edited list.")
                                           : FW_TR("Save autoload.txt to keep the edited list."));
        });
}

void AutoloaderTab::applyDemo()
{
    m_demo = true;
    m_current = QStringLiteral("D:/PS5/USB/ps5_autoloader"); // specimen only: nothing is read or written
    m_folder->blockSignals(true);
    m_folder->setPath(m_current);
    m_folder->blockSignals(false);
    m_folder->setError({});
    setFiles({QStringLiteral("elfldr.elf"), QStringLiteral("ftpsrv.elf"), QStringLiteral("kstuff.elf"), QStringLiteral("shadowmountplus.elf")});
    // The order the ShadowMount+ README shows, behind a LAN-reachable elfldr (!4000).
    m_editor->setPlainText(al::defaultHeader() + QStringLiteral("elfldr.elf\n!4000\nshadowmountplus.elf\n!3000\nkstuff.elf\nftpsrv.elf  # FTP on 2121\n@sync\n"));
    m_badge->set(FW_TR("autoload.txt loaded"), Tone::Ok);
    m_saved->clear();
    revalidate();
}

void AutoloaderTab::clearDemo()
{
    m_demo = false;
    m_current = al::folderFromSettings(m_ctx.settings().snapshot());
    m_folder->blockSignals(true);
    m_folder->setPath(m_current);
    m_folder->blockSignals(false);
    m_dirty = false;
    load(); // the saved folder again (or an empty editor)
    revalidate();
}

} // namespace fw::pages
