#include "browser_theme.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace {
// All chrome colors live here. Webpage content never receives this stylesheet.
const QString window = QStringLiteral("#111820");
const QString strip = QStringLiteral("#17212B");
const QString active = QStringLiteral("#253440");
const QString toolbar = QStringLiteral("#1B2732");
const QString field = QStringLiteral("#101A23");
const QString accent = QStringLiteral("#36C6BC");
const QString text = QStringLiteral("#F0F5F7");
const QString secondary = QStringLiteral("#A5B5C1");
const QString muted = QStringLiteral("#8294A2");
const QString border = QStringLiteral("#354652");
const QString hover = QStringLiteral("#30424E");
const QString tabHover = QStringLiteral("#151E27");
const QString titleStrip = QStringLiteral("#0C1218");
const QString error = QStringLiteral("#E57373");
}

QColor BrowserTheme::tabTextColor(bool emphasized)
{
    return QColor(emphasized ? text : secondary);
}

QColor BrowserTheme::tabColor(TabColor role)
{
    switch (role) {
    case TabColor::Active: return QColor(toolbar);  // Same as the toolbar it joins.
    case TabColor::Hover: return QColor(tabHover);
    case TabColor::Separator: return QColor(border);
    case TabColor::Focus: return QColor(accent);
    }
    return QColor(toolbar);
}

QIcon BrowserTheme::icon(Icon symbol)
{
    QIcon result;
    // Tab icons are drawn at 16 px; the brand mark also appears at 48 px.
    const bool caption = symbol == Icon::Minimize || symbol == Icon::Maximize
                         || symbol == Icon::Restore || symbol == Icon::WindowClose;
    const QList<int> sizes = symbol == Icon::Brand ? QList<int>{16, 20, 48}
                             : symbol == Icon::Globe ? QList<int>{16, 20}
                             : caption               ? QList<int>{10}
                                                     : QList<int>{20};
    for (int logicalSize : sizes) {
      for (int scale : {1, 2, 3}) {
        for (auto mode : {QIcon::Normal, QIcon::Disabled}) {
            QPixmap pixmap(logicalSize * scale, logicalSize * scale);
            pixmap.setDevicePixelRatio(scale);
            pixmap.fill(Qt::transparent);
            QPainter p(&pixmap);
            p.scale(logicalSize / 20.0, logicalSize / 20.0);
            p.setRenderHint(QPainter::Antialiasing);
            p.setPen(QPen(QColor(mode == QIcon::Disabled ? muted
                                      : symbol == Icon::Brand ? accent : text),
                          1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            if (mode == QIcon::Disabled) p.setOpacity(0.5);
            // Caption glyphs are drawn on the 20-unit grid at half size, so a
            // 2-unit pen gives Windows' 1 px hairline.
            if (caption) p.setPen(QPen(QColor(text), 2.0, Qt::SolidLine, Qt::SquareCap, Qt::MiterJoin));
            switch (symbol) {
            case Icon::Back:
            case Icon::Forward: {
                if (symbol == Icon::Forward) { p.translate(20, 0); p.scale(-1, 1); }
                p.drawLine(QPointF(15, 10), QPointF(5, 10));
                p.drawPolyline(QPolygonF{QPointF(9, 6), QPointF(5, 10), QPointF(9, 14)});
                break;
            }
            case Icon::Reload:
                p.drawArc(QRectF(4, 4, 12, 12), 40 * 16, 290 * 16);
                p.drawPolyline(QPolygonF{QPointF(16, 3), QPointF(16, 7), QPointF(12, 7)});
                break;
            case Icon::Stop: p.drawRoundedRect(QRectF(5, 5, 10, 10), 1, 1); break;
            case Icon::Close:
                p.drawLine(QPointF(6, 6), QPointF(14, 14));
                p.drawLine(QPointF(6, 14), QPointF(14, 6));
                break;
            case Icon::Home:
                p.drawPolyline(QPolygonF{QPointF(3, 9), QPointF(10, 3), QPointF(17, 9)});
                p.drawPolyline(QPolygonF{QPointF(5, 8), QPointF(5, 16), QPointF(15, 16), QPointF(15, 8)});
                p.drawRect(QRectF(8, 11, 4, 5));
                break;
            case Icon::Plus:
                p.drawLine(QPointF(5, 10), QPointF(15, 10));
                p.drawLine(QPointF(10, 5), QPointF(10, 15));
                break;
            case Icon::Search:
                p.drawEllipse(QRectF(4, 3, 9, 9));
                p.drawLine(QPointF(12, 11), QPointF(17, 16));
                break;
            case Icon::Brand:
                p.drawRoundedRect(QRectF(3, 3, 14, 14), 4, 4);
                p.drawPolyline(QPolygonF{QPointF(6, 13), QPointF(10, 7), QPointF(14, 13)});
                p.drawLine(QPointF(8, 11), QPointF(12, 11));
                break;
            case Icon::Globe:
                // Default page icon, as Chrome uses for sites without a favicon.
                p.setPen(QPen(QColor(mode == QIcon::Disabled ? muted : secondary), 1.5));
                p.drawEllipse(QRectF(3, 3, 14, 14));
                p.drawEllipse(QRectF(7, 3, 6, 14));
                p.drawLine(QPointF(3, 10), QPointF(17, 10));
                break;
            case Icon::ChevronDown:
                p.drawPolyline(QPolygonF{QPointF(5, 8), QPointF(10, 13), QPointF(15, 8)});
                break;
            case Icon::ChevronUp:
                p.drawPolyline(QPolygonF{QPointF(5, 12), QPointF(10, 7), QPointF(15, 12)});
                break;
            case Icon::Minimize:
                p.drawLine(QPointF(1, 10), QPointF(19, 10));
                break;
            case Icon::Maximize:
                p.drawRect(QRectF(1, 1, 18, 18));
                break;
            case Icon::Restore:
                p.drawRect(QRectF(1, 5, 14, 14));
                p.drawPolyline(QPolygonF{QPointF(5, 5), QPointF(5, 1), QPointF(19, 1),
                                         QPointF(19, 15), QPointF(15, 15)});
                break;
            case Icon::WindowClose:
                p.drawLine(QPointF(1, 1), QPointF(19, 19));
                p.drawLine(QPointF(1, 19), QPointF(19, 1));
                break;
            case Icon::DevTools:
                // A pulse trace inside a panel outline.
                p.drawRoundedRect(QRectF(2.5, 4, 15, 12), 2, 2);
                p.drawPolyline(QPolygonF{QPointF(5, 11), QPointF(7.5, 11), QPointF(9, 7.5),
                                         QPointF(11, 13.5), QPointF(12.5, 10), QPointF(15, 10)});
                break;
            }
            p.end();
            result.addPixmap(pixmap, mode);
        }
      }
    }
    return result;
}

QString BrowserTheme::styleSheet()
{
    QString qss = QStringLiteral(R"(
QMainWindow, QWidget#browserShell, QWidget#contentStack { background: @window; color: @text; }
/* The title-bar strip is darker than the toolbar, as in Chrome, so the active
   tab (toolbar colour) stands out. */
QWidget#tabHeader { background: @titleStrip; }
QTabWidget#browserTabs::pane { border: 0; }
QTabBar#tabStrip { background: @titleStrip; }
/* Tab metrics only: ChromeTabBar paints the shape (rounded top, curved feet,
   separators) itself. The 8 px at each side of a tab hold its feet. */
QTabBar#tabStrip::tab { background: transparent; color: @secondary; height: 34px; margin-top: 8px;
    padding: 0 14px 0 18px; border: 0; }
QTabBar#tabStrip QToolButton { background: @active; border: 1px solid @border;
    border-radius: 0; padding: 0; }
QToolBar#navigationToolbar { background: @toolbar; border: 0; border-bottom: 1px solid @border;
    padding: 6px 10px; spacing: 4px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 6px;
    padding: 5px; color: @text; }
QToolButton:hover { background: @hover; }
QToolButton:pressed { background: @active; }
QToolButton:focus { border-color: @accent; }
QToolButton:disabled { color: @muted; }
/* QTabBar centres the button in the whole 42 px tab, including the 8 px gap
   above the tab shape, and flush with the tab's right edge. The margins move
   the visible 20 px circle down by that gap and left out of the tab's foot,
   so it lines up with the icon and title. */
QTabBar#tabStrip QToolButton#tabCloseButton { background: transparent; border: 1px solid transparent;
    padding: 1px; border-radius: 10px; margin: 8px 16px 0 0; }
QTabBar#tabStrip QToolButton#tabCloseButton:hover { background: @hover; }
QWidget#tabHeader QToolButton#newTabButton { border-radius: 16px; margin-top: 6px; }
QWidget#tabHeader QToolButton#captionMinimize, QWidget#tabHeader QToolButton#captionMaximize,
QWidget#tabHeader QToolButton#captionClose { background: transparent; border: 0; border-radius: 0;
    padding: 0; }
QWidget#tabHeader QToolButton#captionMinimize:hover,
QWidget#tabHeader QToolButton#captionMaximize:hover { background: @hover; }
QWidget#tabHeader QToolButton#captionClose:hover { background: #C42B1C; }
QWidget#tabHeader QToolButton#captionClose:pressed { background: #A52618; }
QWidget#tabHeader QToolButton#tabSearchButton { border-radius: 8px; margin: 6px 2px 0 6px; padding: 2px; }
QFrame#tabSearchPanel { background: @toolbar; border: 1px solid @border; border-radius: 10px; }
QFrame#tabSearchPanel QWidget { background: transparent; color: @text; }
QFrame#tabSearchPanel QFrame#tabSearchRule { background: @border; }
QFrame#tabSearchPanel QLineEdit#tabSearchField { background: transparent; border: 0; font-size: 14px;
    selection-background-color: @accent; selection-color: @window; }
QFrame#tabSearchPanel QLabel#tabSearchHint, QFrame#tabSearchPanel QLabel#tabSearchDetail { color: @muted; }
QFrame#tabSearchPanel QLabel#tabSearchSection { color: @secondary; font-weight: 600; }
QFrame#tabSearchPanel QLabel#tabSearchIconBox { background: @field; border-radius: 8px; }
QFrame#tabSearchPanel QListWidget { outline: 0; }
QFrame#tabSearchPanel QListWidget::item { border: 0; }
QFrame#tabSearchPanel QListWidget::item:hover { background: @tabHover; }
QFrame#tabSearchPanel QListWidget::item:selected { background: @hover; }
QFrame#tabSearchPanel QToolButton { border: 0; border-radius: 12px; }
QFrame#tabSearchPanel QToolButton:hover { background: @active; }
QTabBar#tabStrip QToolButton#tabCloseButton:focus { border-color: @accent; }
QLineEdit#addressBar, QLineEdit#newTabSearch { background: @field; color: @text;
    border: 1px solid @border; border-radius: 10px; min-height: 34px; padding: 0 12px;
    selection-background-color: @accent; selection-color: @window; }
QLineEdit#addressBar:focus, QLineEdit#newTabSearch:focus { border-color: @accent; }
QLineEdit#addressBar QToolButton, QLineEdit#newTabSearch QToolButton {
    background: transparent; border: 0; padding: 0; }
QWidget#newTabPage { background: @window; }
QStatusBar { background: @strip; color: @secondary; border-top: 1px solid @border; font-size: 12px; }
QStatusBar::item { border: 0; }
QProgressBar#loadProgress { background: @field; border: 0; border-radius: 2px; max-height: 3px; }
QProgressBar#loadProgress::chunk { background: @accent; border-radius: 2px; }
QToolTip { background: @active; color: @text; border: 1px solid @border; padding: 5px; }
QToolButton#devToolsButton:checked { background: @active; border-color: @border; }
QDockWidget#devToolsDock { color: @text; font-size: 12px; }
QDockWidget#devToolsDock::title { background: @strip; padding: 5px 8px; border-top: 1px solid @border; }
QDockWidget#devToolsDock QWidget { background: @toolbar; color: @text; }
QDockWidget#devToolsDock QTabWidget::pane { border: 0; border-top: 1px solid @border; }
QDockWidget#devToolsDock QTabBar::tab { background: @toolbar; color: @secondary; padding: 5px 12px;
    border-bottom: 2px solid transparent; }
QDockWidget#devToolsDock QTabBar::tab:selected { color: @text; border-bottom-color: @accent; }
QDockWidget#devToolsDock QTabBar::tab:!selected:hover { color: @text; background: @hover; }
QDockWidget#devToolsDock QTableWidget { background: @field; alternate-background-color: @window;
    gridline-color: @border; border: 1px solid @border; selection-background-color: @active; }
QDockWidget#devToolsDock QHeaderView::section { background: @strip; color: @secondary; border: 0;
    border-right: 1px solid @border; border-bottom: 1px solid @border; padding: 3px 6px; }
QDockWidget#devToolsDock QLabel#runtimeBanner, QDockWidget#devToolsDock QLabel#decisionsBanner {
    color: @secondary; font-style: italic; }
QDockWidget#devToolsDock QLabel#networkScopeNote { color: @muted; font-size: 11px; }
QDockWidget#devToolsDock QPushButton, QDockWidget#devToolsDock QComboBox,
QDockWidget#devToolsDock QLineEdit { background: @field; color: @text; border: 1px solid @border;
    border-radius: 4px; padding: 3px 10px; }
QDockWidget#devToolsDock QPushButton:hover { background: @hover; }
QDockWidget#devToolsDock QPushButton:focus, QDockWidget#devToolsDock QLineEdit:focus,
QDockWidget#devToolsDock QComboBox:focus { border-color: @accent; }
QDockWidget#devToolsDock QPushButton:disabled, QDockWidget#devToolsDock QLineEdit:disabled { color: @muted; }
QMainWindow::separator { background: @border; width: 3px; height: 3px; }
QMainWindow::separator:hover { background: @accent; }
)");
    for (const auto &token : {qMakePair("@window", window), qMakePair("@strip", strip),
             qMakePair("@active", active), qMakePair("@toolbar", toolbar), qMakePair("@field", field),
             qMakePair("@accent", accent), qMakePair("@text", text), qMakePair("@secondary", secondary),
             qMakePair("@muted", muted), qMakePair("@border", border),
             qMakePair("@tabHover", tabHover), qMakePair("@titleStrip", titleStrip),
             qMakePair("@hover", hover),
             qMakePair("@error", error)})
        qss.replace(QLatin1String(token.first), token.second);
    return qss;
}
