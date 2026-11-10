#pragma once

// In-window modal sheets (§4.17) and toasts (§4.16).
//
// A Sheet is a child of the main window: a `scrim` that covers everything and
// a 480/640 px panel in the middle. It runs its own event loop (exec()) so it
// behaves like a modal dialog, but it is part of the window, so screenshots
// capture it and the Windows title bar never appears on it.

#include "app/ui/Widgets.h"

#include <QCheckBox>
#include <QEventLoop>
#include <QPointer>
#include <QWidget>

namespace fw::ui {

class Sheet : public QWidget {
    Q_OBJECT
public:
    enum class Role { Safe, Primary, Destructive };
    enum class Icon { None, Info, Warning, Error };

    Sheet(QWidget* host, const QString& title, Icon icon = Icon::None, int width = theme::size::dialog);
    ~Sheet() override;

    QVBoxLayout* body() { return m_body; }
    // Body text (fg_1, wraps). addText() shows the text literally unless it
    // holds a span made by monoSpan() / monoLines() in this process; then it
    // is rich text and everything else in it must be escaped by the caller.
    // A "<span" from a file name or a server reply never switches the format.
    QLabel* addText(const QString& text);
    QLabel* addPlainText(const QString& text);   // always literal
    QLabel* addRichText(const QString& html);    // markup the app wrote; escape the outside parts
    QCheckBox* addCheckbox(const QString& text);
    QPushButton* addButton(const QString& text, Role role);
    void setTitle(const QString& title);

    // Shows the sheet and blocks until a button is pressed (returns its index),
    // Esc is pressed (returns the safe button's index, or -1), or close() is called.
    int exec();
    void open();                 // non-modal show (screenshots, countdowns)
    void close(int result = -1);
    bool isOpen() const { return isVisible(); }
    int result() const { return m_result; }
    // Sheets open right now, in any window. The main window's shortcuts, Esc
    // handling and drops stand down while one is up (a sheet is a child
    // widget, not a modal window, so Qt does not block them by itself).
    static int openCount();

    // Convenience: a confirm dialog. Returns true when the action button was chosen.
    // `format`: Qt::AutoText = as addText() decides, Qt::PlainText, Qt::RichText.
    static bool confirm(QWidget* host, const QString& title, const QString& text, const QString& safeText,
                        const QString& actionText, bool destructive = true, Icon icon = Icon::Warning,
                        const QString& checkboxText = {}, bool* checkboxValue = nullptr,
                        Qt::TextFormat format = Qt::AutoText);
    static void message(QWidget* host, const QString& title, const QString& text, Icon icon = Icon::Info,
                        const QString& buttonText = QStringLiteral("OK"), Qt::TextFormat format = Qt::AutoText);
    // A message that does not block (no nested event loop): for handlers that
    // must return at once. `text` is shown literally. The sheet deletes itself.
    static Sheet* notify(QWidget* host, const QString& title, const QString& text, Icon icon = Icon::Info,
                         const QString& buttonText = QStringLiteral("OK"));
    // Markup for a value in the mono font; `text` is plain and is escaped here.
    static QString monoSpan(const QString& text);
    // The same for several plain lines: each is escaped, one line per row.
    static QString monoLines(const QStringList& plainLines);

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool focusNextPrevChild(bool next) override;

private:
    void relayout();
    QLabel* addBodyLabel(const QString& text, Qt::TextFormat format);
    QPushButton* defaultButton() const; // Enter outside a button; focused when the sheet opens
    QWidget* m_panel;
    QVBoxLayout* m_body;
    QHBoxLayout* m_buttons;
    QLabel* m_title;
    QList<QPushButton*> m_buttonList;
    QList<Role> m_roles;
    int m_width;
    int m_result = -1;
    bool m_answered = false; // close() ran (exec(): the loop did not just end with the application)
    bool m_counted = false; // in openCount()
    QPointer<QEventLoop> m_loop;
    QPointer<QWidget> m_previousFocus;
    bool m_previousByKeyboard = false;
};

// Bottom-right toast stack (max 3, newest at the bottom).
class ToastHost : public QWidget {
    Q_OBJECT
public:
    explicit ToastHost(QWidget* host);
    struct Action {
        QString text;
        std::function<void()> fn;
    };
    void show(Tone tone, const QString& title, const QString& body, const QList<Action>& actions = {},
              int lifetimeMs = -1); // -1: 6 s for Ok/Info, sticky for Warn/Err
    int count() const { return int(m_toasts.size()); }
    void clear();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void relayout();
    void remove(QWidget* toast);
    std::vector<QWidget*> m_toasts;
};

} // namespace fw::ui
