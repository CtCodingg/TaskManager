#pragma once

// Applies the same dark palette used in the Qt edition's style.qss /
// UiTheme.h, translated to ImGui's ImGuiStyle color/spacing system.
namespace Theme {
void apply();

// Health-level colors, matching UiTheme::colorForPercent() from the Qt
// edition -- used to tint text/bars based on load.
struct Rgb { float r, g, b; };
Rgb colorForPercent(double percent);
Rgb accentCpu();
Rgb accentMemory();
Rgb accentGpu();
Rgb accentDisk();
Rgb accentNetRx();
Rgb accentNetTx();
}
