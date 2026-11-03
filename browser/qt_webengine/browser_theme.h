#pragma once

#include <QIcon>
#include <QString>

namespace BrowserTheme {
enum class Icon { Back, Forward, Reload, Stop, Home, Plus, Close, Search, Brand };
QIcon icon(Icon symbol);
QString styleSheet();
}
