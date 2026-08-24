#pragma once

// Entry point for --tui mode: an htop-style, keyboard-driven terminal UI
// built on FTXUI, running the SAME backend collectors as the graphical
// mode (see src/backend/) -- only the view layer differs. Blocks until
// the user quits (q); returns a process exit code.
int runTuiApp(bool enableBandwidthTracking);
