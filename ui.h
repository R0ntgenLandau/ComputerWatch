#pragma once

#include "common.h"

#include <windows.h>

#include <functional>
#include <optional>
#include <vector>

namespace cw {

struct UiCallbacks {
    std::function<std::vector<Snapshot>()> live;
    std::function<std::vector<Snapshot>(int64_t, int64_t, size_t)> series;
    std::function<std::optional<Snapshot>(int64_t)> at;
    std::function<int()> retention;
    std::function<bool(int)> setRetention;
    std::function<void()> stop;
    std::function<void(bool)> visibility;
    std::function<void()> startCpu;
    std::function<HardwareDetails()> details;
    std::function<void(DetailPane)> selectDetail;
    std::function<bool()> darkMode;
    std::function<bool(bool)> setDarkMode;
};

int runUi(HINSTANCE instance, bool startVisible, UiCallbacks callbacks);

} // namespace cw
