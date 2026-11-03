
#include <QApplication>

#include "browser_window.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QApplication::setApplicationName("AAFR Browser");
    QApplication::setOrganizationName("AAFR");

    BrowserWindow window;
    window.show();

    return app.exec();
}
