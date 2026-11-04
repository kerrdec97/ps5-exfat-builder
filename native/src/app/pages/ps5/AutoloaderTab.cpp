#include "app/pages/ps5/AutoloaderTab.h"

#include "app/AppContext.h"
#include "app/ui/Sheet.h"
#include "connect/ps5/Module.h"

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
    info->setMessage(QStringLiteral("ps5_autoloader runs the payloads listed in autoload.txt after the exploit, looking on USB drives, "
                                    "then /data, then savedata. This edits a folder on this PC: copy it to the console yourself."));
    l->addWidget(info);

    auto* pick = new Card(this);
    m_badge = new Badge(QStringLiteral("No folder"), Tone::Neutral, pick);
    m_badge->setIconName({});
    pick->setTitle(QStringLiteral("ps5_autoloader folder"), m_badge);
    auto* row = new QWidget(pick);
    auto* rh = hbox(row, space::sm);
    m_folder = new PathField(PathField::Mode::Directory, row);
    m_folder->setDialogTitle(QStringLiteral("Choose the ps5_autoloader folder"));
    m_folder->setPlaceholder(QStringLiteral("…/ps5_autoloader"));
    m_reload = button(QStringLiteral("Reload"), ButtonVariant::Secondary, QStringLiteral("refresh"), row);
    rh->addWidget(m_folder, 1);
    rh->addWidget(m_reload, 0, Qt::AlignTop);
    pick->body()->addWidget(row);
    l->addWidget(pick);

    auto* edit = new Card(this, Card::Variant::Table);
    auto* h = edit->header();
    h->addWidget(heading(QStringLiteral("autoload.txt"), TextStyle::Subtitle, edit));
    auto* hint = new ElideLabel(QStringLiteral("One .elf, .bin or .js per line · !1000 waits 1 s · # comment"), Qt::ElideRight, edit);
    hint->setFont(fontFor(TextStyle::Caption));
    setTextColor(hint, color::fg_3);
    h->addWidget(hint, 1);
    m_sleep = button(QStringLiteral("Insert !sleep…"), ButtonVariant::Secondary, QStringLiteral("timer"), edit);
    m_save = button(QStringLiteral("Save autoload.txt"), ButtonVariant::Primary, QStringLiteral("save"), edit);
    h->addWidget(m_sleep);
    h->addWidget(m_save);
    m_editor = new QPlainTextEdit(edit);
    m_editor->setFont(font::mono());
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setTabChangesFocus(true);
    m_editor->setMinimumHeight(12 * font::lineMono + 2 * space::md);
    m_editor->setAccessibleName(QStringLiteral("autoload.txt"));
    m_editor->setPlaceholderText(QStringLiteral("Choose the ps5_autoloader folder first."));
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
    m_add = button(QStringLiteral("Add payload…"), ButtonVariant::Secondary, QStringLiteral("plus"), files);
    files->setTitle(QStringLiteral("Payloads in the folder"), m_add);
    files->body()->addWidget(caption(QStringLiteral("Click a name to add it on a new line below the cursor. Add payload copies a file into the folder."), files));
    m_chips = new QWidget(files);
    m_chipLayout = new FlowLayout(m_chips, space::sm, space::sm);
    files->body()->addWidget(m_chips);
    m_noChips = caption(QStringLiteral("No .elf, .bin or .js files in the folder."), files);
    files->body()->addWidget(m_noChips);
    l->addWidget(files);
    l->addStretch(1);

    connect(m_folder, &PathField::pathChanged, this, [this](const QString& p) {
        if (m_demo)
            return;
        if (m_dirty && !Sheet::confirm(window(), QStringLiteral("Discard the unsaved changes?"),
                                       QStringLiteral("autoload.txt was edited and not saved."), QStringLiteral("Keep editing"),
                                       QStringLiteral("Discard"), true, Sheet::Icon::Warning)) {
            m_folder->blockSignals(true); // keep editing the folder the text belongs to
            m_folder->setPath(m_current);
            m_folder->blockSignals(false);
            return;
        }
        openFolder(p);
    });
    connect(m_reload, &QPushButton::clicked, this, [this] {
        if (m_dirty && !Sheet::confirm(window(), QStringLiteral("Discard the unsaved changes?"),
                                       QStringLiteral("Reloading reads autoload.txt from the folder again."), QStringLiteral("Keep editing"),
                                       QStringLiteral("Reload"), true, Sheet::Icon::Warning))
            return;
        load();
    });
    connect(m_editor, &QPlainTextEdit::textChanged, this, [this] {
        if (!m_demo)
            m_dirty = true;
        revalidate();
    });
    connect(m_sleep, &QPushButton::clicked, this, &AutoloaderTab::askInsertSleep);
    connect(m_save, &QPushButton::clicked, this, &AutoloaderTab::save);
    connect(m_add, &QPushButton::clicked, this, &AutoloaderTab::addPayload);
    connect(this, &ConsoleToolTab::busyChanged, this, [this](bool) { revalidate(); });

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
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
        // Nothing from another folder stays in the editor.
        m_editor->blockSignals(true);
        m_editor->clear();
        m_editor->blockSignals(false);
        m_dirty = false;
        setFiles({});
        m_folder->setError(dir.isEmpty() ? QString() : QStringLiteral("The folder does not exist."));
        m_badge->set(dir.isEmpty() ? QStringLiteral("No folder") : QStringLiteral("Folder missing"), dir.isEmpty() ? Tone::Neutral : Tone::Err);
        m_saved->clear();
        return;
    }
    m_folder->setError({});
    runLocal<Loaded>(
        [dir](const CancellationToken&) -> Result<Loaded> {
            FW_TRY_ASSIGN(al::Loaded file, al::load(dir));
            return Loaded{file, al::payloadFiles(dir)};
        },
        [this, dir](const Result<Loaded>& r) {
            if (dir != folder())
                return;
            if (!r.ok()) {
                m_folder->setError(r.error().message);
                m_badge->set(QStringLiteral("Not loaded"), Tone::Err);
                return;
            }
            setFiles(r->payloads);
            m_editor->setPlainText(r->file.text);
            m_dirty = !r->file.existed; // the header is new: saving creates the file
            m_badge->set(r->file.existed ? QStringLiteral("autoload.txt loaded") : QStringLiteral("New autoload.txt"),
                         r->file.existed ? Tone::Ok : Tone::Info);
            m_saved->setText(r->file.existed ? QString() : QStringLiteral("Not saved yet: the default header was created."));
            revalidate();
        });
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
        insert->setToolTip(QStringLiteral("Add %1 below the cursor's line").arg(name));
        auto* del = new IconButton(QStringLiteral("trash"), QStringLiteral("Delete %1 from the folder").arg(name), IconButton::Size::Tiny, row);
        del->setToolTip(QStringLiteral("Delete %1 from the folder").arg(name));
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
    auto* s = new Sheet(window(), QStringLiteral("Insert a sleep"), Sheet::Icon::None);
    auto* ms = new QSpinBox(s);
    ms->setRange(100, 60000);
    ms->setSingleStep(500);
    ms->setValue(5000);
    ms->setSuffix(QStringLiteral(" ms"));
    s->body()->addWidget(field(QStringLiteral("Wait"), ms, QStringLiteral("5000 ms = 5 seconds. Added as !<ms> below the cursor's line."), s));
    s->addButton(QStringLiteral("Cancel"), Sheet::Role::Safe);
    s->addButton(QStringLiteral("Insert"), Sheet::Role::Primary);
    ms->setFocus();
    ms->selectAll();
    if (s->exec() == 1)
        insertSleepLine(ms->value());
}

void AutoloaderTab::revalidate()
{
    const bool hasFolder = m_demo || (!folder().isEmpty() && QFileInfo(folder()).isDir());
    const QString text = m_editor->toPlainText();
    const al::Validation v = al::validate(text, m_files, hasFolder);
    if (!hasFolder && text.isEmpty())
        m_validation->clear();
    else if (!v.errors.isEmpty())
        m_validation->set(QStringLiteral("%1 problem%2: %3").arg(v.errors.size()).arg(v.errors.size() == 1 ? QString() : QStringLiteral("s")).arg(v.errors.first()), Tone::Err);
    else if (!v.warnings.isEmpty())
        m_validation->set(v.warnings.first() + (v.warnings.size() > 1 ? QStringLiteral(" (+%1 more)").arg(v.warnings.size() - 1) : QString()), Tone::Warn);
    else
        m_validation->set(m_dirty ? QStringLiteral("Looks good · unsaved changes") : QStringLiteral("Looks good"), Tone::Ok);
    m_validation->setToolTip((v.errors + v.warnings).join(u'\n'));
    m_counts->setText(QStringLiteral("%1 payload%2 · %3 sleep%4").arg(v.payloads).arg(v.payloads == 1 ? QString() : QStringLiteral("s")).arg(v.sleeps).arg(v.sleeps == 1 ? QString() : QStringLiteral("s")));
    const bool ready = hasFolder && !isBusy();
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
        QStringList shown;
        for (const QString& e : v.errors.mid(0, 4))
            shown << e.toHtmlEscaped();
        if (!Sheet::confirm(window(), QStringLiteral("Save autoload.txt with problems?"),
                            QStringLiteral("The autoloader would stumble on these lines:<br>%1").arg(shown.join(QStringLiteral("<br>"))),
                            QStringLiteral("Keep editing"), QStringLiteral("Save anyway"), false, Sheet::Icon::Warning))
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
                                     emit toastRequested(Tone::Err, QStringLiteral("autoload.txt not saved"), r.error().message);
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
                                 m_badge->set(QStringLiteral("Saved"), Tone::Ok);
                                 m_saved->setText(!r->changed ? QStringLiteral("Already up to date.")
                                                  : r->backupPath.isEmpty() ? QStringLiteral("Saved.")
                                                                            : QStringLiteral("Saved; the previous file is %1.").arg(QFileInfo(r->backupPath).fileName()));
                                 revalidate();
                                 emit toastRequested(Tone::Ok, QStringLiteral("autoload.txt saved"), nativePath(r->path));
                             });
}

void AutoloaderTab::addPayload()
{
    const QString src = QFileDialog::getOpenFileName(window(), QStringLiteral("Choose a payload to add"), QString(),
                                                     QStringLiteral("Payloads (*.elf *.bin *.js);;All files (*)"));
    if (src.isEmpty())
        return;
    const QString name = QFileInfo(src).fileName();
    if (!al::isPayloadName(name)) {
        Sheet::message(window(), QStringLiteral("Not a payload"), QStringLiteral("Only .elf, .bin and .js files can be autoloaded."), Sheet::Icon::Warning);
        return;
    }
    bool overwrite = false;
    if (QFileInfo::exists(QDir(folder()).filePath(name))) {
        if (!Sheet::confirm(window(), QStringLiteral("Replace %1?").arg(name.toHtmlEscaped()),
                            QStringLiteral("%1 is already in the folder. Replace it with the chosen file?").arg(Sheet::monoSpan(name)),
                            QStringLiteral("Cancel"), QStringLiteral("Replace"), true, Sheet::Icon::Warning))
            return;
        overwrite = true;
    }
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
                emit toastRequested(Tone::Err, QStringLiteral("Payload not added"), r.error().message);
                return;
            }
            const QString name = r->first();
            setFiles(r->mid(1));
            insertPayloadLine(name);
            emit toastRequested(Tone::Ok, QStringLiteral("%1 added").arg(name), QStringLiteral("Copied into the folder and added to autoload.txt; save to keep the line."));
        });
}

void AutoloaderTab::askDelete(const QString& fileName)
{
    if (!Sheet::confirm(window(), QStringLiteral("Delete %1?").arg(fileName.toHtmlEscaped()),
                        QStringLiteral("%1 is removed from the folder (to the recycle bin when the system has one), and its lines in "
                                       "autoload.txt with the sleeps right after them are taken out of the editor.")
                            .arg(Sheet::monoSpan(fileName)),
                        QStringLiteral("Keep"), QStringLiteral("Delete"), true, Sheet::Icon::Warning))
        return;
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
        [dir, fileName](const CancellationToken&) -> Result<Deleted> {
            Deleted d;
            FW_TRY(al::deletePayload(dir, fileName, &d.trashed));
            d.files = al::payloadFiles(dir);
            return d;
        },
        [this, fileName](const Result<Deleted>& r) {
            if (!r.ok()) {
                emit toastRequested(Tone::Err, QStringLiteral("%1 not deleted").arg(fileName), r.error().message);
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
            emit toastRequested(Tone::Ok, QStringLiteral("%1 deleted").arg(fileName),
                                r->trashed ? QStringLiteral("Moved to the recycle bin. Save autoload.txt to keep the edited list.")
                                           : QStringLiteral("Save autoload.txt to keep the edited list."));
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
    m_editor->setPlainText(al::defaultHeader() + QStringLiteral("elfldr.elf\n!1000\nkstuff.elf\n!3000\nftpsrv.elf\nshadowmountplus.elf\n"));
    m_badge->set(QStringLiteral("autoload.txt loaded"), Tone::Ok);
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
