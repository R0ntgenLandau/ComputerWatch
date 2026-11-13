#pragma once

#include "common.h"

#include <memory>
#include <utility>

namespace cw {

class Telemetry {
public:
    Telemetry();
    ~Telemetry();

    HardwareInventory inventory() const;
    Snapshot sample();
    HardwareDetails details(DetailPane pane, bool keepAlive = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class CpuSensors {
public:
    CpuSensors();
    ~CpuSensors();

    // CPU package temperature (°C), then average CPU package power (W).
    std::pair<double, double> sample();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace cw
