#pragma once

#include <QColor>
#include <QString>

// Central color palette for the dark UI theme. Kept in sync by hand with
// resources/style.qss -- this header is for colors needed at runtime in
// C++ (chart series, dynamically-colored progress bars, table cell text),
// the .qss covers static widget chrome (borders, hover states, etc).
namespace UiTheme {

// --- Base surfaces -----------------------------------------------------
inline QColor bgBase()      { return QColor("#0d1117"); }
inline QColor bgSurface()   { return QColor("#131822"); }
inline QColor bgElevated()  { return QColor("#1a2029"); }
inline QColor border()      { return QColor("#262c38"); }

// --- Text ---------------------------------------------------------------
inline QColor textPrimary()   { return QColor("#e7eaf0"); }
inline QColor textSecondary() { return QColor("#939bb0"); }
inline QColor textTertiary()  { return QColor("#5f6a80"); }

// --- Brand / selection accent --------------------------------------------
inline QColor accentBrand() { return QColor("#38bdf8"); }

// --- Per-metric identity colors (used consistently for a given metric's
// card accent stripe + its chart series color) --------------------------
inline QColor accentCpu()    { return QColor("#22d3ee"); } // cyan
inline QColor accentMemory() { return QColor("#a78bfa"); } // violet
inline QColor accentGpu()    { return QColor("#fbbf24"); } // amber
inline QColor accentDisk()   { return QColor("#60a5fa"); } // blue
inline QColor accentNetRx()  { return QColor("#34d399"); } // green (download)
inline QColor accentNetTx()  { return QColor("#fb923c"); } // orange (upload)

// --- Health / status levels (used for progress bar fill + warning text) -
inline QColor levelGood()     { return QColor("#34d399"); }
inline QColor levelWarn()     { return QColor("#fbbf24"); }
inline QColor levelCritical() { return QColor("#f87171"); }

// Maps a 0-100 percentage to a health color.
inline QColor colorForPercent(double percent) {
    if (percent >= 85.0) return levelCritical();
    if (percent >= 60.0) return levelWarn();
    return levelGood();
}

// Maps a 0-100 percentage to the "level" string used by the QSS
// QProgressBar[level="..."] selectors in style.qss.
inline QString levelNameForPercent(double percent) {
    if (percent >= 85.0) return "critical";
    if (percent >= 60.0) return "warn";
    return "good";
}

} // namespace UiTheme
