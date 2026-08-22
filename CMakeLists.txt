cmake_minimum_required(VERSION 3.16)

# Must be set before project(): selects the /MT (static) vs /MD (dynamic)
# MSVC C runtime. NEW behavior makes CMAKE_MSVC_RUNTIME_LIBRARY (set just
# below) actually take effect; cmake_minimum_required(3.16) already
# implies this policy defaults to NEW, but setting it explicitly avoids
# any ambiguity.
cmake_policy(SET CMP0091 NEW)

project(CtTaskManager
    VERSION 0.1.0
    DESCRIPTION "Cross-platform Task Manager -- Dear ImGui edition (no Qt)"
    LANGUAGES CXX
)

# Statically link the MSVC C/C++ runtime (/MT, /MTd for Debug) instead of
# the default /MD, /MDd. Result: the .exe needs no vcruntime140.dll /
# msvcp140.dll / ucrtbase.dll on the target machine -- it's fully
# self-contained, matching this project's "no external dependencies"
# goal now that Qt (and windeployqt) are gone. Must be set BEFORE the
# FetchContent_MakeAvailable() calls below, so glfw/imgui/implot are all
# built with the same runtime setting -- a mismatch between static libs
# built with /MT vs /MD causes LNK2038 "mismatch detected" errors.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

if(EXISTS "/etc/nv_tegra_release")
    set(TM_IS_JETSON TRUE)
else()
    set(TM_IS_JETSON FALSE)
endif()

# ---------------------------------------------------------------------------
# Third-party sources, fetched at configure time (no system install needed).
# All MIT/zlib-licensed -- no GPL/LGPL entanglement like Qt Charts had.
# ---------------------------------------------------------------------------
include(FetchContent)

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(glfw)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
)
FetchContent_MakeAvailable(imgui)

FetchContent_Declare(implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        v0.16
)
FetchContent_MakeAvailable(implot)

# Dear ImGui doesn't ship its own CMakeLists.txt for the core lib + GLFW/GL3
# backends, so build them as a small static lib ourselves.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)
target_link_libraries(imgui PUBLIC glfw)

add_library(implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
)
target_include_directories(implot PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(implot PUBLIC imgui)

# ---------------------------------------------------------------------------
# TaskManager sources
# ---------------------------------------------------------------------------
set(TM_SOURCES
    src/main.cpp
    src/App.cpp
    src/Theme.cpp
    src/ui/ProcessesTab.cpp
    src/backend/FormatUtils.cpp
)

if(WIN32)
    list(APPEND TM_SOURCES src/backend/platform/win/ProcessBackendWin.cpp)
elseif(UNIX)
    list(APPEND TM_SOURCES src/backend/platform/linux/ProcessBackendLinux.cpp)
endif()

add_executable(CtTaskManager ${TM_SOURCES})

target_include_directories(CtTaskManager PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/backend
)

target_link_libraries(CtTaskManager PRIVATE imgui implot)

target_compile_definitions(CtTaskManager PRIVATE
    $<$<BOOL:${TM_IS_JETSON}>:TM_IS_JETSON>
)

if(WIN32)
    target_link_libraries(CtTaskManager PRIVATE opengl32 psapi)
    target_compile_definitions(CtTaskManager PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
elseif(UNIX AND NOT APPLE)
    find_package(OpenGL REQUIRED)
    target_link_libraries(CtTaskManager PRIVATE OpenGL::GL dl pthread)

    # Statically link libstdc++/libgcc (the Linux equivalent of the MSVC
    # static-CRT setting above): makes the binary portable across systems
    # with a different/older GCC runtime than the one it was built with,
    # without needing a matching libstdc++.so installed on the target.
    #
    # Deliberately NOT fully static (glibc itself, via -static): glibc's
    # NSS (Name Service Switch) -- used by getpwuid() in
    # ProcessBackendLinux.cpp for the process table's "User" column --
    # loads its backends as shared plugins at runtime and is well-known to
    # break silently when statically linked. Static libstdc++/libgcc has
    # no such caveat and is the standard, safe choice here.
    target_link_options(CtTaskManager PRIVATE -static-libgcc -static-libstdc++)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
# Unlike the Qt edition, there's no windeployqt step needed here: GLFW is
# fetched and linked as a STATIC library by default (FetchContent above
# doesn't set BUILD_SHARED_LIBS), so there are no runtime DLLs to deploy
# alongside the executable -- OpenGL itself is a system driver library on
# every target platform (opengl32.dll on Windows, libGL on Linux/Jetson).
include(GNUInstallDirs)
install(TARGETS CtTaskManager RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

message(STATUS "CtTaskManager: Qt-free build, GLFW+Dear ImGui+ImPlot fetched via FetchContent")
