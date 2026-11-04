#pragma once

// PS5 connect › Autoloader (legacy Y2JB › Autoloader, Y2J-16): edits
// autoload.txt in a LOCAL ps5_autoloader folder (connect/ps5/Autoloader.h).
// The folder is the ps5.autoloaderFolder setting (legacy al_dir).

#include "app/pages/ps5/ConsoleToolTab.h"
#include "connect/ps5/Autoloader.h"

#include <QPlainTextEdit>

namespace fw::pages {

class AutoloaderTab : public ConsoleToolTab {
    Q_OBJECT
public:
    AutoloaderTab(AppContext& ctx, EndpointFn endpointFn, QWidget* parent = nullptr);
    void applyDemo() override;
    void clearDemo() override;

    // Test hooks (no dialogs).
    QString folder() const;
    void openFolder(const QString& folder); // also stores the setting
    QPlainTextEdit* editor() { return m_editor; }
    QStringList chipNames() const { return m_files; }
    bool isDirty() const { return m_dirty; }
    QString validationText() const { return m_validation->text(); }
    void insertPayloadLine(const QString& fileName);
    void insertSleepLine(int ms);
    void saveNow();                             // no confirmation
    void addPayloadFile(const QString& source, bool overwrite);
    void deletePayloadFile(const QString& fileName);

private:
    void load();
    void setFiles(const QStringList& files);
    void insertLine(const QString& line);
    void revalidate();
    void save();
    void addPayload();
    void askInsertSleep();
    void askDelete(const QString& fileName);

    ui::PathField* m_folder;
    ui::Badge* m_badge;
    QPushButton* m_reload;
    QPlainTextEdit* m_editor;
    ui::MessageRow* m_validation;
    QLabel* m_counts;
    QPushButton* m_sleep;
    QPushButton* m_save;
    QPushButton* m_add;
    QWidget* m_chips;
    ui::FlowLayout* m_chipLayout;
    QLabel* m_noChips;
    QLabel* m_saved;
    QStringList m_files;
    QString m_current; // the folder the editor's text belongs to
    bool m_dirty = false;
    bool m_demo = false;
};

} // namespace fw::pages
