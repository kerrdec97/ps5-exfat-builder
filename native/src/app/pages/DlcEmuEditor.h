#pragma once

// dlc_emu.ini editor (legacy Advanced › DLC-emu): pick the game's app0
// folder, edit the entries in a table, find entries offline, validate live,
// save with a .bak. Lives on the Library page as the "DLC emulator" tab.

#include "app/ui/Widgets.h"
#include "engines/tools/DlcEmu.h"

#include <QTableWidget>

namespace fw::pages {

class DlcEmuEditor : public QWidget {
    Q_OBJECT
public:
    explicit DlcEmuEditor(QWidget* parent = nullptr);
    void openFolder(const QString& app0);
    QString folder() const;
    std::vector<tools::dlcemu::Entry> entries() const; // from the table
    void setEntries(const std::vector<tools::dlcemu::Entry>& entries);
    QString validationText() const { return m_validation->text(); }
    QTableWidget* table() { return m_table; }
signals:
    void toastRequested(fw::ui::Tone tone, const QString& title, const QString& body);

private:
    void load();
    void addRow(const tools::dlcemu::Entry& e);
    void addEntry();
    void duplicateRow();
    void removeRow();
    void findEntries();
    void save();
    void revalidate();
    void updateRowControls();

    ui::PathField* m_folder;
    ui::Badge* m_badge;
    QTableWidget* m_table;
    ui::EmptyState* m_empty;
    ui::MessageRow* m_validation;
    QLabel* m_bytes;
    QPushButton* m_add;
    QPushButton* m_duplicate;
    QPushButton* m_remove;
    QPushButton* m_find;
    QPushButton* m_save;
    std::vector<tools::dlcemu::Entry> m_loaded; // keeps unknown keys per row (Qt::UserRole index)
    bool m_dirty = false;
};

} // namespace fw::pages
