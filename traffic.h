#pragma once
#include <windows.h>
#include <array>
#include <cstdint>
#include <memory>
namespace cw {
struct TrafficRow { wchar_t path[520]{}; std::uint64_t internet{}, lan{}, other{}; };
struct TrafficPage {
    std::int64_t started{}, day{};
    unsigned status = ERROR_NOT_READY, lost{}, count{};
    std::array<TrafficRow, 512> rows{};
};
class TrafficCapture {
public:
    TrafficCapture(); ~TrafficCapture();
    void start();
    void snapshot(TrafficPage& page);
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
// 0 public, 1 private/link-local, 2 loopback/multicast/reserved.
int trafficScope(const unsigned char* address, bool ipv6);
}
