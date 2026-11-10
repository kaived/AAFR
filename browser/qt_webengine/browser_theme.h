#pragma once

#include <QColor>
#include <QIcon>
#include <QString>

namespace BrowserTheme {
enum class Icon { Back, Forward, Reload, Stop, Home, Plus, Close, Search, Brand, DevTools, Globe,
                  // Window caption buttons, drawn at 10 px like Windows' own.
                  Minimize, Maximize, Restore, WindowClose,
                  // Tab search button and section toggles.
                  ChevronDown, ChevronUp };
QIcon icon(Icon symbol);
// Tab title colour: active or hovered tabs use the primary text colour.
QColor tabTextColor(bool emphasized);
enum class TabColor { Active, Hover, Separator, Focus };
QColor tabColor(TabColor role);
QString styleSheet();
}
