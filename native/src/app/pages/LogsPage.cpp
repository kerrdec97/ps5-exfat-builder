#include "app/pages/LogsPage.h"

#include "app/AppContext.h"
#include "app/ui/Icons.h"
#include "core/FileUtil.h"
#include "core/Paths.h"
#include "engines/general/Module.h"

#include <QAction>
#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QItemSelectionModel>

#include <algorithm>

namespace fw::pages {

using namespace ui;
using namespace theme;

LogsPage::LogsPage(AppContext& ctx, LogModel& logModel, QWidget* parent)
    : Page(ctx, QStringLiteral("logs"), QStringLiteral("Logs"), QStringLiteral("Everything the app and its engines report, in one place."), parent),
      m_model(logModel)
{
    m_filter = new LogFilterModel(this);
    m_filter->setSourceModel(&m_model);

    auto* copy = new IconButton(QStringLiteral("copy"), QStringLiteral("Copy the shown lines"), IconButton::Size::Regular, this);
    copy->setToolTip(QStringLiteral("Copy the shown lines"));
    auto* clear = new IconButton(QStringLiteral("trash"), QStringLiteral("Clear the view"), IconButton::Size::Regular, this);
    clear->setToolTip(QStringLiteral("Clear the view (the log file is kept)"));
    auto* exportBtn = button(QStringLiteral("Export…"), ButtonVariant::Secondary, QStringLiteral("download"), this);
    exportBtn->setToolTip(QStringLiteral("Save the shown lines as a text file"));
    m_openFolder = button(QStringLiteral("Open log folder"), ButtonVariant::Secondary, QStringLiteral("folder_open"), this);
    header()->addAction(copy);
    header()->addAction(clear);
    header()->addAction(exportBtn);
    header()->addAction(m_openFolder);
    connect(copy, &QToolButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(shownText());
        emit toastRequested(Tone::Ok, QStringLiteral("Copied"), QStringLiteral("%1 lines on the clipboard").arg(formatCount(m_filter->rowCount())));
    });
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(window(), QStringLiteral("Export the shown log lines"),
                                                          QDir::homePath() + QStringLiteral("/exfat-image-builder-log.txt"), QStringLiteral("Text files (*.txt)"));
        if (path.isEmpty())
            return;
        auto st = writeFileAtomic(path, shownText().toUtf8());
        if (st.ok())
            emit toastRequested(Tone::Ok, QStringLiteral("Log exported"), nativePath(path));
        else
            emit toastRequested(Tone::Err, QStringLiteral("Export failed"), st.error().message);
    });
    connect(m_openFolder, &QPushButton::clicked, this, [this] {
        const QString f = m_ctx.logFilePath().isEmpty() ? Paths::logsDir() : QFileInfo(m_ctx.logFilePath()).absolutePath();
        openInFileManager(f);
    });
    connect(clear, &QToolButton::clicked, this, [this] {
        m_model.clearView();
        updateCounts();
        updateFooter();
    });

    auto* card = new Card(this, Card::Variant::Table);
    card->setObjectName(QStringLiteral("logs-card"));
    auto* h = card->header();
    h->setSpacing(space::md);
    m_search = textInput(QStringLiteral("Search messages"), false, card);
    m_search->setMinimumWidth(180);
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(QStringLiteral("Search messages"));
    m_search->addAction(icon(QStringLiteral("search"), color::fg_4), QLineEdit::LeadingPosition);
    h->addWidget(m_search, 1);
    m_level = new Segmented(card);
    m_level->addSegment(QStringLiteral("All"));
    m_level->addSegment(QStringLiteral("Info"));
    m_level->addSegment(QStringLiteral("Warnings"));
    m_level->addSegment(QStringLiteral("Errors"));
    m_level->setAccessibleName(QStringLiteral("Level"));
    h->addWidget(m_level);
    m_source = dropdown(card);
    m_source->setMinimumWidth(130);
    m_source->setAccessibleName(QStringLiteral("Source"));
    m_source->setToolTip(QStringLiteral("Show one part of the app only"));
    h->addWidget(m_source);
    m_job = dropdown(card);
    m_job->setMinimumWidth(130);
    m_job->setMaximumWidth(220);
    m_job->setAccessibleName(QStringLiteral("Job"));
    m_job->setToolTip(QStringLiteral("Show one job only"));
    h->addWidget(m_job);

    m_table = new LogTable(card);
    m_table->setModel(m_filter);
    m_table->setColumnWidth(LogModel::TimeColumn, 112);
    m_table->setColumnWidth(LogModel::LevelColumn, 88);
    m_table->setColumnWidth(LogModel::SourceColumn, 110);
    m_table->setColumnWidth(LogModel::JobColumn, 112);
    m_table->setMinimumHeight(10 * size::rowCompact + 40);
    card->body()->addWidget(m_table, 1);
    m_noLines = new EmptyState(QStringLiteral("file_search"), QStringLiteral("No line matches"), QStringLiteral("Try another level, source or search."), card);
    auto* clearFiltersBtn = m_noLines->addAction(QStringLiteral("Clear filters"));
    connect(clearFiltersBtn, &QPushButton::clicked, this, &LogsPage::clearFilters);
    m_noLines->hide();
    card->body()->addWidget(m_noLines, 1);
    m_noLog = new EmptyState(QStringLiteral("file_text"), QStringLiteral("Nothing logged yet"), QStringLiteral("New lines appear here as the app works."), card);
    m_noLog->hide();
    card->body()->addWidget(m_noLog, 1);

    m_detail = new QWidget(card);
    m_detail->setObjectName(QStringLiteral("logs-detail"));
    auto* dl = vbox(m_detail, 6, QMargins(space::lg, space::md, space::lg, space::md));
    auto* dh = new QHBoxLayout;
    dh->setSpacing(space::md);
    dh->addWidget(label(QStringLiteral("Selected line"), TextStyle::Overline, color::fg_4, m_detail));
    m_detailMeta = caption({}, m_detail);
    m_detailMeta->setWordWrap(false);
    dh->addWidget(m_detailMeta);
    dh->addStretch(1);
    m_openJob = new Link(QStringLiteral("Open job"), m_detail);
    dh->addWidget(m_openJob);
    auto* copyLine = new Link(QStringLiteral("Copy line"), m_detail);
    dh->addWidget(copyLine);
    dl->addLayout(dh);
    m_detailText = label({}, TextStyle::Mono, color::fg_1, m_detail);
    m_detailText->setWordWrap(true);
    m_detailText->setTextInteractionFlags(Qt::TextSelectableByMouse);
    dl->addWidget(m_detailText);
    auto* detailLine = hairline(card, true);
    card->body()->addWidget(detailLine);
    card->body()->addWidget(m_detail);
    m_detail->hide();
    detailLine->hide();
    connect(m_openJob, &Link::activated, this, [this] {
        if (!m_selectedJob.isEmpty())
            emit navigateRequested({QStringLiteral("queue"), {{QStringLiteral("jobId"), m_selectedJob}}});
    });
    connect(copyLine, &Link::activated, this, [this] { QGuiApplication::clipboard()->setText(m_detailText->text()); });
    m_detailLine = detailLine;

    auto* f = card->footer();
    m_footerLeft = new ElideLabel({}, Qt::ElideRight, card);
    m_footerLeft->setFont(font::caption());
    setTextColor(m_footerLeft, color::fg_3);
    f->addWidget(m_footerLeft, 1);
    f->addWidget(label(QStringLiteral("Follow new lines"), TextStyle::Caption, color::fg_2, card));
    m_follow = new Switch(card);
    m_follow->setChecked(true);
    m_follow->setAccessibleName(QStringLiteral("Follow new lines"));
    m_follow->setToolTip(QStringLiteral("Keep the newest line in view"));
    f->addWidget(m_follow);
    content()->addWidget(card, 1);
    // The log card takes the page's height instead of the bottom spacer.
    if (auto* outer = qobject_cast<QBoxLayout*>(scrollArea()->widget()->layout())) {
        outer->setStretchFactor(content(), 1);
        for (int i = 0; i < outer->count(); ++i)
            if (outer->itemAt(i)->spacerItem())
                outer->setStretch(i, 0);
    }

    connect(m_level, &Segmented::currentChanged, this, &LogsPage::setLevelIndex);
    connect(m_source, &QComboBox::currentIndexChanged, this, [this](int) {
        m_filter->setSource(m_source->currentData().toString());
        updateFooter();
    });
    connect(m_job, &QComboBox::currentIndexChanged, this, [this](int) {
        m_filter->setJobId(m_job->currentData().toString());
        updateFooter();
    });
    // Typing re-filters up to 20,000 rows: wait for a short pause first.
    auto* searchDelay = new QTimer(this);
    searchDelay->setSingleShot(true);
    searchDelay->setInterval(150);
    connect(m_search, &QLineEdit::textChanged, searchDelay, qOverload<>(&QTimer::start));
    connect(searchDelay, &QTimer::timeout, this, [this] { setSearch(m_search->text()); });
    connect(m_follow, &QAbstractButton::toggled, m_table, &LogTable::setFollow);
    connect(m_table->selectionModel(), &QItemSelectionModel::currentRowChanged, this, [this](const QModelIndex&, const QModelIndex&) { updateDetail(); });
    connect(&m_model, &LogModel::sourcesChanged, this, &LogsPage::refreshSources);
    connect(&m_model, &LogModel::rowsAppended, this, [this](int) {
        updateCounts();
        updateFooter();
    });
    connect(&m_ctx.queue(), &JobQueue::jobAdded, this, [this](const QString&) { refreshSources(); });
    refreshSources();
    updateCounts();
    updateFooter();
}

void LogsPage::setLevelIndex(int index)
{
    switch (index) {
    case 1: m_filter->setMinimumLevel(LogLevel::Info); break;
    case 2: m_filter->setMinimumLevel(LogLevel::Warning); break;
    case 3: m_filter->setMinimumLevel(LogLevel::Error); break;
    default: m_filter->setMinimumLevel(LogLevel::Trace); break;
    }
    if (m_level->currentIndex() != index)
        m_level->setCurrentIndex(index);
    updateFooter();
}

void LogsPage::setSearch(const QString& text)
{
    if (m_search->text() != text)
        m_search->setText(text);
    m_filter->setText(text.trimmed());
    updateFooter();
}

void LogsPage::setJobFilter(const QString& jobId)
{
    int i = m_job->findData(jobId);
    if (i < 0 && !jobId.isEmpty()) {
        m_job->addItem(jobId, jobId);
        i = m_job->count() - 1;
    }
    m_job->setCurrentIndex(std::max(0, i));
    m_job->setVisible(m_job->count() > 1);
}

void LogsPage::clearFilters()
{
    setSearch({});
    setLevelIndex(0);
    m_source->setCurrentIndex(0);
    m_job->setCurrentIndex(0);
}

void LogsPage::refreshSources()
{
    const QString curSource = m_source->currentData().toString();
    m_source->blockSignals(true);
    m_source->clear();
    m_source->addItem(QStringLiteral("All sources"), QString());
    for (const QString& s : m_model.sources())
        m_source->addItem(s, s);
    m_source->setCurrentIndex(std::max(0, m_source->findData(curSource)));
    m_source->blockSignals(false);
    const QString curJob = m_job->currentData().toString();
    m_job->blockSignals(true);
    m_job->clear();
    m_job->addItem(QStringLiteral("All jobs"), QString());
    for (const auto& rec : m_ctx.queue().records())
        m_job->addItem(rec.title, rec.id);
    int j = m_job->findData(curJob);
    if (j < 0 && !curJob.isEmpty()) {
        m_job->addItem(curJob, curJob);
        j = m_job->count() - 1;
    }
    m_job->setCurrentIndex(std::max(0, j));
    m_job->blockSignals(false);
    // Only worth a control when there is a job to pick.
    m_job->setVisible(m_job->count() > 1);
}

void LogsPage::updateCounts()
{
    int warn = 0, err = 0;
    for (int r = 0; r < m_model.rowCount(); ++r)
        if (const LogRecord* rec = m_model.recordAt(r)) {
            if (rec->level == LogLevel::Warning) ++warn;
            else if (rec->level == LogLevel::Error) ++err;
        }
    m_level->setSegmentText(2, warn > 0 ? QStringLiteral("Warnings %1").arg(warn) : QStringLiteral("Warnings"));
    m_level->setSegmentText(3, err > 0 ? QStringLiteral("Errors %1").arg(err) : QStringLiteral("Errors"));
}

void LogsPage::updateFooter()
{
    QStringList by;
    if (m_level->currentIndex() > 0)
        by << QStringList{QString(), QStringLiteral("info and up"), QStringLiteral("warnings and errors"), QStringLiteral("errors")}.value(m_level->currentIndex());
    if (!m_source->currentData().toString().isEmpty())
        by << m_source->currentData().toString();
    if (!m_job->currentData().toString().isEmpty())
        by << QStringLiteral("job %1").arg(m_job->currentText());
    if (!m_search->text().trimmed().isEmpty())
        by << QStringLiteral("“%1”").arg(m_search->text().trimmed());
    const int shown = m_filter->rowCount(), total = m_model.rowCount();
    m_footerLeft->setFullText(by.isEmpty() ? QStringLiteral("%1 lines").arg(formatCount(total))
                                           : QStringLiteral("Showing %1 of %2 lines · %3").arg(formatCount(shown), formatCount(total), by.join(QStringLiteral(" · "))));
    const QString path = m_ctx.logFilePath();
    const QFileInfo fi(path);
    const QString file = path.isEmpty() ? QString() : QStringLiteral("%1 · %2").arg(nativePath(path), formatBytes(fi.size()));
    m_footerLeft->setToolTip(file);
    m_openFolder->setToolTip(file.isEmpty() ? QStringLiteral("Open the folder with the log files") : file);

    // Nothing to show: say why and offer the way back.
    const bool none = shown == 0;
    m_table->setVisible(!none);
    m_noLines->setVisible(none && total > 0);
    m_noLog->setVisible(none && total == 0);
    emit statusContextChanged();
}

void LogsPage::updateDetail()
{
    const QModelIndex cur = m_table->currentIndex();
    const LogRecord* rec = cur.isValid() ? m_model.recordAt(m_filter->mapToSource(cur).row()) : nullptr;
    m_detail->setVisible(rec != nullptr);
    m_detailLine->setVisible(rec != nullptr);
    if (!rec)
        return;
    m_selectedJob = rec->jobId;
    QStringList metaParts{rec->time.toString(QStringLiteral("HH:mm:ss.zzz")), rec->source, logLevelName(rec->level)};
    if (!rec->jobId.isEmpty())
        metaParts << QStringLiteral("job %1").arg(rec->jobId);
    m_detailMeta->setText(metaParts.join(QStringLiteral(" · ")));
    QString text = rec->message;
    if (!rec->fields.isEmpty()) {
        QStringList kv;
        for (auto it = rec->fields.constBegin(); it != rec->fields.constEnd(); ++it)
            kv << QStringLiteral("%1=%2").arg(it.key(), it.value().toVariant().toString());
        text += u'\n' + kv.join(u' ');
    }
    m_detailText->setText(text);
    m_openJob->setVisible(!rec->jobId.isEmpty());
}

QString LogsPage::shownText() const
{
    QString out;
    for (int r = 0; r < m_filter->rowCount(); ++r) {
        const QModelIndex src = m_filter->mapToSource(m_filter->index(r, 0));
        if (const LogRecord* rec = m_model.recordAt(src.row())) {
            out += rec->time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")) + u'\t' + logLevelName(rec->level) + u'\t' + rec->source + u'\t'
                   + rec->jobId + u'\t' + rec->message + u'\n';
        }
    }
    return out;
}

void LogsPage::navigateWith(const QVariantMap& args)
{
    if (args.contains(QStringLiteral("jobId")))
        setJobFilter(args.value(QStringLiteral("jobId")).toString());
    if (args.contains(QStringLiteral("source"))) {
        const int i = m_source->findData(args.value(QStringLiteral("source")).toString());
        m_source->setCurrentIndex(std::max(0, i));
    }
    if (args.value(QStringLiteral("level")).toString() == QLatin1String("error"))
        setLevelIndex(3);
    else if (args.value(QStringLiteral("level")).toString() == QLatin1String("warning"))
        setLevelIndex(2);
}

QString LogsPage::statusContext() const
{
    return QStringLiteral("Log level: %1 · keeping %2 files")
        .arg(m_ctx.settings().value(general::keys::kLogLevel).toString())
        .arg(m_ctx.settings().value(general::keys::kLogKeepFiles).toInt());
}

} // namespace fw::pages
