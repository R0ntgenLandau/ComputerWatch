#include "memory_timings.h"

#include <windows.h>
#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <map>
#include <optional>
#include <string>

namespace cw {
namespace {

template<size_t Size>
void copyText(wchar_t (&target)[Size], std::wstring_view text) {
    std::fill(std::begin(target), std::end(target), L'\0');
    std::copy_n(text.data(), std::min(text.size(), Size - 1), target);
}

std::int64_t timestamp() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::wstring hex(std::uint64_t value) {
    wchar_t text[24]{};
    std::swprintf(text, std::size(text), L"0x%llX", static_cast<unsigned long long>(value));
    return text;
}

struct CpuIdentity {
    std::string vendor;
    unsigned family{}, model{}, stepping{};
};

CpuIdentity identifyCpu() {
    CpuIdentity identity;
#if defined(__i386__) || defined(__x86_64__)
    unsigned a{}, b{}, c{}, d{};
    __cpuid(0, a, b, c, d);
    char vendor[13]{};
    std::memcpy(vendor, &b, 4);
    std::memcpy(vendor + 4, &d, 4);
    std::memcpy(vendor + 8, &c, 4);
    identity.vendor = vendor;
    __cpuid(1, a, b, c, d);
    const auto baseFamily = (a >> 8) & 15;
    identity.family = baseFamily + (baseFamily == 15 ? (a >> 20) & 255 : 0);
    identity.model = ((a >> 4) & 15) | ((baseFamily == 6 || baseFamily == 15) ? (a >> 12) & 240 : 0);
    identity.stepping = a & 15;
#endif
    return identity;
}

class PawnModule {
public:
    explicit PawnModule(unsigned resource) {
        wchar_t path[32768]{};
        DWORD bytes = sizeof(path);
        if (RegGetValueW(HKEY_LOCAL_MACHINE,
            LR"(SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO)", L"InstallLocation",
            RRF_RT_REG_SZ, nullptr, path, &bytes) != ERROR_SUCCESS) {
            error = L"PawnIO 未安装，安装器需要提供官方签名版本。";
            return;
        }
        const auto library = std::wstring(path) + L"\\PawnIOLib.dll";
        library_ = LoadLibraryExW(library.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!library_) { error = L"无法加载 PawnIOLib，Win32 " + std::to_wstring(GetLastError()); return; }
        const auto open = std::bit_cast<Open>(GetProcAddress(library_, "pawnio_open"));
        const auto load = std::bit_cast<Load>(GetProcAddress(library_, "pawnio_load"));
        execute_ = std::bit_cast<Execute>(GetProcAddress(library_, "pawnio_execute"));
        close_ = std::bit_cast<Close>(GetProcAddress(library_, "pawnio_close"));
        if (!open || !load || !execute_ || !close_) { error = L"PawnIOLib API 不完整。"; return; }
        const auto opening = open(&handle_);
        if (FAILED(opening)) {
            failure = ReadingStatus::ReadFailed;
            error = L"pawnio_open " + hex(static_cast<unsigned long>(opening));
            return;
        }
        const auto module = GetModuleHandleW(nullptr);
        const auto found = FindResourceW(module, MAKEINTRESOURCEW(resource), RT_RCDATA);
        const auto loaded = found ? LoadResource(module, found) : nullptr;
        const auto* blob = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
        const auto size = found ? SizeofResource(module, found) : 0;
        if (!blob || !size) { error = L"可执行文件缺少签名 PawnIO 模块资源。"; return; }
        const auto status = load(handle_, blob, size);
        if (FAILED(status)) { error = L"pawnio_load " + hex(static_cast<unsigned long>(status)); return; }
        ready_ = true;
    }

    ~PawnModule() {
        if (handle_ && close_) close_(handle_);
        if (library_) FreeLibrary(library_);
    }
    PawnModule(const PawnModule&) = delete;
    PawnModule& operator=(const PawnModule&) = delete;
    explicit operator bool() const { return ready_; }

    std::optional<std::uint32_t> smn(std::uint32_t address) {
        const std::uint64_t input = address;
        std::uint64_t output{};
        size_t count{};
        const auto status = execute_(handle_, "ioctl_read_smn", &input, 1, &output, 1, &count);
        if (FAILED(status) || count != 1) {
            error = L"ioctl_read_smn " + hex(address) + L" HRESULT " +
                hex(static_cast<unsigned long>(status)) + L" count " + std::to_wstring(count);
            return std::nullopt;
        }
        if (output == 0xffffffff) {
            error = L"SMN " + hex(address) + L" 返回全 1，无法确认寄存器有效。";
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(output);
    }

    std::wstring error;
    ReadingStatus failure = ReadingStatus::BackendMissing;
private:
    using Open = HRESULT (WINAPI*)(HANDLE*);
    using Load = HRESULT (WINAPI*)(HANDLE, const BYTE*, size_t);
    using Execute = HRESULT (WINAPI*)(HANDLE, const char*, const std::uint64_t*, size_t,
        std::uint64_t*, size_t, size_t*);
    using Close = HRESULT (WINAPI*)(HANDLE);
    HMODULE library_{};
    HANDLE handle_{};
    Execute execute_{};
    Close close_{};
    bool ready_{};
};

struct FieldDefinition {
    const wchar_t* name;
    std::uint32_t address;
    unsigned low, width;
    const wchar_t* unit;
};

constexpr FieldDefinition AmdDdr4Fields[]{
#include "memory_ddr4_fields.inc"
};

void append(SharedMemoryTimings& report, std::wstring_view name, double value, std::wstring_view unit,
    std::wstring_view scope, std::wstring_view source, ReadingStatus status, std::wstring_view reason) {
    if (report.count >= report.fields.size()) return;
    auto& field = report.fields[report.count++];
    copyText(field.name, name);
    copyText(field.unit, unit);
    copyText(field.scope, scope);
    copyText(field.source, source);
    copyText(field.reason, reason);
    field.value = value;
    field.status = status;
}

void readAmdDdr4(SharedMemoryTimings& report, PawnModule& pawn) {
    HANDLE lock = CreateMutexW(nullptr, FALSE, L"Global\\Access_PCI");
    const auto waited = lock ? WaitForSingleObject(lock, 500) : WAIT_FAILED;
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
        if (lock) CloseHandle(lock);
        report.status = ReadingStatus::ReadFailed;
        copyText(report.reason, L"无法在 500 ms 内获得 Access_PCI 互斥锁。");
        return;
    }
    bool decodedTiming = false;
    // Matisse and Vermeer expose two DDR4 UMC channels. This does not enumerate DIMMs.
    for (unsigned channel = 0; channel < 2; ++channel) {
        const auto offset = channel << 20;
        const auto scope = L"socket 0 / UMC " + std::to_wstring(channel);
        const auto channelConfig = pawn.smn(offset | 0x50df0);
        const auto dimm0 = pawn.smn(offset | 0x50000);
        const auto dimm1 = pawn.smn(offset | 0x50008);
        if (!channelConfig || !dimm0 || !dimm1) {
            append(report, L"ChannelEnabled", Missing, L"bool", scope, L"AMD SMN UMC topology",
                ReadingStatus::ReadFailed, pawn.error);
            continue;
        }
        const bool enabled = !(*channelConfig & (1u << 19)) && ((*dimm0 | *dimm1) & 1);
        append(report, L"ChannelEnabled", enabled, L"bool", scope, L"AMD SMN 0x50DF0 / 0x50000 / 0x50008",
            ReadingStatus::Unverified, L"依据 ZenStates-Core，尚未在 AMD 实机交叉验证。");
        if (!enabled) continue;
        struct RegisterReading {
            std::optional<std::uint32_t> value;
            std::wstring failure;
        };
        std::map<std::uint32_t, RegisterReading> registers;
        for (const auto& definition : AmdDdr4Fields) {
            const auto address = offset | definition.address;
            auto found = registers.find(address);
            if (found == registers.end()) {
                const auto raw = pawn.smn(address);
                found = registers.emplace(address, RegisterReading{raw, raw ? L"" : pawn.error}).first;
            }
            const auto source = L"SMN " + hex(address) + L" [" +
                std::to_wstring(definition.low + definition.width - 1) + L":" +
                std::to_wstring(definition.low) + L"]";
            const auto raw = found->second.value;
            decodedTiming = decodedTiming || raw.has_value();
            const auto mask = (std::uint64_t{1} << definition.width) - 1;
            const auto value = raw ? static_cast<double>((*raw >> definition.low) & mask) : Missing;
            append(report, definition.name, value, definition.unit, scope, source,
                raw ? ReadingStatus::Unverified : ReadingStatus::ReadFailed,
                raw ? L"依据 ZenStates-Core 位域解码，尚未在目标 AMD 平台交叉验证。" : found->second.failure);
        }
        // Retain both programmed refresh register banks instead of selecting a guessed DIMM value.
        for (unsigned bank = 0; bank < 2; ++bank) {
            const auto address = offset | (0x50260 + bank * 4);
            const auto raw = pawn.smn(address);
            const auto bankScope = scope + L" / refresh bank " + std::to_wstring(bank);
            const auto source = L"SMN " + hex(address);
            for (const auto& definition : {FieldDefinition{L"tRFC", 0, 0, 11, L"tCK"},
                {L"tRFC2", 0, 11, 11, L"tCK"}, {L"tRFC4", 0, 22, 10, L"tCK"}}) {
                append(report, definition.name,
                    raw ? static_cast<double>((*raw >> definition.low) & ((1u << definition.width) - 1)) : Missing,
                    L"tCK", bankScope, source, raw ? ReadingStatus::Unverified : ReadingStatus::ReadFailed,
                    raw ? L"寄存器组的 Rank 归属及当前 FGR 活动模式尚待验证。" : pawn.error);
            }
        }
        const auto ratio = pawn.smn(offset | 0x50200);
        const auto ratioFailure = pawn.error;
        const auto refreshMode = pawn.smn(offset | 0x5012c);
        const auto refreshFailure = pawn.error;
        append(report, L"FGR", refreshMode ? static_cast<double>((*refreshMode >> 16) & 7) : Missing,
            L"encoded", scope, L"SMN 0x5012C[18:16]",
            refreshMode ? ReadingStatus::Unverified : ReadingStatus::ReadFailed,
            refreshMode ? L"DDR4 MR3 对应的细粒度刷新模式编码，活动状态尚待实机验证。" : refreshFailure);
        append(report, L"CommandRate", ratio ? 1.0 + ((*ratio >> 10) & 1) : Missing, L"tCK", scope,
            L"SMN 0x50200[10] + 1", ratio ? ReadingStatus::Unverified : ReadingStatus::ReadFailed,
            ratio ? L"由 Cmd2T 编码换算，尚未在 AMD 实机交叉验证。" : ratioFailure);
        append(report, L"MemClkRatio", ratio ? (*ratio & 0x7f) / 3.0 : Missing, L"ratio", scope,
            L"SMN 0x50200[6:0] / 3", ratio ? ReadingStatus::Unverified : ReadingStatus::ReadFailed,
            ratio ? L"计算值；尚未独立测量参考时钟。" : ratioFailure);
        for (const auto* name : {L"MemoryClock", L"EffectiveDataRate", L"tCCD", L"RTL", L"IOL"}) {
            append(report, name, Missing, L"", scope, L"coverage review", ReadingStatus::Unverified,
                L"尚无该平台独立验证的读取或别名映射依据。");
        }
    }
    ReleaseMutex(lock);
    CloseHandle(lock);
    report.status = decodedTiming ? ReadingStatus::Unverified : ReadingStatus::ReadFailed;
    copyText(report.reason, decodedTiming ?
        L"AMD DDR4 解码已实现；当前环境没有适用 AMD 硬件，尚未实机验证完整性和准确性。" :
        L"未取得可解码的时序寄存器；检查通道状态与各字段读取错误。");
}

SharedMemoryTimings collect() {
    SharedMemoryTimings report;
    report.sampledAtMs = timestamp();
    const auto cpu = identifyCpu();
    const auto platform = std::wstring(cpu.vendor.begin(), cpu.vendor.end()) + L" Family " +
        std::to_wstring(cpu.family) + L" Model " + std::to_wstring(cpu.model) + L" Stepping " +
        std::to_wstring(cpu.stepping);
    copyText(report.platform, platform);
#if !defined(__x86_64__)
    copyText(report.platform, L"此架构的内存控制器平台识别尚未适配。");
    copyText(report.reason, L"内存时序原生采集当前仅提供 Windows x64 实现；此架构尚未适配。");
    return report;
#endif
    // Explicitly scoped to AMD desktop Matisse and Vermeer. No cross-generation fallback.
    const bool amdDdr4 = cpu.vendor == "AuthenticAMD" &&
        ((cpu.family == 0x17 && cpu.model == 0x71) || (cpu.family == 0x19 && cpu.model == 0x21));
    if (amdDdr4) {
        copyText(report.backend, L"PawnIO AMDFamily17 / ZenStates DDR4");
        PawnModule pawn(103);
        if (!pawn) { report.status = pawn.failure; copyText(report.reason, pawn.error); return report; }
        readAmdDdr4(report, pawn);
    } else if (cpu.vendor == "GenuineIntel" && cpu.family == 6 && cpu.model == 0x4f) {
        copyText(report.backend, L"PawnIO IntelMCHBAR / Broadwell-EP probe");
        PawnModule pawn(104);
        report.status = pawn ? ReadingStatus::BackendMissing : pawn.failure;
        copyText(report.reason, pawn ? L"模块已加载；缺少已验证的 Broadwell-EP 时序寄存器解码。" :
            pawn.error + L"；Broadwell-EP 当前缺少适配。");
    } else {
        copyText(report.backend, L"platform dispatch");
        copyText(report.reason, L"当前 CPU 平台尚未适配，缺少相应签名读取后端和已验证解码。");
    }
    return report;
}

} // namespace

int runMemoryTimingWorker(std::wstring_view mappingName) {
    if (!mappingName.starts_with(L"Local\\ComputerWatch.MemoryRead.")) return 2;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_WRITE, FALSE, std::wstring(mappingName).c_str());
    if (!mapping) return 3;
    auto* data = static_cast<SharedMemoryTimings*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(SharedMemoryTimings)));
    if (!data) { CloseHandle(mapping); return 4; }
    *data = collect();
    UnmapViewOfFile(data);
    CloseHandle(mapping);
    return 0;
}

SharedMemoryTimings readMemoryTimings() {
    SharedMemoryTimings result;
    result.sampledAtMs = timestamp();
    result.status = ReadingStatus::ReadFailed;
    copyText(result.reason, L"无法创建内存时序采集子进程。");
    const auto name = L"Local\\ComputerWatch.MemoryRead." + std::to_wstring(GetCurrentProcessId()) + L"." +
        std::to_wstring(GetTickCount64());
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(result), name.c_str());
    if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mapping) CloseHandle(mapping);
        return result;
    }
    auto* data = static_cast<SharedMemoryTimings*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(result)));
    if (!data) { CloseHandle(mapping); return result; }
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, std::size(executable));
    if (length && length < std::size(executable)) {
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --memory-timings-worker " + name;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &process)) {
            const auto waited = WaitForSingleObject(process.hProcess, 5000);
            DWORD code{};
            if (waited == WAIT_OBJECT_0 && GetExitCodeProcess(process.hProcess, &code) && code == 0 && data->protocol == 1) {
                result = *data;
            } else {
                TerminateProcess(process.hProcess, 5);
                WaitForSingleObject(process.hProcess, 1000);
                copyText(result.reason, waited == WAIT_TIMEOUT ? L"底层读取超过 5 秒，采集子进程已终止。" :
                    L"采集子进程失败，退出码 " + std::to_wstring(code));
            }
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    }
    UnmapViewOfFile(data);
    CloseHandle(mapping);
    return result;
}

std::shared_ptr<const MemoryTimingReport> memoryTimingReport(const SharedMemoryTimings& data) {
    auto report = std::make_shared<MemoryTimingReport>();
    report->platform = data.platform;
    report->backend = data.backend;
    report->reason = data.reason;
    report->status = data.status;
    report->sampledAtMs = data.sampledAtMs;
    for (size_t i = 0; i < std::min<size_t>(data.count, data.fields.size()); ++i) {
        const auto& row = data.fields[i];
        report->fields.push_back({row.name, row.unit, row.scope, row.source, row.reason,
            std::isfinite(row.value) ? std::optional<double>(row.value) : std::nullopt, row.status, data.sampledAtMs});
    }
    return report;
}

} // namespace cw
