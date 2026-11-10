#include "common.h"
#include "disk_smart.h"
#include "history.h"
#include "telemetry.h"
#include "ui.h"

#include <windows.h>
#include <sddl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>

namespace cw {
namespace {

using namespace std::chrono;
using namespace std::chrono_literals;

constexpr auto DashboardClass = L"ComputerWatchDashboard";
constexpr auto UiMutexName = L"Local\\ComputerWatch.Dashboard.1";
constexpr auto MinuteMs = 60000;
constexpr std::int64_t DayMs = 86400000;

class Handle {
public:
    explicit Handle(HANDLE handle = nullptr) : handle_(handle) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return handle_ && handle_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const { return handle_; }
    void reset(HANDLE handle = nullptr) {
        if (*this) CloseHandle(handle_);
        handle_ = handle;
    }
private:
    HANDLE handle_;
};

std::wstring channelName(DWORD pid, const wchar_t* part) {
    return L"Local\\ComputerWatch.Sensors.2." + std::to_wstring(pid) + L"." + part;
}

std::wstring executablePath() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) return {};
    path.resize(length);
    return path;
}

bool isDashboard(HANDLE process, const std::wstring& executable) {
    std::wstring path(32768, L'\0');
    DWORD length = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &length)) return false;
    return _wcsicmp(path.c_str(), executable.c_str()) == 0;
}

class ReadableMappingSecurity {
public:
    ReadableMappingSecurity() {
        HANDLE rawToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) return;
        Handle token(rawToken);
        alignas(TOKEN_USER) std::array<std::byte, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> bytes{};
        DWORD size = 0;
        if (!GetTokenInformation(token.get(), TokenUser, bytes.data(),
                                 static_cast<DWORD>(bytes.size()), &size)) return;

        const auto* user = reinterpret_cast<const TOKEN_USER*>(bytes.data());
        wchar_t* sid = nullptr;
        if (!ConvertSidToStringSidW(user->User.Sid, &sid)) return;
        const auto policy = std::wstring(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;") + sid + L")";
        LocalFree(sid);
        ConvertStringSecurityDescriptorToSecurityDescriptorW(
            policy.c_str(), SDDL_REVISION_1, &attributes_.lpSecurityDescriptor, nullptr);
    }

    ~ReadableMappingSecurity() { LocalFree(attributes_.lpSecurityDescriptor); }
    SECURITY_ATTRIBUTES* get() { return attributes_.lpSecurityDescriptor ? &attributes_ : nullptr; }

private:
    SECURITY_ATTRIBUTES attributes_{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
};

// A single atomic word keeps both values from the same sensor sample together.
using CpuWord = std::uint64_t;
static_assert(std::atomic_ref<CpuWord>::is_always_lock_free);

struct SharedDisk {
    unsigned number{};
    wchar_t name[120]{};
    wchar_t bus[20]{};
    wchar_t note[160]{};
    int health = -1;
    double temperatureC = Missing;
    double wearPercent = Missing;
    double powerOnHours = Missing;
    double powerCycles = Missing;
    double errors = Missing;
};

struct SensorPage {
    CpuWord cpu;
    CpuWord helperStamp;
    std::size_t diskCount;
    std::array<SharedDisk, 32> disks;
};

template<std::size_t Size>
void copyLabel(wchar_t (&target)[Size], std::wstring_view label) {
    std::copy_n(label.data(), std::min(Size - 1, label.size()), target);
}

void publishDisks(SensorPage& page, const std::vector<DiskDetail>& disks) {
    page.diskCount = std::min(disks.size(), page.disks.size());
    for (std::size_t i = 0; i < page.diskCount; ++i) {
        const auto& disk = disks[i];
        auto& row = page.disks[i];
        row = {};
        row.number = disk.number;
        copyLabel(row.name, disk.name);
        copyLabel(row.bus, disk.bus);
        copyLabel(row.note, disk.note);
        if (disk.healthy) row.health = *disk.healthy ? 1 : 0;
        row.temperatureC = disk.temperatureC;
        row.wearPercent = disk.wearPercent;
        row.powerOnHours = disk.powerOnHours;
        row.powerCycles = disk.powerCycles;
        row.errors = disk.errors;
    }
}

class SensorClient {
public:
    SensorClient()
        : request_(CreateEventW(nullptr, FALSE, FALSE, channelName(pid_, L"request").c_str())),
          response_(CreateEventW(nullptr, FALSE, FALSE, channelName(pid_, L"response").c_str())),
          diskRequest_(CreateEventW(nullptr, FALSE, FALSE, channelName(pid_, L"disk-request").c_str())),
          diskResponse_(CreateEventW(nullptr, FALSE, FALSE, channelName(pid_, L"disk-response").c_str())) {}

    ~SensorClient() { if (view_) UnmapViewOfFile(view_); }

    void update(Snapshot& snapshot) {
        if (!request_ || !response_ || !connect()) return;

        ResetEvent(response_.get());
        SetEvent(request_.get());
        if (WaitForSingleObject(response_.get(), 80) == WAIT_OBJECT_0) {
            const auto values = std::bit_cast<std::array<float, 2>>(
                std::atomic_ref<CpuWord>(view_->cpu).load(std::memory_order_acquire));
            snapshot[Metric::CpuTemp] = values[0];
            snapshot[Metric::CpuWatts] = values[1];
        } else {
            UnmapViewOfFile(view_);
            view_ = nullptr;
        }
    }

    std::optional<std::vector<DiskDetail>> pollDisks() {
        if (!diskRequest_ || !diskResponse_ || !view_) return std::nullopt;
        if (diskPending_) {
            if (WaitForSingleObject(diskResponse_.get(), 0) != WAIT_OBJECT_0) return std::nullopt;
            diskPending_ = false;
            nextDiskRead_ = steady_clock::now() + 60s;
            const auto count = std::min(view_->diskCount, view_->disks.size());
            std::vector<DiskDetail> disks;
            disks.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                const auto& row = view_->disks[i];
                DiskDetail disk;
                disk.number = row.number;
                disk.name = row.name;
                disk.bus = row.bus;
                disk.note = row.note;
                if (row.health >= 0) disk.healthy = row.health != 0;
                disk.temperatureC = row.temperatureC;
                disk.wearPercent = row.wearPercent;
                disk.powerOnHours = row.powerOnHours;
                disk.powerCycles = row.powerCycles;
                disk.errors = row.errors;
                disks.push_back(std::move(disk));
            }
            return disks;
        }
        if (steady_clock::now() >= nextDiskRead_) {
            ResetEvent(diskResponse_.get());
            diskPending_ = SetEvent(diskRequest_.get()) != FALSE;
        }
        return std::nullopt;
    }

    void leaveDiskView() { nextDiskRead_ = {}; }

private:
    bool connect() {
        if (!view_) {
            Handle mapping(OpenFileMappingW(FILE_MAP_READ, FALSE, channelName(pid_, L"data").c_str()));
            if (mapping) {
                view_ = static_cast<SensorPage*>(
                    MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, sizeof(SensorPage)));
            }
        }
        if (!view_) return false;
        const auto stamp = std::atomic_ref<CpuWord>(view_->helperStamp).load(std::memory_order_acquire);
        if (!stamp) {
            UnmapViewOfFile(view_);
            view_ = nullptr;
            return false;
        }
        if (stamp != helperStamp_) {
            helperStamp_ = stamp;
            diskPending_ = false;
            nextDiskRead_ = {};
        }
        return true;
    }

    const DWORD pid_ = GetCurrentProcessId();
    Handle request_;
    Handle response_;
    Handle diskRequest_;
    Handle diskResponse_;
    SensorPage* view_{};
    CpuWord helperStamp_{};
    bool diskPending_{};
    steady_clock::time_point nextDiskRead_{};
};

int runCpuHelper() {
    const auto executable = executablePath();
    ReadableMappingSecurity security;
    if (executable.empty() || !security.get()) return 1;
    const auto deadline = steady_clock::now() + 45s;
    for (; steady_clock::now() < deadline; Sleep(200)) {
        const HWND window = FindWindowW(DashboardClass, nullptr);
        DWORD pid = 0;
        if (!window || !GetWindowThreadProcessId(window, &pid)) continue;

        Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        if (!process || !isDashboard(process.get(), executable)) continue;
        Handle request(OpenEventW(SYNCHRONIZE, FALSE, channelName(pid, L"request").c_str()));
        Handle response(OpenEventW(EVENT_MODIFY_STATE, FALSE, channelName(pid, L"response").c_str()));
        Handle diskRequest(OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, channelName(pid, L"disk-request").c_str()));
        Handle diskResponse(OpenEventW(EVENT_MODIFY_STATE, FALSE, channelName(pid, L"disk-response").c_str()));
        if (!request || !response || !diskRequest || !diskResponse) continue;

        Handle singleHelper(CreateMutexW(nullptr, FALSE, channelName(pid, L"helper").c_str()));
        if (!singleHelper || GetLastError() == ERROR_ALREADY_EXISTS) return 0;
        Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, security.get(), PAGE_READWRITE, 0,
            sizeof(SensorPage), channelName(pid, L"data").c_str()));
        if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS) return 1;
        auto* view = static_cast<SensorPage*>(
            MapViewOfFile(mapping.get(), FILE_MAP_WRITE, 0, 0, sizeof(SensorPage)));
        if (!view) return 1;

        CpuSensors sensors;
        const auto stamp = (static_cast<CpuWord>(GetCurrentProcessId()) << 32) | GetTickCount();
        std::atomic_ref<CpuWord>(view->helperStamp).store(stamp, std::memory_order_release);
        std::atomic<DWORD> diskThreadId{};
        std::jthread diskWorker([&](std::stop_token stop) {
            diskThreadId.store(GetCurrentThreadId(), std::memory_order_release);
            const HANDLE diskWaits[] = {process.get(), diskRequest.get()};
            auto nextRead = steady_clock::time_point{};
            while (!stop.stop_requested() &&
                   WaitForMultipleObjects(2, diskWaits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
                if (stop.stop_requested()) break;
                if (steady_clock::now() >= nextRead) {
                    try {
                        publishDisks(*view, readDiskSmart(stop));
                    } catch (...) {
                        view->diskCount = 0;
                    }
                    nextRead = steady_clock::now() + 60s;
                }
                SetEvent(diskResponse.get());
            }
        });
        const HANDLE waits[] = {process.get(), request.get()};
        while (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
            const auto [temperature, power] = sensors.sample();
            const std::array values{static_cast<float>(temperature), static_cast<float>(power)};
            std::atomic_ref<CpuWord>(view->cpu).store(std::bit_cast<CpuWord>(values), std::memory_order_release);
            SetEvent(response.get());
        }
        diskWorker.request_stop();
        SetEvent(diskRequest.get());
        if (const DWORD id = diskThreadId.load(std::memory_order_acquire)) {
            Handle thread(OpenThread(THREAD_TERMINATE, FALSE, id));
            if (thread) CancelSynchronousIo(thread.get());
        }
        diskWorker.join();
        UnmapViewOfFile(view);
        return 0;
    }
    return 1;
}

void startCpuHelper() {
    Handle active(OpenFileMappingW(FILE_MAP_READ, FALSE, channelName(GetCurrentProcessId(), L"data").c_str()));
    if (active) return;
    const auto executable = executablePath();
    if (executable.empty()) return;
    ShellExecuteW(nullptr, L"runas", executable.c_str(), L"--cpu-sensors", nullptr, SW_HIDE);
}

std::int64_t timeMs() {
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

class Collector {
public:
    Collector(std::filesystem::path path, bool visible)
        : path_(std::move(path)), visible_(visible), worker_([this](std::stop_token stop) { collect(stop); }) {}

    std::vector<Snapshot> live() {
        std::scoped_lock lock(mutex_);
        return {recent_.begin(), recent_.end()};
    }

    void setVisible(bool visible) {
        {
            std::scoped_lock lock(mutex_);
            if (visible_ == visible) return;
            visible_ = visible;
            ++modeRevision_;
        }
        wake_.notify_all();
    }

    HardwareDetails hardwareDetails() {
        std::scoped_lock lock(mutex_);
        return hardwareDetails_;
    }

    void selectDetail(DetailPane pane) {
        {
            std::scoped_lock lock(mutex_);
            if (detailPane_ == pane) return;
            detailPane_ = pane;
            hardwareDetails_ = {};
            ++modeRevision_;
        }
        wake_.notify_all();
    }

    void stop() {
        worker_.request_stop();
        wake_.notify_all();
    }

private:
    void collect(std::stop_token stop) {
        Telemetry telemetry;
        History history(path_, true);
        auto nextSave = steady_clock::now();
        auto nextPrune = steady_clock::now();
        int lastRetention = 0;

        while (!stop.stop_requested()) {
            const auto started = steady_clock::now();
            DetailPane detailPane;
            std::uint64_t revision;
            {
                std::scoped_lock lock(mutex_);
                detailPane = visible_ ? detailPane_ : DetailPane::None;
                revision = modeRevision_;
            }
            auto sample = telemetry.sample();
            sample.timestampMs = timeMs();
            sensors_.update(sample);
            auto details = telemetry.details(detailPane);
            std::optional<std::vector<DiskDetail>> disks;
            if (detailPane == DetailPane::Disk) {
                disks = sensors_.pollDisks();
            } else {
                sensors_.leaveDiskView();
            }
            {
                std::scoped_lock lock(mutex_);
                if (detailPane != DetailPane::None && modeRevision_ == revision) {
                    if (detailPane == DetailPane::Disk) {
                        if (disks) {
                            hardwareDetails_.pane = DetailPane::Disk;
                            hardwareDetails_.disks = std::move(*disks);
                        }
                    } else {
                        hardwareDetails_ = std::move(details);
                    }
                }
                recent_.push_back(sample);
                while (recent_.size() > 61 ||
                       recent_.front().timestampMs < sample.timestampMs - MinuteMs) {
                    recent_.pop_front();
                }
            }

            if (started >= nextSave && history.ready()) {
                if (history.save(sample)) {
                    do { nextSave += 5s; } while (nextSave <= started);
                }
                const int retention = history.retentionDays();
                if (started >= nextPrune || retention != lastRetention) {
                    bool moreExpired = false;
                    if (history.prune(sample.timestampMs - retention * DayMs, moreExpired)) {
                        nextPrune = started + (moreExpired ? 5s : 15min);
                        lastRetention = retention;
                    }
                }
            }

            std::unique_lock lock(mutex_);
            const bool visible = visible_;
            auto deadline = started + (visible ? 1s : 5s);
            if (history.ready() && nextSave > started) deadline = std::min(deadline, nextSave);
            wake_.wait_until(lock, stop, deadline, [&] { return modeRevision_ != revision; });
        }
    }

    std::filesystem::path path_;
    SensorClient sensors_;
    std::mutex mutex_;
    std::condition_variable_any wake_;
    bool visible_;
    DetailPane detailPane_ = DetailPane::None;
    HardwareDetails hardwareDetails_;
    std::uint64_t modeRevision_{};
    std::deque<Snapshot> recent_;
    std::jthread worker_;
};

int run(HINSTANCE instance, std::wstring_view arguments) {
    if (arguments == L"--cpu-sensors") return runCpuHelper();

    const bool startVisible = arguments != L"--tray";
    Handle singleInstance(CreateMutexW(nullptr, FALSE, UiMutexName));
    if (!singleInstance) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (startVisible) {
            for (int retry = 0; retry < 20; ++retry) {
                if (const HWND window = FindWindowW(DashboardClass, nullptr)) {
                    PostMessageW(window, WM_APP + 1, 0, 0);
                    return 0;
                }
                Sleep(50);
            }
        }
        return 0;
    }

    const wchar_t* localData = _wgetenv(L"LOCALAPPDATA");
    if (!localData || !*localData) return 1;
    const auto directory = std::filesystem::path(localData) / L"ComputerWatch";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return 1;
    const auto path = directory / L"history.db";

    Collector collector(path, startVisible);
    std::unique_ptr<History> history;
    const auto openHistory = [&]() -> History& {
        if (!history || !history->ready()) history = std::make_unique<History>(path, false);
        return *history;
    };

    UiCallbacks callbacks;
    callbacks.live = [&] { return collector.live(); };
    callbacks.series = [&](std::int64_t from, std::int64_t to, std::size_t maximum) {
        return openHistory().series(from, to, maximum);
    };
    callbacks.at = [&](std::int64_t when) { return openHistory().at(when); };
    callbacks.retention = [&] { return openHistory().retentionDays(); };
    callbacks.setRetention = [&](int days) { return openHistory().setRetentionDays(days); };
    callbacks.stop = [&] { collector.stop(); };
    callbacks.visibility = [&](bool visible) { collector.setVisible(visible); };
    callbacks.startCpu = startCpuHelper;
    callbacks.details = [&] { return collector.hardwareDetails(); };
    callbacks.selectDetail = [&](DetailPane pane) { collector.selectDetail(pane); };

    if (startVisible) startCpuHelper();
    return runUi(instance, startVisible, std::move(callbacks));
}

} // namespace
} // namespace cw

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    try {
        return cw::run(instance, commandLine);
    } catch (...) {
        return 1;
    }
}
