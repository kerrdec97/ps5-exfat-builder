#pragma once

// Build image (Main artboard): Source · Output (format segmented) ·
// Verification and job settings · Jobs dock.

#include "app/pages/Page.h"
#include "app/services/GameInfo.h"
#include "app/ui/Components.h"

namespace fw::pages {

class BuildImagePage : public JobFormPage {
    Q_OBJECT
public:
    BuildImagePage(AppContext& ctx, JobQueueModel& queueModel, QWidget* parent = nullptr);
    bool acceptsDrop(const QList<QUrl>& urls) const override;
    void dropUrls(const QList<QUrl>& urls) override;
    void navigateWith(const QVariantMap& args) override;
    void refresh() override;
    QString statusContext() const override;
    QStringList demoStates() const override { return {QStringLiteral("space-error")}; }
    void applyDemoState(const QString& name) override;
    void clearDemoState() override;
    void setDragHighlight(bool on) override;
    void setSource(const QString& folder);
    QString format() const;
    void showSpaceError(qint64 needed, qint64 free, const QString& drive); // real flow + demo state

protected:
    Result<QString> submit(bool buildNow) override;
    QString validateForm() override;

private:
    void onSummary(const ui::GameSummary& s);
    void updateOutput();
    void checkSpace();
    qint64 neededBytes() const;
    QString fileName() const;

    ui::GameInfoReader* m_reader;
    ui::GameSummary m_summary;
    ui::PathField* m_source;
    ui::Badge* m_sourceBadge;
    QWidget* m_strip;
    ui::ElideLabel* m_stripTitle;
    ui::ElideLabel* m_stripMeta;
    ui::ChoiceTiles* m_format;
    ui::DropZone* m_drop;
    ui::IconTile* m_stripTile;
    ui::ElideLabel* m_stripPath;
    ui::MessageRow* m_sourceMsg;
    QLabel* m_formatHelp;
    ui::PathField* m_output;
    ui::MessageRow* m_spaceRow;
    ui::InfoBar* m_spaceBar;
    QLineEdit* m_fileName;
    quint64 m_spaceGen = 0;
    bool m_spaceOk = true;
    QString m_spaceProblem;
};

} // namespace fw::pages
