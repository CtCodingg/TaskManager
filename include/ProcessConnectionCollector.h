#pragma once

#include "Types.h"
#include <QVector>

// Collects the current list of network connections (TCP + UDP, IPv4 + IPv6)
// together with the owning process ID -- a connection-level snapshot (who
// is connected to what, and how), not a byte counter. See the class
// comment on ProcessConnection in Types.h for why per-process bandwidth
// needs a heavier mechanism that isn't implemented here yet.
class ProcessConnectionCollector {
public:
    ProcessConnectionCollector();
    ~ProcessConnectionCollector();

    QVector<ProcessConnection> collect();

private:
    class Impl;
    Impl* m_impl;
};
