#include "tab_search_popup.h"

#include "browser_theme.h"

#include <QEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

namespace {
constexpr int kWidth = 360;
constexpr int kMaxListHeight = 420;

// A row's close button appears only while the pointer is over the row.
class RowWidget final : public QWidget
{
public:
    using QWidget::QWidget;
    QToolButton *closeButton = nullptr;

protected:
    void enterEvent(QEnterEvent *) override { if (closeButton) closeButton->setVisible(true); }
    void leaveEvent(QEvent *) override { if (closeButton) closeButton->setVisible(false); }
};
}

TabSearchPopup::TabSearchPopup(QWidget *parent) : QFrame(parent, Qt::Popup)
{
    setObjectName("tabSearchPopup");
    // Transparent outer window so the inner panel can have rounded corners.
    setAttribute(Qt::WA_TranslucentBackground);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *panel = new QFrame(this);
    panel->setObjectName("tabSearchPanel");
    outer->addWidget(panel);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(0, 0, 0, 8);
    layout->setSpacing(0);

    auto *searchRow = new QWidget(panel);
    searchRow->setObjectName("tabSearchRow");
    auto *searchLayout = new QHBoxLayout(searchRow);
    searchLayout->setContentsMargins(16, 10, 16, 10);
    searchLayout->setSpacing(12);
    auto *searchIcon = new QLabel(searchRow);
    searchIcon->setPixmap(BrowserTheme::icon(BrowserTheme::Icon::Search).pixmap(18, 18));
    search_ = new QLineEdit(searchRow);
    search_->setObjectName("tabSearchField");
    search_->setPlaceholderText(tr("Search Tabs"));
    search_->setAccessibleName(tr("Search tabs"));
    search_->setFrame(false);
    auto *hint = new QLabel(tr("Ctrl+Shift+A"), searchRow);
    hint->setObjectName("tabSearchHint");
    searchLayout->addWidget(searchIcon);
    searchLayout->addWidget(search_, 1);
    searchLayout->addWidget(hint);
    layout->addWidget(searchRow);
    auto *rule = new QFrame(panel);
    rule->setObjectName("tabSearchRule");
    rule->setFixedHeight(1);
    layout->addWidget(rule);

    list_ = new QListWidget(panel);
    list_->setObjectName("tabSearchList");
    list_->setFrameShape(QFrame::NoFrame);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setFocusPolicy(Qt::NoFocus);
    list_->setMouseTracking(true);
    list_->setUniformItemSizes(false);
    layout->addWidget(list_);
    setFixedWidth(kWidth);

    search_->installEventFilter(this);
    connect(search_, &QLineEdit::textChanged, this, &TabSearchPopup::applyFilter);
    connect(list_, &QListWidget::itemClicked, this, &TabSearchPopup::activate);
}

QString TabSearchPopup::relativeTime(qint64 secondsAgo)
{
    if (secondsAgo < 60) return tr("just now");
    const qint64 minutes = secondsAgo / 60;
    if (minutes < 60) return minutes == 1 ? tr("1 min ago") : tr("%1 mins ago").arg(minutes);
    const qint64 hours = minutes / 60;
    if (hours < 24) return hours == 1 ? tr("1 hour ago") : tr("%1 hours ago").arg(hours);
    const qint64 days = hours / 24;
    return days == 1 ? tr("1 day ago") : tr("%1 days ago").arg(days);
}

void TabSearchPopup::setEntries(const QList<OpenTab> &open, const QList<ClosedTab> &closed)
{
    open_ = open;
    closed_ = closed;
    rebuild();
}

void TabSearchPopup::setRecentlyClosedExpanded(bool expanded)
{
    closedExpanded_ = expanded;
    if (collapseButton_) {
        collapseButton_->setIcon(BrowserTheme::icon(expanded ? BrowserTheme::Icon::ChevronUp
                                                             : BrowserTheme::Icon::ChevronDown));
        collapseButton_->setToolTip(expanded ? tr("Hide recently closed") : tr("Show recently closed"));
    }
    applyFilter();
}

void TabSearchPopup::popupAt(const QPoint &globalPos)
{
    search_->clear();
    applyFilter();
    adjustSize();
    QPoint pos = globalPos;
    if (const QScreen *screen = QGuiApplication::screenAt(globalPos)) {
        const QRect area = screen->availableGeometry();
        pos.setX(qBound(area.left(), pos.x(), area.right() - width()));
    }
    move(pos);
    show();
    search_->setFocus();
}

QListWidgetItem *TabSearchPopup::addHeader(const QString &text, bool collapsible)
{
    auto *item = new QListWidgetItem(list_);
    item->setFlags(Qt::NoItemFlags);
    item->setData(KindRole, HeaderKind);
    auto *row = new QWidget(list_);
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(16, 10, 10, 4);
    auto *label = new QLabel(text, row);
    label->setObjectName("tabSearchSection");
    layout->addWidget(label, 1);
    if (collapsible) {
        collapseButton_ = new QToolButton(row);
        collapseButton_->setObjectName("tabSearchCollapse");
        collapseButton_->setAutoRaise(true);
        collapseButton_->setFocusPolicy(Qt::NoFocus);
        connect(collapseButton_, &QToolButton::clicked, this,
                [this] { setRecentlyClosedExpanded(!closedExpanded_); });
        layout->addWidget(collapseButton_);
    }
    item->setSizeHint(QSize(kWidth, 36));
    list_->setItemWidget(item, row);
    return item;
}

QListWidgetItem *TabSearchPopup::addRow(Kind kind, int value, const QIcon &icon, const QString &title,
                                        const QString &detail, bool closable)
{
    auto *item = new QListWidgetItem(list_);
    item->setData(KindRole, kind);
    item->setData(ValueRole, value);
    item->setData(SearchTextRole, (title + QLatin1Char(' ') + detail).toLower());
    item->setToolTip(title);
    auto *row = new RowWidget(list_);
    row->setObjectName("tabSearchEntry");
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(16, 6, 10, 6);
    layout->setSpacing(12);
    auto *iconBox = new QLabel(row);
    iconBox->setObjectName("tabSearchIconBox");
    iconBox->setFixedSize(36, 36);
    iconBox->setAlignment(Qt::AlignCenter);
    iconBox->setPixmap(icon.pixmap(16, 16));
    layout->addWidget(iconBox);
    auto *text = new QVBoxLayout;
    text->setSpacing(1);
    auto *titleLabel = new QLabel(row);
    titleLabel->setObjectName("tabSearchTitle");
    titleLabel->setText(fontMetrics().elidedText(title, Qt::ElideRight, kWidth - 130));
    auto *detailLabel = new QLabel(row);
    detailLabel->setObjectName("tabSearchDetail");
    detailLabel->setText(fontMetrics().elidedText(detail, Qt::ElideRight, kWidth - 130));
    text->addWidget(titleLabel);
    text->addWidget(detailLabel);
    layout->addLayout(text, 1);
    if (closable) {
        auto *close = new QToolButton(row);
        close->setObjectName("tabSearchClose");
        close->setIcon(BrowserTheme::icon(BrowserTheme::Icon::Close));
        close->setIconSize(QSize(14, 14));
        close->setFixedSize(24, 24);
        close->setAutoRaise(true);
        close->setFocusPolicy(Qt::NoFocus);
        close->setAccessibleName(tr("Close tab"));
        close->setToolTip(tr("Close tab"));
        close->setVisible(false);
        // Keeps the row width stable while the button is hidden.
        QSizePolicy policy = close->sizePolicy();
        policy.setRetainSizeWhenHidden(true);
        close->setSizePolicy(policy);
        connect(close, &QToolButton::clicked, this, [this, value] { emit closeTab(value); });
        layout->addWidget(close);
        row->closeButton = close;
    }
    item->setSizeHint(QSize(kWidth, 50));
    list_->setItemWidget(item, row);
    return item;
}

void TabSearchPopup::rebuild()
{
    const int selectedKind = list_->currentItem() ? list_->currentItem()->data(KindRole).toInt() : -1;
    const int selectedValue = list_->currentItem() ? list_->currentItem()->data(ValueRole).toInt() : -1;
    list_->clear();
    collapseButton_ = nullptr;

    addHeader(tr("Open Tabs"), false);
    for (const OpenTab &tab : std::as_const(open_)) {
        addRow(OpenKind, tab.index, tab.icon, tab.title,
               tab.subtitle + QStringLiteral(" • ") + tab.when, true);
    }
    if (!closed_.isEmpty()) {
        addHeader(tr("Recently Closed"), true);
        for (const ClosedTab &tab : std::as_const(closed_))
            addRow(ClosedKind, tab.id, tab.icon, tab.title,
                   tab.subtitle + QStringLiteral(" • ") + tab.when, false);
    }
    auto *empty = new QListWidgetItem(tr("No matching tabs"), list_);
    empty->setFlags(Qt::NoItemFlags);
    empty->setData(KindRole, EmptyKind);
    empty->setSizeHint(QSize(kWidth, 40));
    empty->setTextAlignment(Qt::AlignCenter);
    setRecentlyClosedExpanded(closedExpanded_);  // Sets the icon and filters.

    // Keep the previous selection when the list is rebuilt while open.
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem *item = list_->item(i);
        if (!item->isHidden() && item->data(KindRole).toInt() == selectedKind
            && item->data(ValueRole).toInt() == selectedValue) {
            list_->setCurrentItem(item);
            break;
        }
    }
}

void TabSearchPopup::applyFilter()
{
    const QString needle = search_->text().trimmed().toLower();
    QListWidgetItem *header = nullptr;
    bool headerHasRows = false;
    bool anyRow = false;
    QListWidgetItem *empty = nullptr;
    auto closeSection = [&] {
        if (header) header->setHidden(!headerHasRows && !needle.isEmpty());
    };
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem *item = list_->item(i);
        const int kind = item->data(KindRole).toInt();
        if (kind == HeaderKind) {
            closeSection();
            header = item;
            headerHasRows = false;
            continue;
        }
        if (kind == EmptyKind) {
            empty = item;
            continue;
        }
        bool visible = needle.isEmpty() || item->data(SearchTextRole).toString().contains(needle);
        // Collapsing hides closed tabs, except while searching.
        if (kind == ClosedKind && !closedExpanded_ && needle.isEmpty()) visible = false;
        item->setHidden(!visible);
        headerHasRows = headerHasRows || visible;
        anyRow = anyRow || visible;
    }
    closeSection();
    if (empty) empty->setHidden(anyRow);

    // Select the first visible entry if the current one is gone.
    QListWidgetItem *current = list_->currentItem();
    if (!current || current->isHidden() || !(current->flags() & Qt::ItemIsSelectable)) {
        list_->setCurrentItem(nullptr);
        moveSelection(1);
    }

    int height = 4;
    for (int i = 0; i < list_->count(); ++i)
        if (!list_->item(i)->isHidden()) height += list_->item(i)->sizeHint().height();
    list_->setFixedHeight(qBound(48, height, kMaxListHeight));
    adjustSize();
}

void TabSearchPopup::moveSelection(int step)
{
    const int count = list_->count();
    int row = list_->currentItem() ? list_->currentRow() : (step > 0 ? -1 : count);
    for (int i = 0; i < count; ++i) {
        row += step;
        if (row < 0 || row >= count) return;
        QListWidgetItem *item = list_->item(row);
        if (!item->isHidden() && (item->flags() & Qt::ItemIsSelectable)) {
            list_->setCurrentItem(item);
            list_->scrollToItem(item);
            return;
        }
    }
}

void TabSearchPopup::activate(QListWidgetItem *item)
{
    if (!item || !(item->flags() & Qt::ItemIsSelectable)) return;
    const int value = item->data(ValueRole).toInt();
    const int kind = item->data(KindRole).toInt();
    hide();
    if (kind == OpenKind) emit activateTab(value);
    else if (kind == ClosedKind) emit reopenClosedTab(value);
}

bool TabSearchPopup::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == search_ && event->type() == QEvent::KeyPress) {
        switch (static_cast<QKeyEvent *>(event)->key()) {
        case Qt::Key_Down: moveSelection(1); return true;
        case Qt::Key_Up: moveSelection(-1); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: activate(list_->currentItem()); return true;
        default: break;
        }
    }
    return QFrame::eventFilter(watched, event);
}
