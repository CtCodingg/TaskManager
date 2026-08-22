#pragma once

#include "Types.h"
#include <vector>

class ProcessConnectionCollector {
public:
    ProcessConnectionCollector();
    ~ProcessConnectionCollector();

    std::vector<ProcessConnection> collect();

private:
    class Impl;
    Impl* m_impl;
};
