cmake_minimum_required(VERSION 3.16)

project(TaskManager
    VERSION 0.1.0
    DESCRIPTION "Cross-platform Task Manager (Ubuntu 22/24, ARMv8/Jetson, Windows)"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
endif()

# compile_commands.json for clangd / VS Code IntelliSense
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

message(STATUS "TaskManager: building for ${CMAKE_SYSTEM_NAME} (${CMAKE_SYSTEM_PROCESSOR})")

# ---------------------------------------------------------------------------
# Platform / arch detection
# ---------------------------------------------------------------------------
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64|armv8)")
    set(TM_ARCH_ARM64 TRUE)
else()
    set(TM_ARCH_ARM64 FALSE)
endif()

# Jetson boards expose this file; enables Tegra GPU stats via sysfs (no CUDA/NVML needed)
if(EXISTS "/etc/nv_tegra_release")
    set(TM_IS_JETSON TRUE)
else()
    set(TM_IS_JETSON FALSE)
endif()

message(STATUS "TaskManager: ARM64=${TM_ARCH_ARM64} Jetson=${TM_IS_JETSON}")

# ---------------------------------------------------------------------------
# Qt (support Qt5 or Qt6, whichever is found -- covers Ubuntu 22 (Qt5/Qt6)
# and Ubuntu 24 (Qt6) and Windows)
# ---------------------------------------------------------------------------
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets Charts Network)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets Charts Network)

message(STATUS "TaskManager: using Qt${QT_VERSION_MAJOR}")

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
set(TM_SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/ProcessModel.cpp
    src/HistoryChartWidget.cpp
    src/FormatUtils.cpp
)

set(TM_HEADERS
    include/MainWindow.h
    include/ProcessModel.h
    include/ProcessCollector.h
    include/SystemStatsCollector.h
    include/NetworkStatsCollector.h
    include/GpuStatsCollector.h
    include/HistoryChartWidget.h
    include/FormatUtils.h
    include/Types.h
)

if(WIN32)
    list(APPEND TM_SOURCES
        src/platform/win/ProcessBackendWin.cpp
        src/platform/win/SystemBackendWin.cpp
        src/platform/win/NetworkBackendWin.cpp
        src/platform/win/GpuBackendWin.cpp
    )
elseif(UNIX)
    list(APPEND TM_SOURCES
        src/platform/linux/ProcessBackendLinux.cpp
        src/platform/linux/SystemBackendLinux.cpp
        src/platform/linux/NetworkBackendLinux.cpp
        src/platform/linux/GpuBackendLinux.cpp
    )
endif()

add_executable(TaskManager ${TM_SOURCES} ${TM_HEADERS} resources/resources.qrc)

target_include_directories(TaskManager PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(TaskManager PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Charts
    Qt${QT_VERSION_MAJOR}::Network
)

target_compile_definitions(TaskManager PRIVATE
    $<$<BOOL:${TM_IS_JETSON}>:TM_IS_JETSON>
    $<$<BOOL:${TM_ARCH_ARM64}>:TM_ARCH_ARM64>
)

# ---------------------------------------------------------------------------
# Windows: PDH (perf counters), IP Helper (network), PSAPI, SetupAPI (GPU enum)
# ---------------------------------------------------------------------------
if(WIN32)
    target_link_libraries(TaskManager PRIVATE pdh iphlpapi ws2_32 psapi setupapi)
    target_compile_definitions(TaskManager PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ---------------------------------------------------------------------------
# Linux/Jetson: dl + pthread. NVML (for discrete NVIDIA GPUs) is dlopen()'d
# at RUNTIME ONLY if present on the system -- no hard link-time dependency,
# so the "no other dependencies" requirement holds on machines without it
# (e.g. plain Jetson, which uses the sysfs/tegra backend instead).
# ---------------------------------------------------------------------------
if(UNIX AND NOT APPLE)
    target_link_libraries(TaskManager PRIVATE dl pthread)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS TaskManager RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---------------------------------------------------------------------------
# Windows: run windeployqt against the INSTALLED executable (not the raw
# build tree), so that `cmake --install build --prefix <dir>` produces a
# fully self-contained, deployable folder under CMAKE_INSTALL_PREFIX with
# the .exe and all required Qt DLLs/plugins side by side.
#
# install(CODE ...) is evaluated at `cmake --install` time (i.e. when you
# run "cmake --install build" or build the INSTALL target in Visual
# Studio), always AFTER install(TARGETS ...) above has already copied the
# .exe into place, so windeployqt has something to deploy against.
# ---------------------------------------------------------------------------
if(WIN32)
    get_target_property(_qmake_exe Qt${QT_VERSION_MAJOR}::qmake IMPORTED_LOCATION)
    if(_qmake_exe)
        get_filename_component(_qt_bin_dir "${_qmake_exe}" DIRECTORY)
        find_program(WINDEPLOYQT_EXECUTABLE windeployqt HINTS "${_qt_bin_dir}")
    endif()

    if(WINDEPLOYQT_EXECUTABLE)
        install(CODE "
            message(STATUS \"Running windeployqt on installed executable...\")
            execute_process(
                COMMAND \"${WINDEPLOYQT_EXECUTABLE}\"
                        --no-compiler-runtime
                        \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/TaskManager.exe\"
                RESULT_VARIABLE _windeployqt_result
            )
            if(NOT _windeployqt_result EQUAL 0)
                message(WARNING \"windeployqt exited with code \${_windeployqt_result}\")
            endif()
        ")
    else()
        message(WARNING "windeployqt not found -- 'cmake --install' will NOT bundle Qt DLLs. "
                         "Make sure Qt's bin/ directory is next to qmake, or run windeployqt manually "
                         "against the installed .exe afterward.")
    endif()
endif()