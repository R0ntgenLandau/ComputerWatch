#pragma once

#include "common.h"

#include <memory>
#include <string_view>

namespace cw {

struct SharedTimingField {
    wchar_t name[40]{}, unit[20]{}, scope[48]{}, source[120]{}, reason[120]{};
    double value = Missing;
    ReadingStatus status = ReadingStatus::BackendMissing;
};

struct SharedMemoryTimings {
    std::uint32_t protocol = 1;
    ReadingStatus status = ReadingStatus::BackendMissing;
    std::int64_t sampledAtMs{};
    wchar_t platform[160]{}, backend[80]{}, reason[200]{};
    std::uint32_t count{};
    std::array<SharedTimingField, 256> fields;
};

// Executes in the elevated sensor helper. A separate process bounds driver calls.
SharedMemoryTimings readMemoryTimings();
std::shared_ptr<const MemoryTimingReport> memoryTimingReport(const SharedMemoryTimings& data);
int runMemoryTimingWorker(std::wstring_view mappingName);

} // namespace cw
