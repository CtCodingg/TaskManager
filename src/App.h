#pragma once

#include "backend/ProcessCollector.h"
#include "ui/ProcessesTab.h"

// Top-level application state and per-frame draw, called from main.cpp's
// GLFW/OpenGL3 render loop. Replaces Qt's MainWindow: no signals/slots,
// no event loop of its own -- main.cpp drives everything, App just holds
// state and draws ImGui widgets once per frame.
class App {
public:
    App();

    // Called once per frame from main.cpp, after ImGui::NewFrame().
    void draw();

private:
    ProcessCollector m_processCollector;
    ProcessesTab m_processesTab;

    double m_lastProcessPollTime = 0.0;
    double m_processPollIntervalSec = 1.5;
};
