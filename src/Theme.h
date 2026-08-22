#pragma once

// Applies the app's dark palette to ImGui's ImGuiStyle color/spacing system.
namespace Theme {
void apply();

// Health-level colors -- used to tint text/bars based on load.
struct Rgb { float r, g, b; };
Rgb colorForPercent(double percent);
Rgb accentCpu();
Rgb accentMemory();
Rgb accentGpu();
Rgb accentDisk();
Rgb accentNetRx();
Rgb accentNetTx();
}
