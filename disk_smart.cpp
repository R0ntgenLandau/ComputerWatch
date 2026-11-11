#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <nvme.h>

#include "disk_smart.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <string_view>

namespace cw {
namespace {

class DriveHandle {
public:
    DriveHandle(const std::wstring& path, DWORD access)
        : handle_(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, 0, nullptr)),
          error_(handle_ == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS) {}

    ~DriveHandle() {
        if (handle_ != INVALID_HANDLE_VALUE)
            CloseHandle(handle_);
    }

    DriveHandle(const DriveHandle&) = delete;
    DriveHandle& operator=(const DriveHandle&) = delete;

    explicit operator bool() const { return handle_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return handle_; }
    DWORD error() const { return error_; }

private:
    HANDLE handle_;
    DWORD error_;
};

std::vector<unsigned> physicalDrives() {
    std::vector<wchar_t> names(4096);
    while (!QueryDosDeviceW(nullptr, names.data(), static_cast<DWORD>(names.size()))) {
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || names.size() >= (1u << 20))
            return {};
        names.resize(names.size() * 2);
    }

    constexpr std::wstring_view prefix = L"PhysicalDrive";
    std::vector<unsigned> numbers;
    for (const wchar_t* entry = names.data(); *entry;) {
        const std::wstring_view name(entry);
        entry += name.size() + 1;
        if (!name.starts_with(prefix) || name.size() == prefix.size())
            continue;

        unsigned number = 0;
        bool valid = true;
        for (const wchar_t character : name.substr(prefix.size())) {
            if (character < L'0' || character > L'9') {
                valid = false;
                break;
            }
            const auto digit = static_cast<unsigned>(character - L'0');
            if (number > (std::numeric_limits<unsigned>::max() - digit) / 10) {
                valid = false;
                break;
            }
            number = number * 10 + digit;
        }
        if (valid)
            numbers.push_back(number);
    }

    std::sort(numbers.begin(), numbers.end());
    numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());
    return numbers;
}

std::wstring readDescriptorText(const std::vector<BYTE>& bytes, DWORD length, DWORD offset) {
    if (offset < offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties) || offset >= length)
        return {};
    const auto* first = reinterpret_cast<const char*>(bytes.data() + offset);
    const auto* last = static_cast<const char*>(std::memchr(first, '\0', length - offset));
    if (!last)
        return {};
    std::wstring text(first, last);
    const auto start = text.find_first_not_of(L' ');
    if (start == std::wstring::npos)
        return {};
    return text.substr(start, text.find_last_not_of(L' ') - start + 1);
}

std::optional<STORAGE_BUS_TYPE> identifyDrive(HANDLE drive, DiskDetail& disk) {
    STORAGE_PROPERTY_QUERY query{StorageDeviceProperty, PropertyStandardQuery, {}};
    STORAGE_DESCRIPTOR_HEADER header{};
    DWORD returned{};
    if (!DeviceIoControl(drive, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                         &header, sizeof(header), &returned, nullptr) || returned < sizeof(header) ||
        header.Size < offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties) || header.Size > 65536)
        return std::nullopt;

    std::vector<BYTE> bytes(header.Size);
    if (!DeviceIoControl(drive, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                         bytes.data(), static_cast<DWORD>(bytes.size()), &returned, nullptr) ||
        returned < offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties))
        return std::nullopt;

    STORAGE_DEVICE_DESCRIPTOR descriptor{};
    std::memcpy(&descriptor, bytes.data(), offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties));
    const auto vendor = readDescriptorText(bytes, returned, descriptor.VendorIdOffset);
    const auto product = readDescriptorText(bytes, returned, descriptor.ProductIdOffset);
    if (!product.empty()) {
        disk.name = product;
        if (!vendor.empty() && vendor != L"ATA" && vendor != L"NVMe" && !product.starts_with(vendor))
            disk.name = vendor + L" " + product;
    }
    return descriptor.BusType;
}

double readLittleEndian(const BYTE* value, std::size_t length) {
    double result = 0;
    while (length)
        result = result * 256 + value[--length];
    return result;
}

bool validAtaPage(const std::array<BYTE, 512>& page) {
    unsigned checksum = 0;
    for (const BYTE value : page) checksum += value;
    return (page[0] || page[1]) && (checksum & 0xff) == 0;
}

constexpr std::size_t AtaAttributeSize = 12;
constexpr std::size_t AtaAttributeEnd = 2 + 30 * AtaAttributeSize;

bool readNvmeHealth(HANDLE drive, STORAGE_PROPERTY_ID property, DiskDetail& disk) {
    constexpr auto queryOffset = offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters);
    constexpr auto descriptorOffset = offsetof(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
    std::array<BYTE, queryOffset + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + sizeof(NVME_HEALTH_INFO_LOG)> buffer{};

    STORAGE_PROPERTY_QUERY query{property, PropertyStandardQuery, {}};
    STORAGE_PROTOCOL_SPECIFIC_DATA protocol{};
    protocol.ProtocolType = ProtocolTypeNvme;
    protocol.DataType = NVMeDataTypeLogPage;
    protocol.ProtocolDataRequestValue = NVME_LOG_PAGE_HEALTH_INFO;
    protocol.ProtocolDataOffset = sizeof(protocol);
    protocol.ProtocolDataLength = sizeof(NVME_HEALTH_INFO_LOG);
    std::memcpy(buffer.data(), &query, queryOffset);
    std::memcpy(buffer.data() + queryOffset, &protocol, sizeof(protocol));

    DWORD returned{};
    if (!DeviceIoControl(drive, IOCTL_STORAGE_QUERY_PROPERTY, buffer.data(), static_cast<DWORD>(buffer.size()),
                         buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr) ||
        returned < sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR))
        return false;

    STORAGE_PROTOCOL_DATA_DESCRIPTOR descriptor{};
    std::memcpy(&descriptor, buffer.data(), sizeof(descriptor));
    const auto& data = descriptor.ProtocolSpecificData;
    if (descriptor.Version != sizeof(descriptor) || descriptor.Size != sizeof(descriptor) ||
        data.ProtocolType != ProtocolTypeNvme || data.DataType != NVMeDataTypeLogPage ||
        data.ProtocolDataOffset < sizeof(data) || data.ProtocolDataLength < sizeof(NVME_HEALTH_INFO_LOG) ||
        data.ProtocolDataOffset > returned - descriptorOffset ||
        sizeof(NVME_HEALTH_INFO_LOG) > returned - descriptorOffset - data.ProtocolDataOffset)
        return false;

    NVME_HEALTH_INFO_LOG health{};
    std::memcpy(&health, buffer.data() + descriptorOffset + data.ProtocolDataOffset, sizeof(health));
    static_assert(sizeof(health) == 512);
    disk.smartLog.format = SmartLogFormat::Nvme;
    std::memcpy(disk.smartLog.bytes.data(), &health, sizeof(health));
    disk.healthy = health.CriticalWarning.AsUchar == 0;
    const unsigned kelvin = health.Temperature[0] | (static_cast<unsigned>(health.Temperature[1]) << 8);
    if (kelvin != 0 && kelvin != 0xffff)
        disk.temperatureC = static_cast<double>(kelvin) - 273.0;
    disk.wearPercent = health.PercentageUsed;
    disk.powerOnHours = readLittleEndian(health.PowerOnHours, sizeof(health.PowerOnHours));
    disk.powerCycles = readLittleEndian(health.PowerCycle, sizeof(health.PowerCycle));
    disk.errors = readLittleEndian(health.MediaErrors, sizeof(health.MediaErrors));
    return true;
}

template <std::size_t Size>
bool readAtaSmart(HANDLE drive, DWORD control, BYTE command, std::array<BYTE, Size>& data) {
    constexpr auto payloadOffset = offsetof(SENDCMDOUTPARAMS, bBuffer);
    SENDCMDINPARAMS request{};
    request.cBufferSize = static_cast<DWORD>(Size);
    request.irDriveRegs.bFeaturesReg = command;
    request.irDriveRegs.bSectorCountReg = 1;
    request.irDriveRegs.bSectorNumberReg = 1;
    request.irDriveRegs.bCylLowReg = SMART_CYL_LOW;
    request.irDriveRegs.bCylHighReg = SMART_CYL_HI;
    request.irDriveRegs.bDriveHeadReg = 0xa0;
    request.irDriveRegs.bCommandReg = SMART_CMD;

    std::array<BYTE, payloadOffset + Size> buffer{};
    DWORD returned{};
    if (!DeviceIoControl(drive, control, &request, offsetof(SENDCMDINPARAMS, bBuffer),
                         buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr) ||
        returned < buffer.size())
        return false;

    DRIVERSTATUS status{};
    std::memcpy(&status, buffer.data() + offsetof(SENDCMDOUTPARAMS, DriverStatus), sizeof(status));
    if (status.bDriverError != SMART_NO_ERROR)
        return false;

    std::copy_n(buffer.data() + payloadOffset, Size, data.data());
    return true;
}

void readAtaHealth(HANDLE drive, DiskDetail& disk, std::stop_token stop) {
    std::array<BYTE, sizeof(IDEREGS)> status{};
    if (readAtaSmart(drive, SMART_SEND_DRIVE_COMMAND, RETURN_SMART_STATUS, status)) {
        IDEREGS registers{};
        std::memcpy(&registers, status.data(), sizeof(registers));
        if (registers.bCylLowReg == SMART_CYL_LOW && registers.bCylHighReg == SMART_CYL_HI)
            disk.healthy = true;
        else if (registers.bCylLowReg == 0xf4 && registers.bCylHighReg == 0x2c)
            disk.healthy = false;
    }

    if (stop.stop_requested()) return;
    std::array<BYTE, READ_ATTRIBUTE_BUFFER_SIZE> attributes{};
    if (!readAtaSmart(drive, SMART_RCV_DRIVE_DATA, READ_ATTRIBUTES, attributes)) {
        disk.note = L"SATA SMART 属性读取失败";
        return;
    }

    if (!validAtaPage(attributes)) {
        disk.note = L"SATA SMART 数据校验失败";
        return;
    }

    std::array<const BYTE*, 256> fields{};
    for (std::size_t offset = 2; offset < AtaAttributeEnd; offset += AtaAttributeSize)
        fields[attributes[offset]] = attributes.data() + offset;
    disk.smartLog.format = SmartLogFormat::Ata;
    disk.smartLog.bytes = attributes;
    if (!stop.stop_requested()) {
        auto& thresholds = disk.smartLog.thresholds;
        disk.smartLog.hasThresholds =
            readAtaSmart(drive, SMART_RCV_DRIVE_DATA, READ_THRESHOLDS, thresholds) && validAtaPage(thresholds);
    }

    // SMART attributes are vendor-specific. SC001's Maxio profile uses 190 for
    // temperature and 231 for remaining life. Only interpret wear on that profile.
    const bool maxioSc001 = disk.name.find(L"ZHITAI SC001") != std::wstring::npos;
    const BYTE* temperature = fields[maxioSc001 ? 190 : 194];
    if (!temperature)
        temperature = fields[maxioSc001 ? 194 : 190];
    if (temperature && temperature[5] <= 125)
        disk.temperatureC = temperature[5];
    if (maxioSc001 && fields[231] && fields[231][3] <= 100)
        disk.wearPercent = 100 - fields[231][3];
    if (fields[9])
        disk.powerOnHours = readLittleEndian(fields[9] + 5, maxioSc001 ? 3 : 6);
    if (fields[12])
        disk.powerCycles = readLittleEndian(fields[12] + 5, 6);
    if (fields[187])
        disk.errors = readLittleEndian(fields[187] + 5, 6);

    if (!disk.healthy)
        disk.note = L"SATA SMART 健康状态不可用";
}

std::wstring errorNote(std::wstring_view label, DWORD error) {
    return std::wstring(label) + L"（Windows 错误 " + std::to_wstring(error) + L"）";
}

DiskDetail readDrive(unsigned number, std::stop_token stop) {
    DiskDetail disk{};
    disk.number = number;
    disk.name = L"PhysicalDrive" + std::to_wstring(number);
    const std::wstring path = LR"(\\.\)" + disk.name;
    const DriveHandle metadata(path, 0);
    if (!metadata) {
        disk.note = errorNote(L"无法打开物理磁盘", metadata.error());
        return disk;
    }

    const auto bus = identifyDrive(metadata.get(), disk);
    if (stop.stop_requested()) return disk;
    if (!bus) {
        disk.note = L"无法读取磁盘型号和接口";
        return disk;
    }

    if (*bus == BusTypeNvme) {
        disk.bus = L"NVMe";
        if (!readNvmeHealth(metadata.get(), StorageDeviceProtocolSpecificProperty, disk) &&
            !stop.stop_requested() &&
            !readNvmeHealth(metadata.get(), StorageAdapterProtocolSpecificProperty, disk))
            disk.note = L"NVMe SMART 日志读取失败";
    } else if (*bus == BusTypeAta || *bus == BusTypeSata) {
        disk.bus = *bus == BusTypeSata ? L"SATA" : L"ATA";
        // Windows requires both access flags for its legacy SMART IOCTLs.
        // The only ATA commands sent here read status, attributes, or thresholds.
        const DriveHandle smart(path, GENERIC_READ | GENERIC_WRITE);
        if (smart && !stop.stop_requested())
            readAtaHealth(smart.get(), disk, stop);
        else
            disk.note = errorNote(L"无法打开 SATA SMART 接口", smart.error());
    } else {
        disk.bus = *bus == BusTypeUsb ? L"USB" : L"其他";
        disk.note = L"此磁盘接口未提供支持的 SMART 读取方式";
    }
    return disk;
}

} // namespace

std::vector<DiskSmartRow> diskSmartRows(const DiskDetail& disk) {
    std::vector<DiskSmartRow> rows;
    const auto& log = disk.smartLog;
    const auto hex = [](const BYTE* bytes, size_t length) {
        constexpr wchar_t digits[] = L"0123456789ABCDEF";
        std::wstring text = L"0x";
        while (length) {
            const auto byte = bytes[--length];
            text += digits[byte >> 4];
            text += digits[byte & 15];
        }
        return text;
    };
    if (log.format == SmartLogFormat::Ata) {
        std::array<const BYTE*, 256> thresholds{};
        if (log.hasThresholds) {
            for (size_t offset = 2; offset < AtaAttributeEnd; offset += AtaAttributeSize)
                thresholds[log.thresholds[offset]] = log.thresholds.data() + offset;
        }
        const bool sc001 = disk.name.find(L"ZHITAI SC001") != std::wstring::npos;
        constexpr std::pair<BYTE, const wchar_t*> sc001Names[] = {
            {5, L"新增坏块"}, {9, L"通电时长"}, {12, L"通电次数"}, {168, L"SATA PHY 错误"},
            {171, L"编程失败"}, {172, L"擦除失败"}, {180, L"剩余备用块"}, {187, L"已报告的不可纠正错误"},
            {190, L"温度"}, {192, L"意外掉电"}, {196, L"重映射事件"}, {208, L"平均擦除次数"},
            {231, L"剩余寿命"}, {233, L"NAND 写入计数"}, {234, L"NAND 读取计数"},
            {241, L"主机写入计数"}, {242, L"主机读取计数"}, {243, L"NAND 温度"}
        };
        for (size_t offset = 2; offset < AtaAttributeEnd; offset += AtaAttributeSize) {
            const BYTE* field = log.bytes.data() + offset;
            const BYTE id = field[0];
            if (!id) continue;
            auto name = hex(field, 1);
            if (sc001) {
                for (const auto& [number, label] : sc001Names) {
                    if (number == id) { name += L"  "; name += label; break; }
                }
            }
            const auto* threshold = thresholds[id];
            rows.push_back({std::move(name),
                std::to_wstring(field[3]) + L" / " + std::to_wstring(field[4]) + L" / " +
                    (threshold ? std::to_wstring(threshold[1]) : L"—"), hex(field + 5, 6)});
        }
    } else if (log.format == SmartLogFormat::Nvme) {
        NVME_HEALTH_INFO_LOG health{};
        std::memcpy(&health, log.bytes.data(), sizeof(health));
        const auto add = [&](const wchar_t* name, std::wstring value, std::wstring note = {}) {
            rows.push_back({name, std::move(value), std::move(note)});
        };
        const auto counter = [&](const wchar_t* name, const BYTE (&bytes)[16], const wchar_t* unit) {
            // Keep all digits of the device's 128-bit counter on every compiler target.
            std::wstring decimal(1, L'0');
            for (size_t i = sizeof(bytes); i > 0; --i) {
                unsigned carry = bytes[i - 1];
                for (auto digit = decimal.rbegin(); digit != decimal.rend(); ++digit) {
                    carry += static_cast<unsigned>(*digit - L'0') * 256;
                    *digit = static_cast<wchar_t>(L'0' + carry % 10);
                    carry /= 10;
                }
                if (carry) decimal.insert(0, std::to_wstring(carry));
            }
            add(name, decimal + unit);
        };
        const auto kelvinText = [](unsigned value) -> std::wstring {
            return value && value != 0xffff ? std::to_wstring(static_cast<int>(value) - 273) + L" °C" : L"—";
        };
        const auto dataUnits = [&](const wchar_t* name, const BYTE (&bytes)[16]) {
            const double units = readLittleEndian(bytes, sizeof(bytes));
            if (!units) { add(name, L"—", L"控制器未报告"); return; }
            wchar_t size[64]{};
            const double gb = units * 0.000512;
            std::swprintf(size, std::size(size), L"%.3g %ls", gb >= 1000 ? gb / 1000 : gb,
                gb >= 1000 ? L"TB" : L"GB");
            add(name, size, hex(bytes, sizeof(bytes)));
        };
        const auto warning = health.CriticalWarning.AsUchar;
        std::wstring flags;
        constexpr const wchar_t* warnings[]{L"备用容量不足", L"温度越限", L"可靠性下降", L"只读",
            L"易失性备份故障", L"持久内存区域异常"};
        for (size_t bit = 0; bit < std::size(warnings); ++bit) {
            if (!(warning & (1u << bit))) continue;
            if (!flags.empty()) flags += L"、";
            flags += warnings[bit];
        }
        if (warning & 0xc0) {
            if (!flags.empty()) flags += L"、";
            flags += L"未知警告位";
        }
        add(L"关键警告", warning ? flags : L"无", hex(&warning, 1));
        add(L"综合温度", kelvinText(health.Temperature[0] | (static_cast<unsigned>(health.Temperature[1]) << 8)));
        add(L"剩余备用容量", std::to_wstring(health.AvailableSpare) + L"%");
        add(L"备用容量阈值", std::to_wstring(health.AvailableSpareThreshold) + L"%");
        add(L"寿命消耗", std::to_wstring(health.PercentageUsed) + L"%");
        dataUnits(L"主机累计读取", health.DataUnitRead);
        dataUnits(L"主机累计写入", health.DataUnitWritten);
        counter(L"主机读取命令", health.HostReadCommands, L" 次");
        counter(L"主机写入命令", health.HostWrittenCommands, L" 次");
        counter(L"控制器忙碌时长", health.ControllerBusyTime, L" 分钟");
        counter(L"通电次数", health.PowerCycle, L" 次");
        counter(L"通电时长", health.PowerOnHours, L" 小时");
        counter(L"非正常关机", health.UnsafeShutdowns, L" 次");
        counter(L"介质及数据完整性错误", health.MediaErrors, L" 次");
        counter(L"错误日志累计条目", health.ErrorInfoLogEntryCount, L" 条");
        add(L"警告温度时长", std::to_wstring(health.WarningCompositeTemperatureTime) + L" 分钟");
        add(L"临界温度时长", std::to_wstring(health.CriticalCompositeTemperatureTime) + L" 分钟");
        const unsigned temperatures[]{health.TemperatureSensor1, health.TemperatureSensor2,
            health.TemperatureSensor3, health.TemperatureSensor4, health.TemperatureSensor5,
            health.TemperatureSensor6, health.TemperatureSensor7, health.TemperatureSensor8};
        for (size_t i = 0; i < std::size(temperatures); ++i) {
            rows.push_back({L"温度传感器 " + std::to_wstring(i + 1), kelvinText(temperatures[i]), {}});
        }
        // NVMe 1.4+ defines these fields inside the older Windows SDK's Reserved1.
        constexpr const wchar_t* thermal[]{L"热管理一级触发", L"热管理二级触发", L"热管理一级时长", L"热管理二级时长"};
        for (size_t i = 0; i < std::size(thermal); ++i) {
            const auto value = static_cast<unsigned long>(readLittleEndian(log.bytes.data() + 216 + i * 4, 4));
            add(thermal[i], value ? std::to_wstring(value) + (i < 2 ? L" 次" : L" 秒") : L"—",
                value ? L"" : L"未触发或未实现");
        }
    }
    return rows;
}

std::vector<std::wstring> readDiskModels() {
    std::vector<std::wstring> models;
    for (const unsigned number : physicalDrives()) {
        const auto index = std::to_wstring(number);
        const DriveHandle drive(LR"(\\.\PhysicalDrive)" + index, 0);
        DiskDetail disk;
        if (drive) identifyDrive(drive.get(), disk);
        models.push_back(L"磁盘 " + index + L" · " + (disk.name.empty() ? L"型号未提供" : disk.name));
    }
    return models;
}

std::vector<DiskDetail> readDiskSmart(std::stop_token stop) {
    const auto numbers = physicalDrives();
    std::vector<DiskDetail> result;
    result.reserve(numbers.size());
    for (const unsigned number : numbers) {
        if (stop.stop_requested()) break;
        result.push_back(readDrive(number, stop));
    }
    return result;
}

} // namespace cw
