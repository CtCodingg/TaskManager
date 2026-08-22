#include "Theme.h"
#include "imgui.h"

namespace Theme {

namespace {
ImVec4 hex(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}
}

void apply() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Dark palette.
    const ImVec4 bgBase      = hex(0x0d, 0x11, 0x17);
    const ImVec4 bgSurface   = hex(0x13, 0x18, 0x22);
    const ImVec4 bgElevated  = hex(0x1a, 0x20, 0x29);
    const ImVec4 bgHover     = hex(0x21, 0x28, 0x36);
    const ImVec4 border      = hex(0x26, 0x2c, 0x38);
    const ImVec4 textPrimary = hex(0xe7, 0xea, 0xf0);
    const ImVec4 textSecond  = hex(0x93, 0x9b, 0xb0);
    const ImVec4 accent      = hex(0x38, 0xbd, 0xf8);

    colors[ImGuiCol_WindowBg]         = bgBase;
    colors[ImGuiCol_ChildBg]          = bgSurface;
    colors[ImGuiCol_PopupBg]          = bgElevated;
    colors[ImGuiCol_Border]           = border;
    colors[ImGuiCol_FrameBg]          = bgElevated;
    colors[ImGuiCol_FrameBgHovered]   = bgHover;
    colors[ImGuiCol_FrameBgActive]    = bgHover;
    colors[ImGuiCol_TitleBg]          = bgBase;
    colors[ImGuiCol_TitleBgActive]    = bgBase;
    colors[ImGuiCol_MenuBarBg]        = bgElevated;
    colors[ImGuiCol_ScrollbarBg]      = bgBase;
    colors[ImGuiCol_ScrollbarGrab]    = border;
    colors[ImGuiCol_ScrollbarGrabHovered] = hex(0x33, 0x3b, 0x4a);
    colors[ImGuiCol_CheckMark]        = accent;
    colors[ImGuiCol_SliderGrab]       = accent;
    colors[ImGuiCol_Button]           = bgElevated;
    colors[ImGuiCol_ButtonHovered]    = bgHover;
    colors[ImGuiCol_ButtonActive]     = hex(0x16, 0x1c, 0x26);
    colors[ImGuiCol_Header]           = hex(0x1c, 0x3f, 0x52);
    colors[ImGuiCol_HeaderHovered]    = hex(0x22, 0x4a, 0x60);
    colors[ImGuiCol_HeaderActive]     = hex(0x22, 0x4a, 0x60);
    colors[ImGuiCol_Separator]        = border;
    // Active tab needs to clearly pop against both the tab bar (bgBase)
    // and the content area (bgSurface) -- bgSurface alone (the previous
    // value) was too close to bgBase to read as "selected" at a glance.
    colors[ImGuiCol_Tab]              = bgBase;
    colors[ImGuiCol_TabHovered]       = bgHover;
    colors[ImGuiCol_TabActive]        = bgElevated;
    colors[ImGuiCol_TabUnfocused]     = bgBase;
    colors[ImGuiCol_TabUnfocusedActive] = bgElevated;
    // Accent-colored line on the selected tab, marking it clearly.
    // Requires Dear ImGui >= 1.91.
    colors[ImGuiCol_TabSelectedOverline] = accent;
    colors[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(accent.x, accent.y, accent.z, 0.5f);
    colors[ImGuiCol_Text]             = textPrimary;
    colors[ImGuiCol_TextDisabled]     = textSecond;
    colors[ImGuiCol_TableHeaderBg]    = bgElevated;
    colors[ImGuiCol_TableBorderStrong] = border;
    colors[ImGuiCol_TableBorderLight] = hex(0x1e, 0x24, 0x2e);
    colors[ImGuiCol_TableRowBg]       = bgSurface;
    colors[ImGuiCol_TableRowBgAlt]    = hex(0x16, 0x1c, 0x26);
    colors[ImGuiCol_PlotLines]        = accent;
    colors[ImGuiCol_PlotHistogram]    = accent;

    style.WindowRounding = 8.0f;
    style.ChildRounding = 8.0f;
    style.FrameRounding = 6.0f;
    style.PopupRounding = 8.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.TabRounding = 6.0f;
    style.WindowPadding = ImVec2(12, 12);
    style.FramePadding = ImVec2(8, 6);
    style.ItemSpacing = ImVec2(8, 8);
    style.IndentSpacing = 16.0f;
    // Table row height/comfort: ImGui's default (~4,2) reads much tighter
    // than a comfortable table row. This is the main lever for readable
    // table rows -- taller cell padding, not font size alone.
    style.CellPadding = ImVec2(10, 8);
}

Rgb colorForPercent(double percent) {
    if (percent >= 85.0) return {0.973f, 0.443f, 0.443f}; // #f87171 critical
    if (percent >= 60.0) return {0.984f, 0.749f, 0.141f}; // #fbbf24 warn
    return {0.204f, 0.827f, 0.600f};                      // #34d399 good
}

Rgb accentCpu()    { return {0.133f, 0.827f, 0.933f}; } // #22d3ee
Rgb accentMemory() { return {0.655f, 0.545f, 0.980f}; } // #a78bfa
Rgb accentGpu()    { return {0.984f, 0.749f, 0.141f}; } // #fbbf24
Rgb accentDisk()   { return {0.376f, 0.647f, 0.980f}; } // #60a5fa
Rgb accentNetRx()  { return {0.204f, 0.827f, 0.600f}; } // #34d399
Rgb accentNetTx()  { return {0.984f, 0.573f, 0.235f}; } // #fb923c

} // namespace Theme
