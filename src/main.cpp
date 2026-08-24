#include "App.h"
#include "Theme.h"
#include "cli/TuiApp.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"

// Deliberately NOT using GLFW_INCLUDE_NONE here: GLFW's default behavior
// of pulling in the system's gl.h is exactly what declares the legacy
// GL 1.1 functions used below (glViewport/glClear/glClearColor). Dear
// ImGui's bundled OpenGL3 loader (via imgui_impl_opengl3.h above) only
// adds function pointers for GL 3.x+ entry points beyond that -- it's
// designed to coexist with GLFW's default gl.h, not replace it. This
// matches Dear ImGui's own official example_glfw_opengl3/main.cpp.
#include <GLFW/glfw3.h>
#include <cstdio>
#include <iostream>
#include <string>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

void glfwErrorCallback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

// Command-line flags this app recognizes.
constexpr const char* kTrackBandwidthFlag = "--track-bandwidth";
constexpr const char* kTuiFlag = "--tui";
constexpr const char* kHelpFlagLong = "--help";
constexpr const char* kHelpFlagShort = "-h";

bool hasFlag(int argc, char* argv[], const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == flag) return true;
    }
    return false;
}

void printHelp() {
    std::cout <<
        "CtTaskManager -- cross-platform system monitor (Dear ImGui edition)\n"
        "\n"
        "Usage: CtTaskManager [options]\n"
        "\n"
        "Options:\n"
        "  -h, --help          Show this help message and exit.\n"
        "\n"
        "  --tui               Run as an htop-style terminal UI instead of\n"
        "                      opening a graphical window. Runs the same\n"
        "                      backend as the graphical mode; currently the\n"
        "                      Processes view only (sortable/filterable table,\n"
        "                      CPU/memory meter bars, kill-with-confirmation).\n"
        "                      Press '1' inside for Processes, 'q' to quit.\n"
        "\n"
        "  --track-bandwidth   Enable the Bandwidth tab (per-process download/\n"
        "                      upload, TCP + UDP). Graphical mode only for now.\n"
        "\n"
        "                      Windows: requires Administrator rights and\n"
        "                      triggers a UAC prompt on launch; cancelling the\n"
        "                      prompt falls back to a normal launch without the\n"
        "                      tab, rather than refusing to start.\n"
        "\n"
        "                      Linux: TCP works without elevation. UDP\n"
        "                      additionally needs root or the CAP_NET_RAW\n"
        "                      capability on the binary, e.g.:\n"
        "                        sudo setcap cap_net_raw+ep <path-to-binary>\n"
        "                      Without it, the Bandwidth tab still opens with\n"
        "                      TCP data and a status note explaining why UDP\n"
        "                      is unavailable.\n"
        "\n"
        "Without any options, CtTaskManager starts normally with the\n"
        "Processes, Performance, Network, and Connections tabs. No admin/\n"
        "elevated rights are required for normal operation on either platform.\n"
        << std::flush;
}

#ifdef _WIN32
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

bool relaunchElevated(int argc, char* argv[]) {
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return false;

    std::string args;
    for (int i = 1; i < argc; ++i) {
        args += argv[i];
        if (i + 1 < argc) args += ' ';
    }
    std::wstring argsW(args.begin(), args.end());

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath;
    sei.lpParameters = argsW.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) return false; // most commonly: user clicked "No" on the UAC prompt
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return true;
}
#endif

} // namespace

int main(int argc, char* argv[]) {
    if (hasFlag(argc, argv, kHelpFlagLong) || hasFlag(argc, argv, kHelpFlagShort)) {
        printHelp();
        return 0;
    }

    bool trackBandwidth = hasFlag(argc, argv, kTrackBandwidthFlag);
    bool tuiMode = hasFlag(argc, argv, kTuiFlag);

#ifdef _WIN32
    // Skip elevation entirely in TUI mode: --track-bandwidth isn't wired
    // into the TUI's tabs yet (see runTuiApp's doc comment), so there's
    // nothing that would actually need the elevated rights -- prompting
    // for UAC here would be misleading.
    if (trackBandwidth && !tuiMode && !isProcessElevated()) {
        if (relaunchElevated(argc, argv)) {
            return 0; // the elevated instance takes over; this one exits quietly
        }
        trackBandwidth = false; // elevation cancelled/failed -- run normally, without the flag
    }
#endif

    if (tuiMode) {
        // Terminal UI mode: no window, no OpenGL, no GLFW at all -- just
        // the backend collectors driving an FTXUI terminal session. See
        // src/cli/TuiApp.cpp.
        return runTuiApp(trackBandwidth);
    }

    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialize GLFW\n");
        return 1;
    }

    // OpenGL 3.3 core -- broadly supported on Linux/Jetson/Windows alike,
    // matches what imgui_impl_opengl3's bundled loader targets by default.
    const char* glslVersion = "#version 330";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(1280, 800, "CtTaskManager", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create GLFW window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // vsync -- keeps polling/redraw cheap on the CPU

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // no imgui.ini clutter next to the binary

    // ImGui's built-in default font (ProggyClean) is a small pixel-art
    // style font designed to look crisp only at its native ~13px size --
    // it was never meant to be rasterized larger with antialiasing, which
    // reads noticeably blurrier/blockier at larger sizes. Load a real
    // system TrueType font instead, with proper oversampling for smooth
    // antialiased glyphs at any size.
    // Falls back to the improved-but-still-limited default font only if
    // no system font could be found at any of the well-known paths.
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true;

    const char* candidateFonts[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\segoeui.ttf",   // Windows' default UI font
        "C:\\Windows\\Fonts\\arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",             // Arch
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", // Fedora
#endif
    };

    ImFont* loadedFont = nullptr;
    for (const char* path : candidateFonts) {
        if (FILE* f = std::fopen(path, "rb")) {
            std::fclose(f);
            loadedFont = io.Fonts->AddFontFromFileTTF(path, 18.0f, &fontConfig);
            if (loadedFont) break;
        }
    }
    if (!loadedFont) {
        fontConfig.SizePixels = 18.0f;
        io.Fonts->AddFontDefault(&fontConfig);
    }

    Theme::apply();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glslVersion);

    App app(trackBandwidth);

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.draw();

        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.05f, 0.07f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
