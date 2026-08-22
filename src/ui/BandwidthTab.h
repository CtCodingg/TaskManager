#pragma once

#include "backend/Types.h"
#include <map>
#include <string>
#include <functional>

class BandwidthTab {
public:
    void updateData(std::map<int64_t, ProcessBandwidthStats> stats);
    void setAvailabilityNote(std::string note) { m_availabilityNote = std::move(note); }
    void draw(const std::function<std::string(int64_t)>& pidToName);

private:
    std::map<int64_t, ProcessBandwidthStats> m_stats;
    std::string m_availabilityNote;
};
