#include "AboutWindow.h"
#include "imgui.h"

#include <cstdlib>
#include <string>

#ifndef TM_VERSION_STRING
#define TM_VERSION_STRING "1.0.1"
#endif

namespace {
constexpr const char* kAuthorName = "CtCodingg";
constexpr const char* kGithubUrl = "https://github.com/CtCodingg";

void openUrl(const char* url) {
#ifdef _WIN32
    // ShellExecuteA needs <shellapi.h>; avoided here to keep this file
    // platform-generic -- system()+start is a simple, dependency-free
    // fallback that works fine for a plain https:// URL.
    std::string cmd = std::string("start \"\" \"") + url + "\"";
    std::system(cmd.c_str());
#else
    std::string cmd = std::string("xdg-open \"") + url + "\" >/dev/null 2>&1 &";
    std::system(cmd.c_str());
#endif
}
}

void AboutWindow::open() { m_shouldOpen = true; }

void AboutWindow::draw() {
    if (m_shouldOpen) {
        ImGui::OpenPopup("About CtTaskManager");
        m_shouldOpen = false;
    }

    ImGui::SetNextWindowSize(ImVec2(440, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("About CtTaskManager", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("CtTaskManager");
        ImGui::TextDisabled("Version %s", TM_VERSION_STRING);

        ImGui::Spacing();
        ImGui::TextDisabled("License");
        ImGui::Text("MIT License");
        ImGui::TextWrapped("All third-party dependencies (GLFW, Dear ImGui, ImPlot) are "
                            "MIT/zlib licensed too -- no GPL/LGPL obligations. Full text in "
                            "LICENCE at the repository root.");

        ImGui::Spacing();
        ImGui::TextDisabled("Author");
        ImGui::Text("%s", kAuthorName);
        if (ImGui::Selectable(kGithubUrl)) openUrl(kGithubUrl);
        ImGui::TextDisabled("(click to open)");

        ImGui::Spacing();
        ImGui::TextDisabled("Third-party software");
        ImGui::BulletText("GLFW -- zlib/libpng license");
        ImGui::BulletText("Dear ImGui -- MIT license");
        ImGui::BulletText("ImPlot -- MIT license");
        ImGui::TextWrapped("No other third-party libraries are bundled. Platform APIs used directly "
                            "(Linux: /proc, Netlink, AF_PACKET; Windows: PDH, IP Helper, ETW, TCP EStats) "
                            "are part of the OS, not redistributed with this app.");

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}
