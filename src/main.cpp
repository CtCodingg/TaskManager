#include "App.h"
#include "Theme.h"

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

namespace {
void glfwErrorCallback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}
}

int main(int, char**) {
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

    GLFWwindow* window = glfwCreateWindow(1280, 800, "TaskManager", nullptr, nullptr);
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
    // is why it looked noticeably blurrier/blockier than the Qt edition's
    // native Segoe UI rendering. Load a real system TrueType font instead,
    // with proper oversampling for smooth antialiased glyphs at any size.
    // Falls back to the improved-but-still-limited default font only if
    // no system font could be found at any of the well-known paths.
    ImFontConfig fontConfig;
    fontConfig.OversampleH = 3;
    fontConfig.OversampleV = 1;
    fontConfig.PixelSnapH = true;

    const char* candidateFonts[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\segoeui.ttf",   // same font the Qt edition used
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

    App app;

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
