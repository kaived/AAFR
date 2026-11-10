#pragma once

#include <QFrame>
#include <QIcon>
#include <QList>
#include <QString>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QToolButton;

// Chrome-style tab search: a popup with a filter field, the open tabs and
// the recently closed ones. It only displays entries and reports choices;
// BrowserWindow owns the tabs and decides what each choice does.
class TabSearchPopup final : public QFrame
{
    Q_OBJECT

public:
    struct OpenTab {
        int index = -1;
        QIcon icon;
        QString title;
        QString subtitle;  // Site or page kind, e.g. "postiz.com".
        QString when;      // "Current tab", "4 mins ago", ...
        bool current = false;
    };
    struct ClosedTab {
        int id = -1;
        QIcon icon;
        QString title;
        QString subtitle;
        QString when;
    };

    explicit TabSearchPopup(QWidget *parent = nullptr);

    void setEntries(const QList<OpenTab> &open, const QList<ClosedTab> &closed);
    // Shows the popup with its top-left corner at globalPos and focuses search.
    void popupAt(const QPoint &globalPos);

    QLineEdit *searchField() const { return search_; }
    QListWidget *list() const { return list_; }
    bool recentlyClosedExpanded() const { return closedExpanded_; }
    void setRecentlyClosedExpanded(bool expanded);

    // "just now", "1 min ago", "5 mins ago", "2 hours ago", "3 days ago".
    static QString relativeTime(qint64 secondsAgo);

signals:
    void activateTab(int index);
    void closeTab(int index);
    void reopenClosedTab(int id);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    enum Role { KindRole = Qt::UserRole + 1, ValueRole, SearchTextRole };
    enum Kind { HeaderKind, OpenKind, ClosedKind, EmptyKind };

    void rebuild();
    void applyFilter();
    void moveSelection(int step);
    void activate(QListWidgetItem *item);
    QListWidgetItem *addHeader(const QString &text, bool collapsible);
    QListWidgetItem *addRow(Kind kind, int value, const QIcon &icon, const QString &title,
                            const QString &detail, bool closable);

    QLineEdit *search_ = nullptr;
    QListWidget *list_ = nullptr;
    QToolButton *collapseButton_ = nullptr;
    QList<OpenTab> open_;
    QList<ClosedTab> closed_;
    bool closedExpanded_ = true;
};
