#pragma once

// Preferences modal: rate-unit display (bits/bytes) and UI refresh rate.
// Persists to a tiny INI-style file next to the executable via simple
// manual read/write.
class SettingsWindow {
public:
    SettingsWindow();

    void open();
    // Draws the modal if open; returns true the frame the user accepts
    // changes (caller should re-apply rate unit / refresh interval then).
    bool draw();

    bool useBits() const { return m_useBits; }
    int refreshRateMs() const { return m_refreshRateMs; }

private:
    bool m_shouldOpen = false;
    bool m_useBits = true;
    int m_refreshRateMs = 1000;

    void load();
    void save();
};
