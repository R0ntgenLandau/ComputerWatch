#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace cw {

enum class Metric {
    CpuPercent, MemoryPercent, MemoryUsed, MemoryTotal, DiskUsed, DiskTotal,
    DiskRead, DiskWrite, GpuPercent, GpuMemoryUsed, GpuMemoryTotal,
    NetworkDown, NetworkUp, CpuTemp, CpuWatts, GpuTemp, GpuWatts, Count
};

inline constexpr std::size_t TopCount = 10;
inline constexpr std::size_t MetricCount = static_cast<std::size_t>(Metric::Count);
inline constexpr double Missing = std::numeric_limits<double>::quiet_NaN();

enum class DetailPane { None, Cpu, Gpu, Disk };

struct CoreDetail {
    unsigned group{};
    unsigned number{};
    double loadPercent = Missing;
    double frequencyMhz = Missing;
};

struct DiskDetail {
    unsigned number{};
    std::wstring name;
    std::wstring bus;
    std::optional<bool> healthy;
    double temperatureC = Missing;
    double wearPercent = Missing;
    double powerOnHours = Missing;
    double powerCycles = Missing;
    double errors = Missing;
    std::wstring note;
};

struct HardwareInventory {
    std::vector<std::wstring> processors, memory, disks, graphics;
    std::wstring sampledGpu;
};

struct HardwareDetails {
    DetailPane pane = DetailPane::None;
    std::vector<CoreDetail> cores;
    std::vector<DiskDetail> disks;
    double gpuCoreMhz = Missing;
    double gpuMemoryMhz = Missing;
    double gpuFanPercent = Missing;
    double gpuFanRpm = Missing;
    double gpuHotspotC = Missing;
    bool gpuLimitsAvailable{};
    bool gpuThermalLimited{};
    bool gpuPowerLimited{};
};

struct ProcessEntry {
    std::uint32_t pid{};
    wchar_t name[128]{};
    double cpu{};
    std::uint64_t memory{};
};

struct Snapshot {
    std::int64_t timestampMs{};
    std::array<double, MetricCount> values = [] {
        std::array<double, MetricCount> result;
        result.fill(Missing);
        return result;
    }();
    std::array<ProcessEntry, TopCount> byCpu{};
    std::array<ProcessEntry, TopCount> byMemory{};

    double& operator[](Metric metric) { return values[static_cast<std::size_t>(metric)]; }
    double operator[](Metric metric) const { return values[static_cast<std::size_t>(metric)]; }
};

} // namespace cw
