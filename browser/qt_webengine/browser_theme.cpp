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
const QString error = QStringLiteral("#E57373");
}

QIcon BrowserTheme::icon(Icon symbol)
{
    QIcon result;
    for (int logicalSize : (symbol == Icon::Brand ? QList<int>{20, 48} : QList<int>{20})) {
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
QWidget#tabHeader { background: @strip; border-bottom: 1px solid @border; }
QTabWidget#browserTabs::pane { border: 0; }
QTabBar#tabStrip { background: @strip; }
QTabBar#tabStrip::tab { background: @strip; color: @secondary; padding: 0 10px; height: 40px;
    border-top: 2px solid transparent; border-right: 1px solid @strip; }
QTabBar#tabStrip::tab:selected { background: @active; color: @text; border-top-color: @accent; }
QTabBar#tabStrip::tab:!selected:hover { background: @hover; color: @text; }
QTabBar#tabStrip:focus { border-bottom: 1px solid @accent; }
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
QTabBar#tabStrip QToolButton#tabCloseButton { background: transparent; border: 1px solid transparent;
    padding: 2px; border-radius: 4px; }
QTabBar#tabStrip QToolButton#tabCloseButton:hover { background: @hover; }
QTabBar#tabStrip QToolButton#tabCloseButton:focus { border-color: @accent; }
QLineEdit#addressBar, QLineEdit#newTabSearch { background: @field; color: @text;
    border: 1px solid @border; border-radius: 10px; min-height: 34px; padding: 0 12px;
    selection-background-color: @accent; selection-color: @window; }
QLineEdit#addressBar:focus, QLineEdit#newTabSearch:focus { border-color: @accent; }
QLineEdit#addressBar QToolButton, QLineEdit#newTabSearch QToolButton {
    background: transparent; border: 0; padding: 0; }
QWidget#newTabPage { background: @window; }
QLabel#newTabEyebrow { color: @accent; font-weight: 600; letter-spacing: 4px; }
QLabel#newTabHeading { color: @text; font-size: 30px; font-weight: 600; }
QLabel#newTabSubtitle { color: @secondary; font-size: 14px; }
QLabel#newTabHint { color: @muted; font-size: 12px; }
QStatusBar { background: @strip; color: @secondary; border-top: 1px solid @border; font-size: 12px; }
QStatusBar::item { border: 0; }
QProgressBar#loadProgress { background: @field; border: 0; border-radius: 2px; max-height: 3px; }
QProgressBar#loadProgress::chunk { background: @accent; border-radius: 2px; }
QToolTip { background: @active; color: @text; border: 1px solid @border; padding: 5px; }
)");
    for (const auto &token : {qMakePair("@window", window), qMakePair("@strip", strip),
             qMakePair("@active", active), qMakePair("@toolbar", toolbar), qMakePair("@field", field),
             qMakePair("@accent", accent), qMakePair("@text", text), qMakePair("@secondary", secondary),
             qMakePair("@muted", muted), qMakePair("@border", border), qMakePair("@hover", hover),
             qMakePair("@error", error)})
        qss.replace(QLatin1String(token.first), token.second);
    return qss;
}
