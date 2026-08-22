#include "MainWindow.h"

#include <QApplication>
#include <QStyleFactory>
#include <QPalette>
#include <QColor>
#include <QFile>
#include <QTextStream>
#include <QFont>
#include <QString>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#include <shellapi.h>
#endif

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

// The one command-line flag this app recognizes: opts into the
// per-process Bandwidth tab (see ProcessBandwidthCollector). Deliberately
// NOT the default, since on Windows it requires elevation (a UAC prompt)
// -- see relaunchElevated() below. Everything else about normal operation
// (Processes/Performance/Network/Connections tabs) never needs admin
// rights, on either platform.
constexpr const char* kTrackBandwidthFlag = "--track-bandwidth";

bool parseTrackBandwidthFlag(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == kTrackBandwidthFlag) return true;
    }
    return false;
}

#ifdef Q_OS_WIN
bool isProcessElevated() {
    bool elevated = false;
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        TOKEN_ELEVATION elevation{};
        DWORD size = sizeof(elevation);
        if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
            elevated = elevation.TokenIsElevated != 0;
        }
        CloseHandle(token);
    }
    return elevated;
}

// Relaunches the current executable elevated (triggers a UAC prompt) with
// the same command-line arguments. Returns true if the elevated instance
// was launched successfully -- the caller should exit(0) immediately
// afterward so there's never two copies running side by side. Returns
// false if the user cancelled the UAC prompt or elevation otherwise
// failed; the caller should then fall back to running normally without
// the flag's feature rather than refusing to start at all.
bool relaunchElevated(int argc, char* argv[]) {
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;

    QString args;
    for (int i = 1; i < argc; ++i) {
        args += QString::fromLocal8Bit(argv[i]);
        if (i + 1 < argc) args += ' ';
    }
    std::wstring argsW = args.toStdWString();

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = argsW.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        return false; // most commonly: user clicked "No" on the UAC prompt
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}
#endif

} // namespace

int main(int argc, char* argv[]) {
    bool trackBandwidth = parseTrackBandwidthFlag(argc, argv);

#ifdef Q_OS_WIN
    // The Bandwidth tab's Windows backend (TCP Extended Statistics API)
    // requires admin rights. Only ask for elevation when the flag was
    // actually passed -- default launches never see a UAC prompt.
    if (trackBandwidth && !isProcessElevated()) {
        if (relaunchElevated(argc, argv)) {
            return 0; // the elevated instance takes over; this one exits quietly
        }
        // Elevation was cancelled or failed -- still start normally, just
        // without bandwidth tracking, rather than not starting at all.
        trackBandwidth = false;
    }
#endif

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

    MainWindow window(trackBandwidth);
    window.show();

    return app.exec();
}
