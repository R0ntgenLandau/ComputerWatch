#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>
#include <winternl.h>

#include "telemetry.h"
#include "disk_smart.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cw {
namespace {

using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

template <typename Function>
Function resolve(HMODULE module, const char* name) {
    return std::bit_cast<Function>(GetProcAddress(module, name));
}

// The firmware strings are byte sequences. Accept UTF-8 and retain legacy
// manufacturer strings reported in the Windows system code page.
std::wstring modelText(std::string_view raw) {
    const auto first = raw.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    raw = raw.substr(first, raw.find_last_not_of(" \t") - first + 1);
    const int length = static_cast<int>(raw.size());
    UINT codepage = CP_UTF8;
    int count = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, raw.data(), length, nullptr, 0);
    if (!count) {
        codepage = CP_ACP;
        count = MultiByteToWideChar(codepage, 0, raw.data(), length, nullptr, 0);
    }
    std::wstring text(count, L'\0');
    if (count) MultiByteToWideChar(codepage, 0, raw.data(), length, text.data(), count);
    return text;
}

std::wstring firmwareString(const BYTE* first, const BYTE* end, unsigned index) {
    if (!index) return {};
    while (first < end) {
        const auto* terminator = std::find(first, end, 0);
        if (--index == 0)
            return modelText({reinterpret_cast<const char*>(first), static_cast<size_t>(terminator - first)});
        first = terminator == end ? end : terminator + 1;
    }
    return {};
}

MemoryDevice readMemoryDevice(const BYTE* entry, const BYTE* strings, const BYTE* end, BYTE smbiosMajor,
    BYTE smbiosMinor) {
    const auto length = static_cast<size_t>(strings - entry);
    const auto field = [&]<typename T>(size_t offset, T fallback) {
        if (offset + sizeof(T) > length) return fallback;
        T value{};
        std::memcpy(&value, entry + offset, sizeof(value));
        return value;
    };
    const auto byte = [&](size_t offset) { return field(offset, BYTE{}); };
    const auto word = [&](size_t offset) { return field(offset, WORD{}); };
    const auto text = [&](size_t offset) { return firmwareString(strings, end, byte(offset)); };
    const auto speed = [&](size_t offset, size_t extendedOffset) -> double {
        const auto legacy = word(offset);
        const auto value = legacy == 0xffff ? field(extendedOffset, DWORD{}) & 0x7fffffff : legacy;
        return value ? static_cast<double>(value) : missing;
    };
    const auto width = [&](size_t offset) -> double {
        const auto value = field(offset, WORD{0xffff});
        return value == 0xffff ? missing : static_cast<double>(value);
    };
    const auto volts = [&](size_t offset) -> double {
        const auto value = word(offset);
        return value ? value / 1000.0 : missing;
    };
    constexpr const wchar_t* types[]{
        L"", L"", L"", L"DRAM", L"EDRAM", L"VRAM", L"SRAM", L"RAM", L"ROM",
        L"Flash", L"EEPROM", L"FEPROM", L"EPROM", L"CDRAM", L"3DRAM", L"SDRAM",
        L"SGRAM", L"RDRAM", L"DDR", L"DDR2", L"DDR2 FB-DIMM", L"", L"", L"",
        L"DDR3", L"FBD2", L"DDR4", L"LPDDR", L"LPDDR2", L"LPDDR3", L"LPDDR4",
        L"逻辑非易失内存", L"HBM", L"HBM2", L"DDR5", L"LPDDR5", L"HBM3", L"MRDIMM"
    };
    constexpr const wchar_t* forms[]{
        L"", L"", L"", L"SIMM", L"SIP", L"芯片", L"DIP", L"ZIP", L"专用卡",
        L"DIMM", L"TSOP", L"行芯片", L"RIMM", L"SO-DIMM", L"SRIMM", L"FB-DIMM",
        L"芯片裸片", L"CAMM", L"CUDIMM", L"CSODIMM"
    };
    const auto enumText = [](const auto& names, BYTE code) -> std::wstring {
        if (code < std::size(names) && *names[code]) return names[code];
        if (code <= 2) return code == 1 ? L"其他" : L"";
        return L"SMBIOS 编码 " + std::to_wstring(code);
    };
    MemoryDevice device;
    device.slot = text(0x10);
    device.bank = text(0x11);
    const auto size = field(0x0c, WORD{0xffff});
    device.empty = size == 0;
    if (device.empty) return device;
    if (size == 0x7fff) {
        const auto extended = field(0x1c, DWORD{}) & 0x7fffffff;
        if (extended) device.capacity = extended * 1048576.0;
    } else if (size != 0xffff) {
        device.capacity = (size & 0x7fff) * ((size & 0x8000) ? 1024.0 : 1048576.0);
    }
    device.manufacturer = text(0x17);
    device.serialNumber = text(0x18);
    device.assetTag = text(0x19);
    device.partNumber = text(0x1a);
    device.type = enumText(types, byte(0x12));
    device.formFactor = enumText(forms, byte(0x0e));
    // Before SMBIOS 3.1 the specified unit differed from some firmware implementations.
    device.legacySpeed = smbiosMajor < 3 || (smbiosMajor == 3 && smbiosMinor == 0);
    device.ratedSpeed = speed(0x15, 0x54);
    device.configuredSpeed = speed(0x20, 0x58);
    device.dataWidth = width(0x0a);
    device.totalWidth = width(0x08);
    if (const auto ranks = byte(0x1b) & 0xf) device.ranks = ranks;
    device.minVolts = volts(0x22);
    device.maxVolts = volts(0x24);
    device.configuredVolts = volts(0x26);
    return device;
}

std::wstring firmwareCode(BYTE code) {
    wchar_t result[5]{};
    std::swprintf(result, std::size(result), L"0x%02X", static_cast<unsigned>(code));
    return result;
}

template <size_t N>
std::wstring firmwareEnum(const wchar_t* const (&names)[N], BYTE code) {
    if (!code) return {};
    return code < N && names[code] ? names[code] : firmwareCode(code);
}

std::wstring slotType(BYTE code) {
    constexpr BYTE pcie[]{0xa5, 0xab, 0xb1, 0xb8, 0xbe};
    constexpr const wchar_t* widths[]{L"", L" x1", L" x2", L" x4", L" x8", L" x16"};
    for (size_t i = 0; i < std::size(pcie); ++i) {
        if (code >= pcie[i] && code < pcie[i] + std::size(widths)) {
            return L"PCIe Gen " + std::to_wstring(i + 1) + widths[code - pcie[i]];
        }
    }
    if (code == 6) return L"PCI";
    if (code == 0x0e) return L"PCI-X";
    if (code >= 0x14 && code <= 0x17) return L"M.2";
    if (code == 0xc4) return L"PCIe Gen 6+";
    return code > 2 ? firmwareCode(code) : std::wstring{};
}

void readBoardRecord(const BYTE* entry, const BYTE* strings, const BYTE* end,
    std::vector<BoardSection>& boards, BoardSection& bios, BoardSection& slots,
    BoardSection& ports, BoardSection& devices, BoardSection& legacyDevices) {
    const size_t length = strings - entry;
    const auto byte = [&](size_t offset) -> BYTE { return offset < length ? entry[offset] : 0; };
    const auto text = [&](size_t offset) { return firmwareString(strings, end, byte(offset)); };
    const auto release = [&](size_t offset) -> std::wstring {
        if (length < offset + 2 || entry[offset] == 0xff || entry[offset + 1] == 0xff) return {};
        return std::to_wstring(entry[offset]) + L"." + std::to_wstring(entry[offset + 1]);
    };
    const auto pciAddress = [&](size_t offset) -> std::wstring {
        if (length < offset + 4 || entry[offset + 2] == 0xff || entry[offset + 3] == 0xff) return {};
        const unsigned segment = entry[offset] | (entry[offset + 1] << 8);
        if (segment == 0xffff) return {};
        const unsigned bus = entry[offset + 2];
        const unsigned function = entry[offset + 3];
        wchar_t address[16]{};
        std::swprintf(address, std::size(address), L"%04X:%02X:%02X.%X", segment, bus, function >> 3, function & 7);
        return address;
    };
    const auto append = [](std::wstring& target, const std::wstring& value) {
        if (value.empty()) return;
        if (!target.empty()) target += L" · ";
        target += value;
    };
    const auto add = [](BoardSection& section, std::wstring name, std::wstring value, const wchar_t* fallback) {
        if (name.empty()) name = fallback + (L" " + std::to_wstring(section.fields.size() + 1));
        section.fields.emplace_back(std::move(name), std::move(value));
    };
    constexpr const wchar_t* deviceTypes[]{nullptr, L"其他", nullptr, L"显卡", L"SCSI", L"以太网",
        L"令牌环网", L"音频", L"PATA", L"SATA", L"SAS", L"无线网卡", L"蓝牙", L"WWAN", L"eMMC", L"NVMe", L"UFS"};

    switch (entry[0]) {
    case 0:
        if (length < 0x12) break;
        bios.fields = {{L"厂商", text(4)}, {L"版本", text(5)}, {L"发布日期", text(8)},
            {L"系统 BIOS 修订版", release(0x14)}, {L"嵌入式控制器版本", release(0x16)},
            {L"支持 UEFI", length > 0x13 ? (byte(0x13) & 8 ? L"是" : L"否") : L""}};
        if (byte(9) != 0xff) {
            bios.fields.emplace_back(L"固件 ROM 容量", std::to_wstring((byte(9) + 1) * 64) + L" KiB");
        } else if (length >= 0x1a) {
            const unsigned rom = byte(0x18) | (byte(0x19) << 8);
            if ((rom & 0x3fff) && (rom >> 14) <= 1) {
                bios.fields.emplace_back(L"固件 ROM 容量", std::to_wstring(rom & 0x3fff) +
                    (rom >> 14 ? L" GiB" : L" MiB"));
            }
        }
        break;
    case 2: {
        if (length < 8) break;
        constexpr const wchar_t* types[]{nullptr, L"未知", L"其他", L"服务器刀片", L"连接模块", L"系统管理模块",
            L"处理器模块", L"I/O 模块", L"内存模块", L"子板", L"主板", L"处理器与内存模块",
            L"处理器与 I/O 模块", L"互连板"};
        constexpr const wchar_t* flags[]{L"承载系统", L"需子板", L"可拆卸", L"可更换", L"可热插拔"};
        std::wstring features;
        for (size_t i = 0; i < std::size(flags); ++i) {
            if (byte(9) & (1 << i)) append(features, flags[i]);
        }
        if (length > 9 && features.empty()) features = L"无已声明特性";
        boards.push_back({L"主板与模块", {{L"厂商", text(4)}, {L"型号", text(5)}, {L"硬件版本", text(6)},
            {L"序列号", text(7)}, {L"资产标签", text(8)}, {L"机箱内位置", text(0x0a)},
            {L"板卡类型", firmwareEnum(types, byte(0x0d))}, {L"板卡特性", std::move(features)}}});
        break;
    }
    case 8: {
        if (length < 9) break;
        auto external = text(6);
        auto internal = text(4);
        std::wstring endpoints;
        if (!external.empty()) append(endpoints, L"外部 " + external);
        if (!internal.empty()) append(endpoints, L"内部 " + internal);
        constexpr const wchar_t* types[]{L"未指定", L"并口 XT/AT", L"并口 PS/2", L"并口 ECP", L"并口 EPP",
            L"并口 ECP/EPP", L"串口 XT/AT", L"串口 16450", L"串口 16550", L"串口 16550A", L"SCSI",
            L"MIDI", L"游戏端口", L"键盘", L"鼠标", L"SSA SCSI", L"USB", L"FireWire", L"PCMCIA I",
            L"PCMCIA II", L"PCMCIA III", L"CardBus", L"Access Bus", L"SCSI II", L"SCSI Wide", L"PC-98",
            L"PC-98-Hireso", L"PC-H98", L"视频", L"音频", L"调制解调器", L"网络", L"SATA", L"SAS",
            L"MFDP", L"Thunderbolt"};
        const auto type = byte(8);
        std::wstring description = type < std::size(types) ? types[type] : firmwareCode(type);
        if (byte(5) || byte(7)) {
            description += L" · 接头（内/外） " + firmwareCode(byte(5)) + L"/" + firmwareCode(byte(7));
        }
        add(ports, std::move(endpoints), std::move(description), L"端口");
        break;
    }
    case 9: {
        if (length < 0x0c) break;
        constexpr const wchar_t* widths[]{nullptr, L"其他", L"位宽未知", L"8 bit", L"16 bit", L"32 bit", L"64 bit",
            L"128 bit", L"x1", L"x2", L"x4", L"x8", L"x16", L"x32"};
        constexpr const wchar_t* usage[]{nullptr, L"其他", nullptr, L"可用", L"使用中", L"不可用"};
        auto description = slotType(byte(5));
        append(description, firmwareEnum(widths, byte(6)));
        append(description, byte(7) == 2 ? L"占用状态未知" : firmwareEnum(usage, byte(7)));
        append(description, pciAddress(0x0d));
        add(slots, text(4), std::move(description), L"插槽");
        break;
    }
    case 10:
        for (size_t offset = 4; offset + 1 < length; offset += 2) {
            auto description = firmwareEnum(deviceTypes, byte(offset) & 0x7f);
            append(description, byte(offset) & 0x80 ? L"已启用" : L"已禁用");
            add(legacyDevices, text(offset + 1), std::move(description), L"设备");
        }
        break;
    case 41: {
        if (length < 0x0b) break;
        auto description = firmwareEnum(deviceTypes, byte(5) & 0x7f);
        append(description, byte(5) & 0x80 ? L"已启用" : L"已禁用");
        append(description, pciAddress(7));
        add(devices, text(4), std::move(description), L"设备");
        break;
    }
    }
}

void readFirmwareModels(HardwareInventory& inventory) {
    constexpr DWORD rawSmbios = 0x52534D42; // RSMB
    constexpr UINT headerSize = 8;
    const UINT size = GetSystemFirmwareTable(rawSmbios, 0, nullptr, 0);
    if (size < headerSize || size > 16 * 1024 * 1024) return;
    std::vector<BYTE> bytes(size);
    if (GetSystemFirmwareTable(rawSmbios, 0, bytes.data(), size) != size) return;
    DWORD length{};
    std::memcpy(&length, bytes.data() + 4, sizeof(length));
    if (length > size - headerSize) return;

    std::vector<BoardSection> boards;
    BoardSection bios{L"BIOS 与启动固件", {}};
    BoardSection slots{L"扩展插槽", {}};
    BoardSection ports{L"板载端口与接头", {}};
    BoardSection devices{L"板载设备", {}};
    BoardSection legacyDevices{L"板载设备", {}};
    const auto* entry = bytes.data() + headerSize;
    const auto* end = entry + length;
    while (end - entry >= 4 && entry[0] != 127) {
        const auto type = entry[0];
        const auto formattedLength = entry[1];
        if (formattedLength < 4 || end - entry < formattedLength + 2) break;
        const auto* strings = entry + formattedLength;
        const auto* last = strings;
        while (end - last >= 2 && (last[0] || last[1])) ++last;
        if (end - last < 2) break;

        // Type 4: populated processor socket. Type 17: memory device or empty slot.
        if (type == 4 && formattedLength > 0x18 && (entry[0x18] & 0x40)) {
            inventory.processors.push_back(firmwareString(strings, last, entry[0x10]));
        } else if (type == 17 && formattedLength >= 0x15) {
            auto device = readMemoryDevice(entry, strings, last, bytes[1], bytes[2]);
            if (device.slot.empty()) device.slot = L"内存设备 " + std::to_wstring(inventory.memoryDevices.size() + 1);
            if (!device.empty) {
                inventory.memory.push_back(device.slot + L" · " +
                    (device.partNumber.empty() ? L"型号未提供" : device.partNumber));
            }
            inventory.memoryDevices.push_back(std::move(device));
        } else {
            readBoardRecord(entry, strings, last, boards, bios, slots, ports, devices, legacyDevices);
        }
        entry = last + 2;
    }
    bios.fields.emplace_back(L"SMBIOS 版本", std::to_wstring(bytes[1]) + L"." + std::to_wstring(bytes[2]));
    FIRMWARE_TYPE firmware{};
    if (GetFirmwareType(&firmware)) {
        if (firmware == FirmwareTypeUefi) bios.fields.emplace_back(L"当前启动模式", L"UEFI");
        if (firmware == FirmwareTypeBios) bios.fields.emplace_back(L"当前启动模式", L"传统 BIOS");
    }
    if (boards.empty()) boards.push_back({L"主板与模块", {}});
    inventory.board = std::move(boards);
    inventory.board.push_back(std::move(bios));
    inventory.board.push_back(std::move(slots));
    inventory.board.push_back(std::move(ports));
    inventory.board.push_back(devices.fields.empty() ? std::move(legacyDevices) : std::move(devices));
}

std::wstring windowsProcessorName() {
    wchar_t name[256]{};
    DWORD size = sizeof(name);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, name, &size) != ERROR_SUCCESS) return {};
    return name;
}

BoardSection chipsetDevices() {
    BoardSection section{L"芯片组相关 PCI 设备", {}};
    const auto devices = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (devices == INVALID_HANDLE_VALUE) return section;
    const auto destroy = [](void* value) { SetupDiDestroyDeviceInfoList(value); };
    const std::unique_ptr<void, decltype(destroy)> guard(devices, destroy);
    for (DWORD index = 0; ; ++index) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices, index, &device)) break;
        wchar_t ids[512]{};
        DWORD size{};
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_COMPATIBLEIDS, nullptr,
            reinterpret_cast<BYTE*>(ids), sizeof(ids), &size)) continue;
        std::wstring_view role;
        const auto* end = ids + std::min<size_t>(size / sizeof(wchar_t), std::size(ids));
        for (const auto* id = ids; id < end && *id; ) {
            const auto* stop = std::find(id, end, L'\0');
            const std::wstring_view code(id, stop - id);
            if (code == L"PCI\\CC_0601") role = L"ISA / LPC / eSPI 桥";
            if (code == L"PCI\\CC_0C05") role = L"SMBus 控制器";
            if (!role.empty() || stop == end) break;
            id = stop + 1;
        }
        if (role.empty()) continue;
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        std::wstring hardwareId;
        if (SetupDiGetDeviceInstanceIdW(devices, &device, instance, std::size(instance), nullptr)) {
            const std::wstring_view id(instance);
            if (id.starts_with(L"PCI\\VEN_") && id.size() >= 21) hardwareId = id.substr(4, 17);
        }
        wchar_t name[256]{};
        SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_DEVICEDESC, nullptr,
            reinterpret_cast<BYTE*>(name), sizeof(name), nullptr);
        std::wstring description(name);
        if (!hardwareId.empty()) {
            if (!description.empty()) description += L" · ";
            description += hardwareId;
        }
        section.fields.emplace_back(role, std::move(description));
    }
    return section;
}

std::vector<std::wstring> graphicsModels() {
    // GUID_DEVCLASS_DISPLAY. One PnP instance per adapter, including headless devices.
    constexpr GUID displayClass{0x4d36e968, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    const auto devices = SetupDiGetClassDevsW(&displayClass, nullptr, nullptr, DIGCF_PRESENT);
    std::vector<std::wstring> models;
    if (devices == INVALID_HANDLE_VALUE) return models;
    const auto destroy = [](void* value) { SetupDiDestroyDeviceInfoList(value); };
    const std::unique_ptr<void, decltype(destroy)> guard(devices, destroy);
    for (DWORD index = 0; ; ++index) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices, index, &device)) break;
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        if (!SetupDiGetDeviceInstanceIdW(devices, &device, instance, std::size(instance), nullptr)) continue;
        const std::wstring_view id(instance);
        if (id.starts_with(L"ROOT\\") || id.starts_with(L"SWD\\")) continue;

        wchar_t name[256]{};
        const auto read = [&](DWORD property) {
            return SetupDiGetDeviceRegistryPropertyW(devices, &device, property, nullptr,
                reinterpret_cast<BYTE*>(name), sizeof(name), nullptr) && name[0];
        };
        if (read(SPDRP_FRIENDLYNAME) || read(SPDRP_DEVICEDESC)) models.emplace_back(name);
        else models.push_back(L"型号未提供");
    }
    return models;
}

uint64_t ticks(const FILETIME& time) {
    return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

double seconds(TimePoint start, TimePoint end) {
    return std::chrono::duration<double>(end - start).count();
}

class DiskRates {
public:
    DiskRates() {
        if (PdhOpenQueryW(nullptr, 0, &query_) == ERROR_SUCCESS) {
            PdhAddEnglishCounterW(query_, LR"(\PhysicalDisk(_Total)\Disk Read Bytes/sec)", 0, &read_);
            PdhAddEnglishCounterW(query_, LR"(\PhysicalDisk(_Total)\Disk Write Bytes/sec)", 0, &write_);
        }
    }

    ~DiskRates() {
        if (query_)
            PdhCloseQuery(query_);
    }

    void sample(Snapshot& snapshot) {
        if (query_ && PdhCollectQueryData(query_) == ERROR_SUCCESS) {
            snapshot[Metric::DiskRead] = value(read_);
            snapshot[Metric::DiskWrite] = value(write_);
        }
    }

private:
    static double value(PDH_HCOUNTER counter) {
        PDH_FMT_COUNTERVALUE data{};
        if (counter &&
            PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, nullptr, &data) == ERROR_SUCCESS &&
            (data.CStatus == PDH_CSTATUS_VALID_DATA || data.CStatus == PDH_CSTATUS_NEW_DATA) &&
            data.doubleValue >= 0) {
            return data.doubleValue;
        }
        return missing;
    }

    PDH_HQUERY query_{};
    PDH_HCOUNTER read_{};
    PDH_HCOUNTER write_{};
};

class NvidiaHotspot {
    using QueryInterface = void* (*)(unsigned int);
    using Initialize = int (*)();
    using Unload = int (*)();
    using Enumerate = int (*)(void**, unsigned int*);
    struct ThermalSensors {
        uint32_t version;
        uint32_t mask;
        int32_t reserved[8];
        int32_t temperatures[32];
    };
    static_assert(sizeof(ThermalSensors) == 168);
    using ReadThermalSensors = int (*)(void*, ThermalSensors*);

public:
    NvidiaHotspot() {
        module_ = LoadLibraryExW(L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_)
            return;
        const auto query = resolve<QueryInterface>(module_, "nvapi_QueryInterface");
        if (!query)
            return;
        const auto initialize = std::bit_cast<Initialize>(query(0x0150E828));
        unload_ = std::bit_cast<Unload>(query(0xD22BDD7E));
        const auto enumerate = std::bit_cast<Enumerate>(query(0xE5AC921F));
        // The sensor layout used by LibreHardwareMonitor on Ampere and Ada.
        read_ = std::bit_cast<ReadThermalSensors>(query(0x65FE3AAD));
        if (!initialize || !unload_ || !enumerate || !read_ || initialize() != 0)
            return;
        initialized_ = true;
        std::array<void*, 64> devices{};
        unsigned int count{};
        if (enumerate(devices.data(), &count) == 0 && count == 1)
            device_ = devices.front();
    }

    ~NvidiaHotspot() {
        if (initialized_)
            unload_();
        if (module_)
            FreeLibrary(module_);
    }

    double sample() const {
        if (!device_)
            return missing;
        ThermalSensors sensors{};
        sensors.version = sizeof(ThermalSensors) | (2u << 24);
        sensors.mask = 1u << 1;
        if (read_(device_, &sensors) != 0)
            return missing;
        const double celsius = sensors.temperatures[1] / 256.0;
        return celsius > 0 && celsius <= 150 ? celsius : missing;
    }

private:
    HMODULE module_{};
    void* device_{};
    Unload unload_{};
    ReadThermalSensors read_{};
    bool initialized_{};
};

class NvidiaGpu {
    using Init = int (*)();
    using Shutdown = int (*)();
    using Device = void*;
    using GetDevice = int (*)(unsigned int, Device*);
    using GetTemperature = int (*)(Device, unsigned int, unsigned int*);
    using GetUnsigned = int (*)(Device, unsigned int*);
    using GetEngineUtilization = int (*)(Device, unsigned int*, unsigned int*);
    struct Utilization { unsigned int gpu, memory; };
    using GetUtilization = int (*)(Device, Utilization*);
    struct Memory { unsigned long long total, free, used; };
    using GetMemory = int (*)(Device, Memory*);
    using GetClock = int (*)(Device, unsigned int, unsigned int*);
    using GetFanPercent = int (*)(Device, unsigned int*);
    using GetFanPercentByIndex = int (*)(Device, unsigned int, unsigned int*);
    struct FanSpeed { unsigned int version, fan, speed; };
    using GetFanRpm = int (*)(Device, FanSpeed*);
    using GetClockReasons = int (*)(Device, unsigned long long*);
    using GetDeviceCount = int (*)(unsigned int*);
    using GetArchitecture = int (*)(Device, unsigned int*);

public:
    NvidiaGpu() {
        module_ = LoadLibraryExW(L"nvml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_)
            return;
        const auto init = resolve<Init>(module_, "nvmlInit_v2");
        shutdown_ = resolve<Shutdown>(module_, "nvmlShutdown");
        const auto getDevice = resolve<GetDevice>(module_, "nvmlDeviceGetHandleByIndex_v2");
        if (!init || !shutdown_ || !getDevice || init() != 0)
            return;
        initialized_ = true;
        if (getDevice(0, &device_) != 0) {
            device_ = nullptr;
            return;
        }
        getTemperature_ = resolve<GetTemperature>(module_, "nvmlDeviceGetTemperature");
        getPower_ = resolve<GetUnsigned>(module_, "nvmlDeviceGetPowerUsage");
        getPowerLimit_ = resolve<GetUnsigned>(module_, "nvmlDeviceGetEnforcedPowerLimit");
        getPerformanceState_ = resolve<GetUnsigned>(module_, "nvmlDeviceGetPerformanceState");
        getPcieGeneration_ = resolve<GetUnsigned>(module_, "nvmlDeviceGetCurrPcieLinkGeneration");
        getPcieWidth_ = resolve<GetUnsigned>(module_, "nvmlDeviceGetCurrPcieLinkWidth");
        getEncoderUtilization_ = resolve<GetEngineUtilization>(module_, "nvmlDeviceGetEncoderUtilization");
        getDecoderUtilization_ = resolve<GetEngineUtilization>(module_, "nvmlDeviceGetDecoderUtilization");
        getUtilization_ = resolve<GetUtilization>(module_, "nvmlDeviceGetUtilizationRates");
        getMemory_ = resolve<GetMemory>(module_, "nvmlDeviceGetMemoryInfo");
        getClock_ = resolve<GetClock>(module_, "nvmlDeviceGetClockInfo");
        getFanPercent_ = resolve<GetFanPercent>(module_, "nvmlDeviceGetFanSpeed");
        getFanPercentByIndex_ = resolve<GetFanPercentByIndex>(module_, "nvmlDeviceGetFanSpeed_v2");
        getFanRpm_ = resolve<GetFanRpm>(module_, "nvmlDeviceGetFanSpeedRPM");
        getClockReasons_ = resolve<GetClockReasons>(module_, "nvmlDeviceGetCurrentClocksEventReasons");
        getThrottleReasons_ = resolve<GetClockReasons>(module_, "nvmlDeviceGetCurrentClocksThrottleReasons");
        getDeviceCount_ = resolve<GetDeviceCount>(module_, "nvmlDeviceGetCount_v2");
        getArchitecture_ = resolve<GetArchitecture>(module_, "nvmlDeviceGetArchitecture");
    }

    std::wstring model() const {
        if (!device_) return {};
        using GetName = int (*)(Device, char*, unsigned int);
        const auto getName = resolve<GetName>(module_, "nvmlDeviceGetName");
        char name[256]{};
        if (!getName || getName(device_, name, sizeof(name)) != 0) return {};
        name[sizeof(name) - 1] = 0;
        return modelText(name);
    }

    GpuSpecifications specifications() const {
        GpuSpecifications result;
        if (!device_)
            return result;

        const auto readNumber = [&](const char* symbol) -> double {
            const auto query = resolve<GetUnsigned>(module_, symbol);
            unsigned int value{};
            return query && query(device_, &value) == 0 ? value : missing;
        };
        const auto readText = [&](const char* symbol) -> std::wstring {
            using GetText = int (*)(Device, char*, unsigned int);
            const auto query = resolve<GetText>(module_, symbol);
            char value[256]{};
            if (!query || query(device_, value, sizeof(value)) != 0)
                return {};
            value[sizeof(value) - 1] = 0;
            return modelText(value);
        };

        result.cores = readNumber("nvmlDeviceGetNumGpuCores");
        result.memoryBusWidth = readNumber("nvmlDeviceGetMemoryBusWidth");
        result.defaultPowerLimitWatts = readNumber("nvmlDeviceGetPowerManagementDefaultLimit") / 1000.0;
        result.maxPcieGeneration = readNumber("nvmlDeviceGetGpuMaxPcieLinkGeneration");
        result.hostMaxPcieGeneration = readNumber("nvmlDeviceGetMaxPcieLinkGeneration");
        result.hostMaxPcieWidth = readNumber("nvmlDeviceGetMaxPcieLinkWidth");
        result.vbios = readText("nvmlDeviceGetVbiosVersion");
        result.boardPartNumber = readText("nvmlDeviceGetBoardPartNumber");
        result.serial = readText("nvmlDeviceGetSerial");

        Memory memory{};
        if (getMemory_ && getMemory_(device_, &memory) == 0)
            result.memoryCapacity = static_cast<double>(memory.total);

        using GetCapability = int (*)(Device, int*, int*);
        const auto getCapability = resolve<GetCapability>(module_, "nvmlDeviceGetCudaComputeCapability");
        int major{}, minor{};
        if (getCapability && getCapability(device_, &major, &minor) == 0)
            result.cudaCapability = std::to_wstring(major) + L"." + std::to_wstring(minor);

        // Indexed by the values of NVML_DEVICE_ARCH_* in nvml.h.
        static constexpr std::array architectures{L"", L"", L"Kepler", L"Maxwell", L"Pascal",
            L"Volta", L"Turing", L"Ampere", L"Ada Lovelace", L"Hopper", L"Blackwell",
            L"DLA", L"DLA2", L"Rubin", L"", L"NPU3"};
        unsigned int architecture{};
        if (getArchitecture_ && getArchitecture_(device_, &architecture) == 0 && architecture < architectures.size())
            result.architecture = architectures[architecture];

        return result;
    }

    ~NvidiaGpu() {
        hotspot_.reset();
        if (initialized_)
            shutdown_();
        if (module_)
            FreeLibrary(module_);
    }

    void sample(Snapshot& snapshot) {
        if (!device_)
            return;
        unsigned int value{};
        if (getTemperature_ && getTemperature_(device_, 0, &value) == 0)
            snapshot[Metric::GpuTemp] = value;
        if (getPower_ && getPower_(device_, &value) == 0)
            snapshot[Metric::GpuWatts] = value / 1000.0;
        Utilization utilization{};
        if (getUtilization_ && getUtilization_(device_, &utilization) == 0)
            snapshot[Metric::GpuPercent] = utilization.gpu;
        Memory memory{};
        if (getMemory_ && getMemory_(device_, &memory) == 0) {
            snapshot[Metric::GpuMemoryUsed] = static_cast<double>(memory.used);
            snapshot[Metric::GpuMemoryTotal] = static_cast<double>(memory.total);
        }
    }

    void details(HardwareDetails& details) {
        if (!device_)
            return;
        Utilization utilization{};
        if (getUtilization_ && getUtilization_(device_, &utilization) == 0)
            details.gpuMemoryActivity = utilization.memory;
        unsigned int value{}, samplingPeriodUs{};
        if (getEncoderUtilization_ && getEncoderUtilization_(device_, &value, &samplingPeriodUs) == 0)
            details.gpuEncoderPercent = value;
        if (getDecoderUtilization_ && getDecoderUtilization_(device_, &value, &samplingPeriodUs) == 0)
            details.gpuDecoderPercent = value;
        if (getPowerLimit_ && getPowerLimit_(device_, &value) == 0)
            details.gpuPowerLimitWatts = value / 1000.0;
        if (getPerformanceState_ && getPerformanceState_(device_, &value) == 0 && value <= 15)
            details.gpuPerformanceState = value;
        if (getPcieGeneration_ && getPcieGeneration_(device_, &value) == 0)
            details.gpuPcieGeneration = value;
        if (getPcieWidth_ && getPcieWidth_(device_, &value) == 0)
            details.gpuPcieWidth = value;

        constexpr unsigned int graphicsClock = 0; // NVML_CLOCK_GRAPHICS
        constexpr unsigned int memoryClock = 2;   // NVML_CLOCK_MEM
        if (getClock_ && getClock_(device_, graphicsClock, &value) == 0)
            details.gpuCoreMhz = value;
        if (getClock_ && getClock_(device_, memoryClock, &value) == 0)
            details.gpuMemoryMhz = value;

        // Pair the percentage and RPM for the same fan when per-fan queries are available.
        if ((getFanPercentByIndex_ && getFanPercentByIndex_(device_, 0, &value) == 0) ||
            (getFanPercent_ && getFanPercent_(device_, &value) == 0)) {
            details.gpuFanPercent = value;
        }
        constexpr unsigned int fanSpeedVersion = sizeof(FanSpeed) | (1u << 24);
        FanSpeed fan{fanSpeedVersion, 0, 0};
        if (getFanRpm_ && getFanRpm_(device_, &fan) == 0)
            details.gpuFanRpm = fan.speed;

        unsigned long long reasons{};
        if ((getClockReasons_ && getClockReasons_(device_, &reasons) == 0) ||
            (getThrottleReasons_ && getThrottleReasons_(device_, &reasons) == 0)) {
            constexpr unsigned long long softwarePowerCap = 0x04;
            constexpr unsigned long long softwareThermalSlowdown = 0x20;
            constexpr unsigned long long hardwareThermalSlowdown = 0x40;
            constexpr unsigned long long hardwarePowerBrake = 0x80;
            details.gpuLimitsAvailable = true;
            details.gpuThermalLimited = (reasons & (softwareThermalSlowdown | hardwareThermalSlowdown)) != 0;
            details.gpuPowerLimited = (reasons & (softwarePowerCap | hardwarePowerBrake)) != 0;
        }

        if (!hotspotChecked_) {
            hotspotChecked_ = true;
            unsigned int architecture{}, count{};
            constexpr unsigned int ampere = 7, ada = 8;
            // NVAPI and NVML can enumerate in different orders. The current
            // dashboard only combines their results with one device in each.
            if (getDeviceCount_ && getDeviceCount_(&count) == 0 && count == 1 &&
                getArchitecture_ && getArchitecture_(device_, &architecture) == 0 &&
                (architecture == ampere || architecture == ada)) {
                hotspot_ = std::make_unique<NvidiaHotspot>();
            }
        }
        if (hotspot_)
            details.gpuHotspotC = hotspot_->sample();
    }

    void stopDetails() {
        hotspot_.reset();
        hotspotChecked_ = false;
    }

private:
    HMODULE module_{};
    Device device_{};
    bool initialized_{};
    Shutdown shutdown_{};
    GetTemperature getTemperature_{};
    GetUnsigned getPower_{};
    GetUnsigned getPowerLimit_{};
    GetUnsigned getPerformanceState_{};
    GetUnsigned getPcieGeneration_{};
    GetUnsigned getPcieWidth_{};
    GetEngineUtilization getEncoderUtilization_{};
    GetEngineUtilization getDecoderUtilization_{};
    GetUtilization getUtilization_{};
    GetMemory getMemory_{};
    GetClock getClock_{};
    GetFanPercent getFanPercent_{};
    GetFanPercentByIndex getFanPercentByIndex_{};
    GetFanRpm getFanRpm_{};
    GetClockReasons getClockReasons_{};
    GetClockReasons getThrottleReasons_{};
    GetDeviceCount getDeviceCount_{};
    GetArchitecture getArchitecture_{};
    bool hotspotChecked_{};
    std::unique_ptr<NvidiaHotspot> hotspot_;
};

class CoreDetails {
    struct Readings {
        double load = missing;
        double frequency = missing;
        double performance = missing;
        double percentMaximumFrequency = missing;
    };

public:
    CoreDetails() {
        const WORD groupCount = GetActiveProcessorGroupCount();
        for (WORD group = 0; group < groupCount; ++group) {
            groupOffsets_.push_back(cores_.size());
            const auto processorCount = GetActiveProcessorCount(group);
            for (DWORD number = 0; number < processorCount; ++number)
                cores_.push_back({group, number});
        }
        groupOffsets_.push_back(cores_.size());
        readings_.resize(cores_.size());
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS)
            return;
        PdhAddEnglishCounterW(query_, LR"(\Processor Information(*)\% Processor Time)", 0, &load_);
        PdhAddEnglishCounterW(query_, LR"(\Processor Information(*)\Processor Frequency)", 0, &frequency_);
        PdhAddEnglishCounterW(query_, LR"(\Processor Information(*)\% Processor Performance)", 0, &performance_);
        PdhAddEnglishCounterW(query_, LR"(\Processor Information(*)\% of Maximum Frequency)", 0, &percentMaximum_);
    }

    ~CoreDetails() {
        if (query_)
            PdhCloseQuery(query_);
    }

    void sample(HardwareDetails& details) {
        details.cores = cores_;
        if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS)
            return;
        std::fill(readings_.begin(), readings_.end(), Readings{});
        read(load_, &Readings::load);
        read(frequency_, &Readings::frequency);
        read(performance_, &Readings::performance);
        read(percentMaximum_, &Readings::percentMaximumFrequency);
        for (size_t index = 0; index < details.cores.size(); ++index) {
            const auto& data = readings_[index];
            auto& core = details.cores[index];
            if (data.load >= 0)
                core.loadPercent = std::min(data.load, 100.0);
            if (data.frequency > 0) {
                core.frequencyMhz = data.frequency;
                // Processor Frequency can be the requested P-state value.
                // Use measured performance and nominal frequency to reflect turbo.
                if (data.performance > 0 && data.percentMaximumFrequency > 0)
                    core.frequencyMhz *= data.performance / data.percentMaximumFrequency;
            }
        }
    }

private:
    void read(PDH_HCOUNTER counter, double Readings::* field) {
        if (!counter)
            return;
        DWORD count{};
        DWORD size = static_cast<DWORD>(buffer_.size());
        constexpr DWORD format = PDH_FMT_DOUBLE | PDH_FMT_NOCAP100;
        const auto items = [this] { return reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data()); };
        auto status = PdhGetFormattedCounterArrayW(counter, format, &size, &count, items());
        if (status == static_cast<PDH_STATUS>(PDH_MORE_DATA)) {
            buffer_.resize(size);
            status = PdhGetFormattedCounterArrayW(counter, format, &size, &count, items());
        }
        if (status != ERROR_SUCCESS)
            return;
        for (DWORD index = 0; index < count; ++index) {
            const auto& item = items()[index];
            const auto& value = item.FmtValue;
            if (!item.szName ||
                (value.CStatus != PDH_CSTATUS_VALID_DATA && value.CStatus != PDH_CSTATUS_NEW_DATA)) {
                continue;
            }
            wchar_t* separator{};
            const auto group = std::wcstoul(item.szName, &separator, 10);
            if (separator == item.szName || *separator != L',' || group >= groupOffsets_.size() - 1)
                continue;
            wchar_t* end{};
            const auto number = std::wcstoul(separator + 1, &end, 10);
            const auto start = groupOffsets_[group];
            if (end != separator + 1 && *end == L'\0' && number < groupOffsets_[group + 1] - start)
                readings_[start + number].*field = value.doubleValue;
        }
    }

    PDH_HQUERY query_{};
    PDH_HCOUNTER load_{};
    PDH_HCOUNTER frequency_{};
    PDH_HCOUNTER performance_{};
    PDH_HCOUNTER percentMaximum_{};
    std::vector<size_t> groupOffsets_;
    std::vector<CoreDetail> cores_;
    std::vector<Readings> readings_;
    std::vector<std::byte> buffer_;
};

// SystemProcessInformation supplies all processes in one kernel query. The SDK
// structure hides the timestamps as reserved fields on some toolchains.
struct ProcessInformation {
    ULONG nextEntryOffset;
    ULONG threadCount;
    LARGE_INTEGER reserved[3];
    LARGE_INTEGER creationTime;
    LARGE_INTEGER userTime;
    LARGE_INTEGER kernelTime;
    UNICODE_STRING imageName;
    LONG basePriority;
    HANDLE pid;
    HANDLE parentPid;
    ULONG handleCount;
    ULONG sessionId;
    void* reservedPointer;
    SIZE_T peakVirtualSize;
    SIZE_T virtualSize;
    ULONG pageFaultCount;
    SIZE_T peakWorkingSetSize;
    SIZE_T workingSetSize;
};

static_assert(offsetof(ProcessInformation, imageName) == offsetof(SYSTEM_PROCESS_INFORMATION, ImageName));
static_assert(offsetof(ProcessInformation, pid) == offsetof(SYSTEM_PROCESS_INFORMATION, UniqueProcessId));
static_assert(offsetof(ProcessInformation, workingSetSize) == (sizeof(void*) == 8 ? 144 : 104));

class Processes {
    using Query = NTSTATUS (NTAPI*)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);
    struct Previous {
        LONGLONG created;
        LONGLONG cpuTime;
        uint64_t generation;
    };
    struct Candidate {
        const UNICODE_STRING* name;
        uint32_t pid;
        double cpu;
        uint64_t memory;
    };

public:
    Processes()
        : query_(resolve<Query>(GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation")),
          buffer_(64 * 1024),
          processorCount_(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)) {}

    void sample(Snapshot& snapshot, TimePoint now) {
        const auto length = queryProcesses();
        if (length < sizeof(ProcessInformation))
            return;
        const double capacity = seconds(lastSample_, now) * processorCount_ * 1e7;
        candidates_.clear();
        ++generation_;
        for (size_t offset = 0; offset <= length - sizeof(ProcessInformation);) {
            const auto* info = reinterpret_cast<const ProcessInformation*>(buffer_.data() + offset);
            const auto pid = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(info->pid));
            if (pid != 0) {
                const auto cpuTime = info->userTime.QuadPart + info->kernelTime.QuadPart;
                const auto created = info->creationTime.QuadPart;
                auto [entry, inserted] = previous_.try_emplace(pid, Previous{created, cpuTime, generation_});
                double cpu = missing;
                if (!inserted && entry->second.created == created &&
                    capacity > 0 && cpuTime >= entry->second.cpuTime) {
                    cpu = std::min(100.0, (cpuTime - entry->second.cpuTime) * 100.0 / capacity);
                }
                entry->second = {created, cpuTime, generation_};
                candidates_.push_back({&info->imageName, pid, cpu, info->workingSetSize});
            }
            if (!info->nextEntryOffset || info->nextEntryOffset < sizeof(ProcessInformation) ||
                info->nextEntryOffset % alignof(ProcessInformation) != 0 ||
                info->nextEntryOffset > length - offset) {
                break;
            }
            offset += info->nextEntryOffset;
        }

        std::erase_if(previous_, [this](const auto& entry) {
            return entry.second.generation != generation_;
        });
        lastSample_ = now;
        rank(snapshot.byMemory, [](const Candidate& first, const Candidate& second) {
            return first.memory != second.memory ? first.memory > second.memory : first.pid < second.pid;
        });
        std::erase_if(candidates_, [](const Candidate& candidate) { return !(candidate.cpu >= 0); });
        rank(snapshot.byCpu, [](const Candidate& first, const Candidate& second) {
            return first.cpu != second.cpu ? first.cpu > second.cpu : first.pid < second.pid;
        });
    }

private:
    size_t queryProcesses() {
        if (!query_)
            return 0;
        constexpr ULONG lengthMismatch = 0xC0000004;
        constexpr size_t bufferLimit = 32 * 1024 * 1024;
        for (;;) {
            ULONG needed{};
            const auto status = query_(SystemProcessInformation, buffer_.data(),
                                       static_cast<ULONG>(buffer_.size()), &needed);
            if (status >= 0)
                return needed <= buffer_.size() ? needed : 0;
            if (static_cast<ULONG>(status) != lengthMismatch)
                return 0;
            const auto capacity = std::max(buffer_.size() * 2, static_cast<size_t>(needed) + 4096);
            if (capacity > bufferLimit)
                return 0;
            buffer_.resize(capacity);
        }
    }

    template <typename Compare>
    void rank(std::array<ProcessEntry, TopCount>& output, Compare compare) {
        const size_t count = std::min(output.size(), candidates_.size());
        std::partial_sort(candidates_.begin(), candidates_.begin() + count, candidates_.end(), compare);
        for (size_t index = 0; index < count; ++index) {
            const auto& item = candidates_[index];
            auto& result = output[index];
            result.pid = item.pid;
            result.cpu = item.cpu;
            result.memory = item.memory;
            if (item.name->Buffer && item.name->Length >= sizeof(wchar_t)) {
                size_t length = std::min(item.name->Length / sizeof(wchar_t), std::size(result.name) - 1);
                const wchar_t last = item.name->Buffer[length - 1];
                if (last >= 0xD800 && last <= 0xDBFF)
                    --length;
                std::copy_n(item.name->Buffer, length, result.name);
                result.name[length] = L'\0';
            } else {
                std::wcscpy(result.name, item.pid == 4 ? L"System" : L"(unknown)");
            }
        }
    }

    Query query_;
    std::vector<std::byte> buffer_;
    DWORD processorCount_;
    std::unordered_map<uint32_t, Previous> previous_;
    std::vector<Candidate> candidates_;
    TimePoint lastSample_{};
    uint64_t generation_{};
};

std::wstring networkAddress(const SOCKET_ADDRESS& address) {
    if (!address.lpSockaddr || address.iSockaddrLength < static_cast<int>(sizeof(ADDRESS_FAMILY)))
        return {};
    const auto length = static_cast<size_t>(address.iSockaddrLength);
    wchar_t text[INET6_ADDRSTRLEN]{};
    if (address.lpSockaddr->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
        const auto& ipv4 = *reinterpret_cast<const sockaddr_in*>(address.lpSockaddr);
        if (InetNtopW(AF_INET, &ipv4.sin_addr, text, INET6_ADDRSTRLEN))
            return text;
    } else if (address.lpSockaddr->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
        const auto& ipv6 = *reinterpret_cast<const sockaddr_in6*>(address.lpSockaddr);
        if (InetNtopW(AF_INET6, &ipv6.sin6_addr, text, INET6_ADDRSTRLEN)) {
            std::wstring result = text;
            if (ipv6.sin6_scope_id)
                result += L"%" + std::to_wstring(ipv6.sin6_scope_id);
            return result;
        }
    }
    return {};
}

template <typename Address>
void appendNetworkAddresses(const Address* entry, std::vector<std::wstring>& addresses) {
    for (; entry; entry = entry->Next) {
        auto address = networkAddress(entry->Address);
        if (!address.empty())
            addresses.push_back(std::move(address));
    }
}

std::optional<NetworkDetails> readNetworkDetails() {
    MIB_IF_TABLE2* table{};
    if (GetIfTable2(&table) != NO_ERROR)
        return std::nullopt;
    const std::unique_ptr<MIB_IF_TABLE2, decltype(&FreeMibTable)> owner(table, FreeMibTable);

    NetworkDetails result;
    ULONG size = 15 * 1024;
    std::vector<std::byte> buffer(size);
    IP_ADAPTER_ADDRESSES* adapters{};
    for (int attempt = 0; attempt < 3; ++attempt) {
        adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        const ULONG status = GetAdaptersAddresses(AF_UNSPEC, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, adapters, &size);
        result.addressesAvailable = status == NO_ERROR || status == ERROR_NO_DATA;
        if (status == NO_ERROR)
            break;
        adapters = nullptr;
        if (status != ERROR_BUFFER_OVERFLOW || attempt == 2)
            break;
        buffer.resize(size);
    }
    std::unordered_map<uint64_t, const IP_ADAPTER_ADDRESSES*> byLuid;
    for (auto* adapter = adapters; adapter; adapter = adapter->Next)
        byLuid.emplace(adapter->Luid.Value, adapter);

    for (ULONG index = 0; index < table->NumEntries; ++index) {
        const auto& row = table->Table[index];
        const auto flags = row.InterfaceAndOperStatusFlags;
        if (flags.FilterInterface || row.Type == IF_TYPE_SOFTWARE_LOOPBACK)
            continue;
        if (flags.HardwareInterface)
            ++result.physicalAdapters;
        else
            ++result.virtualAdapters;
        if (row.OperStatus != IfOperStatusUp)
            continue;

        NetworkConnection connection;
        connection.name = row.Alias;
        connection.description = row.Description;
        connection.physical = flags.HardwareInterface;
        if (row.ReceiveLinkSpeed && row.ReceiveLinkSpeed != std::numeric_limits<ULONG64>::max())
            connection.receiveBitsPerSecond = static_cast<double>(row.ReceiveLinkSpeed);
        if (row.TransmitLinkSpeed && row.TransmitLinkSpeed != std::numeric_limits<ULONG64>::max())
            connection.transmitBitsPerSecond = static_cast<double>(row.TransmitLinkSpeed);
        if (row.PhysicalAddressLength <= sizeof(row.PhysicalAddress)) {
            constexpr wchar_t hex[] = L"0123456789ABCDEF";
            for (ULONG byte = 0; byte < row.PhysicalAddressLength; ++byte) {
                if (byte) connection.macAddress += L'-';
                connection.macAddress += hex[row.PhysicalAddress[byte] >> 4];
                connection.macAddress += hex[row.PhysicalAddress[byte] & 0xf];
            }
        }
        if (const auto entry = byLuid.find(row.InterfaceLuid.Value); entry != byLuid.end()) {
            const auto& adapter = *entry->second;
            for (auto* ip = adapter.FirstUnicastAddress; ip; ip = ip->Next) {
                auto address = networkAddress(ip->Address);
                if (address.empty()) continue;
                auto& addresses = ip->Address.lpSockaddr->sa_family == AF_INET ? connection.ipv4 : connection.ipv6;
                addresses.push_back(std::move(address));
            }
            appendNetworkAddresses(adapter.FirstGatewayAddress, connection.gateways);
            appendNetworkAddresses(adapter.FirstDnsServerAddress, connection.dnsServers);
        }
        // Internal miniports can report Up without carrying an IP connection.
        if (connection.physical || !connection.ipv4.empty() || !connection.ipv6.empty())
            result.connections.push_back(std::move(connection));
    }
    std::stable_sort(result.connections.begin(), result.connections.end(), [](const auto& left, const auto& right) {
        if (left.gateways.empty() != right.gateways.empty()) return !left.gateways.empty();
        if (left.physical != right.physical) return left.physical;
        return left.name < right.name;
    });
    return result;
}

class Network {
    struct Count {
        uint64_t down;
        uint64_t up;
        uint64_t generation;
    };

public:
    void sample(Snapshot& snapshot, TimePoint now) {
        MIB_IF_TABLE2* table{};
        if (GetIfTable2(&table) != NO_ERROR)
            return;
        const std::unique_ptr<MIB_IF_TABLE2, decltype(&FreeMibTable)> owner(table, FreeMibTable);
        const double elapsed = seconds(lastSample_, now);
        uint64_t down{}, up{};
        ++generation_;
        for (ULONG index = 0; index < table->NumEntries; ++index) {
            const auto& row = table->Table[index];
            // Count physical links once, including traffic carried by tunnels.
            if (!row.InterfaceAndOperStatusFlags.HardwareInterface ||
                row.InterfaceAndOperStatusFlags.FilterInterface ||
                row.Type == IF_TYPE_SOFTWARE_LOOPBACK || row.OperStatus != IfOperStatusUp) {
                continue;
            }
            const Count current{row.InOctets, row.OutOctets, generation_};
            auto [entry, inserted] = previous_.try_emplace(row.InterfaceLuid.Value, current);
            if (!inserted) {
                if (current.down >= entry->second.down)
                    down += current.down - entry->second.down;
                if (current.up >= entry->second.up)
                    up += current.up - entry->second.up;
                entry->second = current;
            }
        }
        std::erase_if(previous_, [this](const auto& entry) {
            return entry.second.generation != generation_;
        });
        if (lastSample_ != TimePoint{} && elapsed > 0) {
            snapshot[Metric::NetworkDown] = down / elapsed;
            snapshot[Metric::NetworkUp] = up / elapsed;
        }
        lastSample_ = now;
    }

private:
    std::unordered_map<uint64_t, Count> previous_;
    TimePoint lastSample_{};
    uint64_t generation_{};
};

}  // namespace

struct Telemetry::Impl {
    HardwareInventory inventory() const {
        HardwareInventory result;
        readFirmwareModels(result);
        auto chipset = chipsetDevices();
        if (!chipset.fields.empty()) result.board.push_back(std::move(chipset));
        ULONGLONG installedKiB{};
        if (GetPhysicallyInstalledSystemMemory(&installedKiB)) {
            result.installedMemory = installedKiB * 1024.0;
        }
        if (result.processors.empty()) result.processors.push_back(windowsProcessorName());
        else if (result.processors.front().empty()) result.processors.front() = windowsProcessorName();
        for (auto& name : result.processors) if (name.empty()) name = L"型号未提供";

        result.disks = readDiskModels();
        result.graphics = graphicsModels();
        result.sampledGpu = gpu.model();
        result.gpu = gpu.specifications();
        if (result.graphics.empty() && !result.sampledGpu.empty()) {
            result.graphics.push_back(result.sampledGpu);
        }
        return result;
    }

    Snapshot sample() {
        Snapshot snapshot{};
        snapshot.timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto now = Clock::now();
        cpuUsage(snapshot);
        memory(snapshot);
        diskCapacity(snapshot, now);
        diskRates.sample(snapshot);
        network.sample(snapshot, now);
        processes.sample(snapshot, now);
        gpu.sample(snapshot);
        return snapshot;
    }

    HardwareDetails details(DetailPane pane, bool keepAlive) {
        HardwareDetails result;
        result.pane = pane;
        if (pane == DetailPane::Cpu) {
            if (!coreDetails)
                coreDetails = std::make_unique<CoreDetails>();
            coreDetails->sample(result);
        } else if (!keepAlive) {
            coreDetails.reset();
        }
        if (pane == DetailPane::Gpu)
            gpu.details(result);
        else if (!keepAlive)
            gpu.stopDetails();
        if (pane == DetailPane::Network)
            result.network = readNetworkDetails();
        if (pane == DetailPane::Memory) {
            PERFORMANCE_INFORMATION info{};
            if (GetPerformanceInfo(&info, sizeof(info))) {
                const auto page = static_cast<double>(info.PageSize);
                result.memory = {info.SystemCache * page, info.CommitTotal * page, info.CommitLimit * page,
                    info.CommitPeak * page, info.KernelPaged * page, info.KernelNonpaged * page, page};
            }
        }
        return result;
    }

    void cpuUsage(Snapshot& snapshot) {
        FILETIME idle{}, kernel{}, user{};
        if (!GetSystemTimes(&idle, &kernel, &user))
            return;
        const auto idleTicks = ticks(idle);
        const auto totalTicks = ticks(kernel) + ticks(user);
        if (previousTotal && totalTicks > previousTotal && idleTicks >= previousIdle) {
            const auto elapsed = totalTicks - previousTotal;
            const auto idleElapsed = std::min(elapsed, idleTicks - previousIdle);
            snapshot[Metric::CpuPercent] = (elapsed - idleElapsed) * 100.0 / elapsed;
        }
        previousTotal = totalTicks;
        previousIdle = idleTicks;
    }

    static void memory(Snapshot& snapshot) {
        MEMORYSTATUSEX data{};
        data.dwLength = sizeof(data);
        if (GlobalMemoryStatusEx(&data) && data.ullTotalPhys > 0) {
            const auto used = data.ullTotalPhys - data.ullAvailPhys;
            snapshot[Metric::MemoryUsed] = static_cast<double>(used);
            snapshot[Metric::MemoryTotal] = static_cast<double>(data.ullTotalPhys);
            snapshot[Metric::MemoryPercent] = used * 100.0 / data.ullTotalPhys;
        }
    }

    void diskCapacity(Snapshot& snapshot, TimePoint now) {
        if (now >= nextDiskCapacity) {
            nextDiskCapacity = now + std::chrono::seconds(30);
            diskUsed = missing;
            diskTotal = missing;
            uint64_t used{}, total{};
            std::set<std::wstring> counted;
            const DWORD drives = GetLogicalDrives();
            for (unsigned int index = 0; index < 26; ++index) {
                wchar_t drive[] = {static_cast<wchar_t>(L'A' + index), L':', L'\\', L'\0'};
                if ((drives & (1u << index)) == 0 || GetDriveTypeW(drive) != DRIVE_FIXED)
                    continue;
                wchar_t volume[64]{};
                if (!GetVolumeNameForVolumeMountPointW(drive, volume, std::size(volume)) ||
                    counted.contains(volume)) {
                    continue;
                }
                ULARGE_INTEGER capacity{}, free{};
                if (!GetDiskFreeSpaceExW(volume, nullptr, &capacity, &free) || free.QuadPart > capacity.QuadPart)
                    continue;
                counted.emplace(volume);
                total += capacity.QuadPart;
                used += capacity.QuadPart - free.QuadPart;
            }
            if (total > 0) {
                diskTotal = static_cast<double>(total);
                diskUsed = static_cast<double>(used);
            }
        }
        snapshot[Metric::DiskUsed] = diskUsed;
        snapshot[Metric::DiskTotal] = diskTotal;
    }

    DiskRates diskRates;
    Network network;
    Processes processes;
    NvidiaGpu gpu;
    std::unique_ptr<CoreDetails> coreDetails;
    uint64_t previousIdle{}, previousTotal{};
    TimePoint nextDiskCapacity{};
    double diskUsed{missing};
    double diskTotal{missing};
};

struct CpuSensors::Impl {
    using Open = HRESULT (WINAPI*)(HANDLE*);
    using Load = HRESULT (WINAPI*)(HANDLE, const unsigned char*, size_t);
    using Close = HRESULT (WINAPI*)(HANDLE);
    using Execute = HRESULT (WINAPI*)(HANDLE, const char*, const uint64_t*, size_t,
                                      uint64_t*, size_t, size_t*);

    Impl() {
        const auto executable = GetModuleHandleW(nullptr);
        const auto resource = FindResourceW(executable, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));
        if (!resource)
            return;
        const auto bytes = static_cast<const unsigned char*>(LockResource(LoadResource(executable, resource)));
        const auto size = SizeofResource(executable, resource);
        if (!bytes || !size)
            return;

        wchar_t installDirectory[1024]{};
        DWORD pathBytes = sizeof(installDirectory);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO)",
                         L"InstallLocation", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                         nullptr, installDirectory, &pathBytes) != ERROR_SUCCESS ||
            installDirectory[0] == L'\0') {
            return;
        }
        const std::wstring library = std::wstring(installDirectory) + L"\\PawnIOLib.dll";
        module = LoadLibraryExW(library.c_str(), nullptr,
                                LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module)
            return;
        const auto open = resolve<Open>(module, "pawnio_open");
        const auto load = resolve<Load>(module, "pawnio_load");
        close = resolve<Close>(module, "pawnio_close");
        execute = resolve<Execute>(module, "pawnio_execute");
        if (!open || !load || !close || !execute || FAILED(open(&handle)))
            return;
        if (FAILED(load(handle, bytes, size))) {
            close(handle);
            handle = nullptr;
            return;
        }

        uint64_t raw{};
        if (read(0x1A2, raw))
            tjMax = static_cast<unsigned int>((raw >> 16) & 0xFF);
        if (read(0x606, raw))
            joulesPerTick = 1.0 / (uint64_t{1} << ((raw >> 8) & 0x1F));
    }

    ~Impl() {
        if (handle)
            close(handle);
        if (module)
            FreeLibrary(module);
    }

    bool read(uint64_t address, uint64_t& result) const {
        size_t returned{};
        return handle && SUCCEEDED(execute(handle, "ioctl_read_msr", &address, 1, &result, 1, &returned)) &&
               returned == 1;
    }

    std::pair<double, double> sample() {
        double temperature = missing;
        double watts = missing;
        uint64_t raw{};
        if (tjMax && read(0x1B1, raw) && (raw & 0x80000000))
            temperature = static_cast<double>(tjMax) - static_cast<double>((raw >> 16) & 0x7F);
        if (joulesPerTick && read(0x611, raw)) {
            const auto now = Clock::now();
            const auto energy = static_cast<uint32_t>(raw);
            const double elapsed = seconds(lastEnergySample, now);
            if (lastEnergySample != TimePoint{} && elapsed > 0 && elapsed <= 30)
                watts = static_cast<uint32_t>(energy - lastEnergy) * joulesPerTick / elapsed;
            lastEnergySample = now;
            lastEnergy = energy;
        }
        return {temperature, watts};
    }

    HMODULE module{};
    HANDLE handle{};
    Close close{};
    Execute execute{};
    unsigned int tjMax{};
    double joulesPerTick{};
    TimePoint lastEnergySample{};
    uint32_t lastEnergy{};
};

Telemetry::Telemetry() : impl_(std::make_unique<Impl>()) {}
Telemetry::~Telemetry() = default;
Snapshot Telemetry::sample() { return impl_->sample(); }
HardwareInventory Telemetry::inventory() const { return impl_->inventory(); }
HardwareDetails Telemetry::details(DetailPane pane, bool keepAlive) { return impl_->details(pane, keepAlive); }

CpuSensors::CpuSensors() : impl_(std::make_unique<Impl>()) {}
CpuSensors::~CpuSensors() = default;
std::pair<double, double> CpuSensors::sample() { return impl_->sample(); }

}  // namespace cw
