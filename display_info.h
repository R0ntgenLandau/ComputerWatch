#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace cw {

struct Chromaticity {
    double x{}, y{};
};

inline constexpr std::array<Chromaticity, 3> SrgbPrimaries{{{.640, .330}, {.300, .600}, {.150, .060}}};

struct DisplayGamut {
    std::array<Chromaticity, 3> primaries;
    double srgb{}, displayP3{}, adobeRgb{};
};

struct DisplayInfo {
    std::wstring name;
    std::wstring connection;
    unsigned signalWidth{};
    unsigned signalHeight{};
    std::optional<double> refreshHz;
    std::optional<DisplayGamut> gamut;
};

// A missing result means Windows could not query the active display configuration.
std::optional<std::vector<DisplayInfo>> activeDisplays();

} // namespace cw
