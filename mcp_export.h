#pragma once
#include "common.h"
#include "display_info.h"
#include <filesystem>
namespace cw {
struct DashboardExport {
    HardwareInventory inventory;
    std::array<HardwareDetails,5> details;
    std::array<std::int64_t,5> detailTimes{};
    std::vector<Snapshot> live;
    std::optional<std::vector<DisplayInfo>> displays;
    std::int64_t generatedAt{};
    bool pending{};
    int retention{};
    bool dark{};
};
// Writes only on an explicit MCP refresh request; atomic replacement prevents partial JSON reads.
bool writeDashboardExport(const std::filesystem::path&, const DashboardExport&);
}
