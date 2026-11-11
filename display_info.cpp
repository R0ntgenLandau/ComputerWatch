#include "display_info.h"

#include <windows.h>
#include <setupapi.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace cw {
namespace {

using Triangle = std::array<Chromaticity, 3>;
constexpr Triangle DisplayP3{{{.680, .320}, {.265, .690}, {.150, .060}}};
constexpr Triangle AdobeRgb{{{.640, .330}, {.210, .710}, {.150, .060}}};

double side(Chromaticity a, Chromaticity b, Chromaticity point) {
    return (b.x - a.x) * (point.y - a.y) - (b.y - a.y) * (point.x - a.x);
}

// Clip the EDID triangle against the three edges of the reference in CIE 1931 xy.
double coverage(const Triangle& primaries, const Triangle& reference) {
    std::vector<Chromaticity> polygon(primaries.begin(), primaries.end());
    for (size_t i = 0; i < reference.size() && !polygon.empty(); ++i) {
        const auto a = reference[i];
        const auto b = reference[(i + 1) % reference.size()];
        std::vector<Chromaticity> clipped;
        auto previous = polygon.back();
        auto previousSide = side(a, b, previous);
        for (const auto point : polygon) {
            const auto currentSide = side(a, b, point);
            if ((previousSide >= 0) != (currentSide >= 0)) {
                const auto fraction = previousSide / (previousSide - currentSide);
                clipped.push_back({previous.x + fraction * (point.x - previous.x),
                    previous.y + fraction * (point.y - previous.y)});
            }
            if (currentSide >= 0) clipped.push_back(point);
            previous = point;
            previousSide = currentSide;
        }
        polygon.swap(clipped);
    }
    double doubleArea = 0;
    for (size_t i = 2; i < polygon.size(); ++i) {
        doubleArea += side(polygon[0], polygon[i - 1], polygon[i]);
    }
    return std::clamp(100 * std::abs(doubleArea) / side(reference[0], reference[1], reference[2]), 0.0, 100.0);
}

std::vector<BYTE> monitorEdid(const wchar_t* path) {
    if (!*path) return {};
    const auto devices = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (devices == INVALID_HANDLE_VALUE) return {};
    SP_DEVICE_INTERFACE_DATA interfaceData{};
    interfaceData.cbSize = sizeof(interfaceData);
    SP_DEVINFO_DATA device{};
    device.cbSize = sizeof(device);
    std::vector<BYTE> edid;
    if (SetupDiOpenDeviceInterfaceW(devices, path, 0, &interfaceData)) {
        // SetupAPI can populate the device node even when no detail buffer is supplied.
        SetupDiGetDeviceInterfaceDetailW(devices, &interfaceData, nullptr, 0, nullptr, &device);
        const auto key = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
        if (key != INVALID_HANDLE_VALUE) {
            DWORD size{};
            if (RegGetValueW(key, nullptr, L"EDID", RRF_RT_REG_BINARY, nullptr, nullptr, &size) == ERROR_SUCCESS &&
                size >= 128 && size <= 256 * 128) {
                edid.resize(size);
                if (RegGetValueW(key, nullptr, L"EDID", RRF_RT_REG_BINARY, nullptr, edid.data(), &size) == ERROR_SUCCESS) {
                    edid.resize(size);
                } else {
                    edid.clear();
                }
            }
            RegCloseKey(key);
        }
    }
    SetupDiDestroyDeviceInfoList(devices);
    return edid;
}

std::optional<DisplayGamut> edidGamut(const std::vector<BYTE>& edid) {
    constexpr std::array<BYTE, 8> header{0, 255, 255, 255, 255, 255, 255, 0};
    if (edid.size() < 128 || !std::equal(header.begin(), header.end(), edid.begin()) || edid[18] != 1 ||
        std::accumulate(edid.begin(), edid.begin() + 128, 0) % 256 != 0) return std::nullopt;

    // EDID 1.x stores six high bytes and packs the two low bits of each coordinate.
    const auto coordinate = [&](size_t index) {
        const auto low = (edid[25 + index / 4] >> (6 - 2 * (index % 4))) & 3;
        return (edid[27 + index] * 4 + low) / 1024.0;
    };
    Triangle primaries;
    for (size_t i = 0; i < primaries.size(); ++i) {
        const auto x = coordinate(i * 2);
        const auto y = coordinate(i * 2 + 1);
        if (x <= 0 || y <= 0 || x + y > 1) return std::nullopt;
        primaries[i] = {x, y};
    }
    if (std::abs(side(primaries[0], primaries[1], primaries[2])) < 1e-6) return std::nullopt;
    return DisplayGamut{primaries, coverage(primaries, SrgbPrimaries), coverage(primaries, DisplayP3),
        coverage(primaries, AdobeRgb)};
}

const wchar_t* connectionName(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology) {
    switch (technology) {
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15: return L"VGA";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI: return L"DVI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI: return L"HDMI";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL: return L"内置";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL: return L"DisplayPort";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED: return L"eDP";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_USB_TUNNEL: return L"DP（USB4）";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST: return L"Miracast";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED: return L"有线间接显示";
    case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL: return L"虚拟显示";
    default: return L"";
    }
}

std::optional<double> refreshHz(DISPLAYCONFIG_RATIONAL rate) {
    if (!rate.Numerator || !rate.Denominator) return std::nullopt;
    return static_cast<double>(rate.Numerator) / rate.Denominator;
}

DisplayInfo readDisplay(const DISPLAYCONFIG_PATH_TARGET_INFO& target,
    const std::vector<DISPLAYCONFIG_MODE_INFO>& modes, UINT32 modeCount) {
    DisplayInfo display;
    DISPLAYCONFIG_TARGET_DEVICE_NAME device{};
    device.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
    device.header.size = sizeof(device);
    device.header.adapterId = target.adapterId;
    device.header.id = target.id;
    if (DisplayConfigGetDeviceInfo(&device.header) == ERROR_SUCCESS) {
        display.name = device.monitorFriendlyDeviceName;
        display.gamut = edidGamut(monitorEdid(device.monitorDevicePath));
    }
    display.connection = connectionName(target.outputTechnology);
    display.refreshHz = refreshHz(target.refreshRate);
    if (target.modeInfoIdx < modeCount) {
        const auto& mode = modes[target.modeInfoIdx];
        if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
            const auto& signal = mode.targetMode.targetVideoSignalInfo;
            display.signalWidth = signal.activeSize.cx;
            display.signalHeight = signal.activeSize.cy;
            if (const auto hz = refreshHz(signal.vSyncFreq)) display.refreshHz = hz;
        }
    }
    return display;
}

} // namespace

std::optional<std::vector<DisplayInfo>> activeDisplays() {
    constexpr UINT32 flags = QDC_ONLY_ACTIVE_PATHS;
    // A topology change can invalidate the buffer sizes between the two calls.
    for (int attempt = 0; attempt < 4; ++attempt) {
        UINT32 pathCount{}, modeCount{};
        if (GetDisplayConfigBufferSizes(flags, &pathCount, &modeCount) != ERROR_SUCCESS) break;
        if (!pathCount) return std::vector<DisplayInfo>{};
        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount ? modeCount : 1);
        const auto result = QueryDisplayConfig(flags, &pathCount, paths.data(), &modeCount, modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) continue;
        if (result != ERROR_SUCCESS) break;

        std::vector<DisplayInfo> displays;
        displays.reserve(pathCount);
        for (UINT32 i = 0; i < pathCount; ++i) {
            displays.push_back(readDisplay(paths[i].targetInfo, modes, modeCount));
        }
        return displays;
    }
    return std::nullopt;
}

} // namespace cw
