#pragma once

#include "backend/Types.h"
#include <vector>
#include <functional>

// pidToName: callback into ProcessesTab's known process list.
class ConnectionsTab {
public:
    void updateData(std::vector<ProcessConnection> connections);
    void draw(const std::function<std::string(int64_t)>& pidToName);

private:
    std::vector<ProcessConnection> m_connections;
    char m_filterBuf[128] = "";
};
