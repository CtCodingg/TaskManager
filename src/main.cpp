#include "MainWindow.h"

#include <QApplication>
#include <QStyleFactory>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("TaskManager");
    QApplication::setOrganizationName("TaskManager");

    // Fusion style renders consistently across Linux/Windows/Jetson desktops
    QApplication::setStyle(QStyleFactory::create("Fusion"));

    MainWindow window;
    window.show();

    return app.exec();
}
