#include "app/pages/LogsPage.h"

#include "app/AppContext.h"
#include "core/FileUtil.h"
#include "core/Paths.h"
#include "engines/general/Module.h"

#include <QClipboard>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QItemSelectionModel>

namespace fw::pages {

using namespace ui;
using namespace theme;

LogsPage::LogsPage(AppContext& ctx, LogModel& logModel, QWidget* parent)
    : Page(ctx, QStringLiteral("logs"), QStringLiteral("Logs"),
           QStringLiteral("Everything the app and its engines report, in one place. Raw tool output is kept here, never in error messages."), parent),
      m_model(logModel)
{
    m_filter = new LogFilterModel(this);
    m_filter->setSourceModel(&m_model);

    auto* copy = button(QStringLiteral("Copy shown"), ButtonVariant::Secondary, QStringLiteral("copy"), this);
    auto* exportBtn = button(QStringLiteral("Export…"), ButtonVariant::Secondary, QStringLiteral("download"), this);
    auto* folder = button(QStringLiteral("Open log folder"), ButtonVariant::Secondary, QStringLiteral("folder_open"), this);
    auto* clear = button(QStringLiteral("Clear"), ButtonVariant::Secondary, QStringLiteral("trash"), this);
    header()->addAction(copy);
    header()->addAction(exportBtn);
    header()->addAction(folder);
    header()->addAction(clear);
    connect(copy, &QPushButton::clicked, this, [this] {
        QGuiApplication::clipboard()->setText(shownText());
        emit toastRequested(Tone::Ok, QStringLiteral("Copied"), QStringLiteral("%1 lines on the clipboard").arg(formatCount(m_filter->rowCount())));
    });
    connect(exportBtn, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(window(), QStringLiteral("Export the shown log lines"),
                                                          QDir::homePath() + QStringLiteral("/farewell-log.txt"), QStringLiteral("Text files (*.txt)"));
        if (path.isEmpty())
            return;
        auto st = writeFileAtomic(path, shownText().toUtf8());
        if (st.ok())
            emit toastRequested(Tone::Ok, QStringLiteral("Log exported"), nativePath(path));
        else
            emit toastRequested(Tone::Err, QStringLiteral("Export failed"), st.error().message);
    });
    connect(folder, &QPushButton::clicked, this, [this] {
        const QString f = m_ctx.logFilePath().isEmpty() ? Paths::logsDir() : QFileInfo(m_ctx.logFilePath()).absolutePath();
        openInFileManager(f);
    });
    connect(clear, &QPushButton::clicked, this, [this] {
        m_model.clearView();
        updateFooter();
    });

    auto* card = new Card(this, Card::Variant::Table);
    auto* h = card->header();
    h->setSpacing(space::md);
    m_level = new Segmented(card);
    m_level->addSegment(QStringLiteral("All"));
    m_level->addSegment(QStringLiteral("Info"));
    m_level->addSegment(QStringLiteral("Warnings"), QStringLiteral("warning"));
    m_level->addSegment(QStringLiteral("Errors"), QStringLiteral("error"));
    m_level->setAccessibleName(QStringLiteral("Level"));
    h->addWidget(m_level);
    m_source = dropdown(card);
    m_source->setMinimumWidth(160);
    m_source->setAccessibleName(QStringLiteral("Source"));
    h->addWidget(m_source);
    m_job = dropdown(card);
    m_job->setMinimumWidth(160);
    m_job->setAccessibleName(QStringLiteral("Job"));
    h->addWidget(m_job);
    m_search = textInput(QStringLiteral("Search messages"), false, card);
    m_search->setFixedWidth(260);
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(QStringLiteral("Search messages"));
    h->addWidget(m_search);
    h->addStretch(1);
    h->addWidget(label(QStringLiteral("Follow"), TextStyle::Body, color::fg_1, card));
    m_follow = new Switch(card);
    m_follow->setChecked(true);
    m_follow->setAccessibleName(QStringLiteral("Follow new lines"));
    h->addWidget(m_follow);

    m_table = new LogTable(card);
    m_table->setModel(m_filter);
    m_table->setColumnWidth(LogModel::TimeColumn, 116);
    m_table->setColumnWidth(LogModel::LevelColumn, 92);
    m_table->setColumnWidth(LogModel::SourceColumn, 120);
    m_table->setColumnWidth(LogModel::JobColumn, 90);
    m_table->setMinimumHeight(12 * size::rowCompact + 40);
    card->body()->addWidget(m_table, 1);

    m_detail = new QWidget(card);
    auto* dl = vbox(m_detail, 6, QMargins(space::lg, space::md, space::lg, space::md));
    auto* dh = new QHBoxLayout;
    dh->setSpacing(space::sm);
    dh->addWidget(label(QStringLiteral("Selected line"), TextStyle::Overline, color::fg_4, m_detail));
    m_detailMeta = caption({}, m_detail);
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
    card->body()->addWidget(hairline(card, true));
    card->body()->addWidget(m_detail);
    m_detail->hide();
    connect(m_openJob, &Link::activated, this, [this] {
        if (!m_selectedJob.isEmpty())
            emit navigateRequested({QStringLiteral("queue"), {{QStringLiteral("jobId"), m_selectedJob}}});
    });
    connect(copyLine, &Link::activated, this, [this] { QGuiApplication::clipboard()->setText(m_detailText->text()); });

    auto* f = card->footer();
    m_footerLeft = new ElideLabel({}, Qt::ElideRight, card);
    m_footerLeft->setFont(font::caption());
    setTextColor(m_footerLeft, color::fg_3);
    m_footerRight = new ElideLabel({}, Qt::ElideMiddle, card);
    m_footerRight->setFont(font::monoSmall());
    setTextColor(m_footerRight, color::fg_3);
    m_footerRight->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    f->addWidget(m_footerLeft, 1);
    f->addWidget(m_footerRight, 1);
    content()->addWidget(card, 1);

    connect(m_level, &Segmented::currentChanged, this, &LogsPage::setLevelIndex);
    connect(m_source, &QComboBox::currentIndexChanged, this, [this](int) {
        m_filter->setSource(m_source->currentData().toString());
        updateFooter();
    });
    connect(m_job, &QComboBox::currentIndexChanged, this, [this](int) {
        m_filter->setJobId(m_job->currentData().toString());
        updateFooter();
    });
    connect(m_search, &QLineEdit::textChanged, this, &LogsPage::setSearch);
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
    m_job->setCurrentIndex(std::max(0, m_job->findData(curJob)));
    m_job->blockSignals(false);
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
    by << QStringLiteral("level: %1").arg(QStringList{QStringLiteral("All"), QStringLiteral("Info"), QStringLiteral("Warnings"), QStringLiteral("Errors")}.value(m_level->currentIndex()));
    by << QStringLiteral("source: %1").arg(m_source->currentData().toString().isEmpty() ? QStringLiteral("All") : m_source->currentData().toString());
    if (!m_job->currentData().toString().isEmpty())
        by << QStringLiteral("job: %1").arg(m_job->currentText());
    if (!m_search->text().trimmed().isEmpty())
        by << QStringLiteral("search: \"%1\"").arg(m_search->text().trimmed());
    m_footerLeft->setFullText(QStringLiteral("Showing %1 of %2 lines · filtered by %3").arg(formatCount(m_filter->rowCount()), formatCount(m_model.rowCount()), by.join(QStringLiteral(" · "))));
    const QString path = m_ctx.logFilePath();
    const QFileInfo fi(path);
    m_footerRight->setFullText(path.isEmpty() ? QString() : QStringLiteral("%1 · %2").arg(nativePath(path), formatBytes(fi.size())));
    emit statusContextChanged();
}

void LogsPage::updateDetail()
{
    const QModelIndex cur = m_table->currentIndex();
    if (!cur.isValid()) {
        m_detail->hide();
        return;
    }
    const QModelIndex src = m_filter->mapToSource(cur);
    const LogRecord* rec = m_model.recordAt(src.row());
    if (!rec) {
        m_detail->hide();
        return;
    }
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
    m_detail->show();
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
