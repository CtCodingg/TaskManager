#include "MainWindow.h"

#include <QApplication>
#include <QStyleFactory>
#include <QPalette>
#include <QColor>
#include <QFile>
#include <QTextStream>
#include <QFont>

namespace {

// Builds the dark QPalette that backs the .qss theme. Setting this on the
// QApplication (in addition to the stylesheet) ensures widgets/dialogs that
// QSS doesn't fully reach -- native color pickers, some message box chrome,
// disabled-state colors -- still render correctly in dark mode instead of
// falling back to the OS's light palette.
QPalette buildDarkPalette() {
    QPalette p;

    const QColor bgBase("#0d1117");
    const QColor bgSurface("#131822");
    const QColor bgElevated("#1a2029");
    const QColor border("#262c38");
    const QColor textPrimary("#e7eaf0");
    const QColor textSecondary("#939bb0");
    const QColor textTertiary("#5f6a80");
    const QColor accent("#38bdf8");

    p.setColor(QPalette::Window, bgBase);
    p.setColor(QPalette::WindowText, textPrimary);
    p.setColor(QPalette::Base, bgSurface);
    p.setColor(QPalette::AlternateBase, bgElevated);
    p.setColor(QPalette::ToolTipBase, bgElevated);
    p.setColor(QPalette::ToolTipText, textPrimary);
    p.setColor(QPalette::Text, textPrimary);
    p.setColor(QPalette::PlaceholderText, textTertiary);
    p.setColor(QPalette::Button, bgElevated);
    p.setColor(QPalette::ButtonText, textPrimary);
    p.setColor(QPalette::BrightText, Qt::white);
    p.setColor(QPalette::Link, accent);
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, QColor("#0b0f14"));

    p.setColor(QPalette::Disabled, QPalette::WindowText, textTertiary);
    p.setColor(QPalette::Disabled, QPalette::Text, textTertiary);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, textTertiary);

    Q_UNUSED(border);
    Q_UNUSED(textSecondary);
    return p;
}

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("TaskManager");
    QApplication::setOrganizationName("TaskManager");

    // Fusion style renders consistently across Linux/Windows/Jetson desktops
    // and is the style QSS themes correctly on all three.
    QApplication::setStyle(QStyleFactory::create("Fusion"));
    QApplication::setPalette(buildDarkPalette());

    // Clean UI sans-serif; Qt falls back to a sensible system default on
    // platforms where "Segoe UI" isn't installed (e.g. Linux/Jetson).
    QFont appFont("Segoe UI", 9);
    QApplication::setFont(appFont);

    QFile styleFile(":/style.qss");
    if (styleFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream stream(&styleFile);
        app.setStyleSheet(stream.readAll());
    }

    MainWindow window;
    window.show();

    return app.exec();
}
