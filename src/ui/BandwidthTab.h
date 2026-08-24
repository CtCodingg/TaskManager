#pragma once

#include "backend/Types.h"
#include <vector>
#include <string>
#include <functional>

class BandwidthTab {
public:
    void updateData(std::vector<ProcessInterfaceBandwidth> entries);
    void setAvailabilityNote(std::string note) { m_availabilityNote = std::move(note); }
    void draw(const std::function<std::string(int64_t)>& pidToName);

private:
    std::vector<ProcessInterfaceBandwidth> m_entries;
    std::string m_availabilityNote;
};
