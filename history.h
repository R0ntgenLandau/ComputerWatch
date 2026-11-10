#pragma once

#include "common.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace cw {

class History {
public:
    History(const std::filesystem::path& path, bool writer);
    ~History();
    History(History&&) noexcept;
    History& operator=(History&&) noexcept;

    bool ready() const;
    bool save(const Snapshot& snapshot);
    int retentionDays() const;
    bool setRetentionDays(int days);
    bool prune(std::int64_t cutoffMs, bool& more);
    std::vector<Snapshot> series(std::int64_t fromMs, std::int64_t toMs, std::size_t maxPoints) const;
    std::optional<Snapshot> at(std::int64_t timestampMs) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cw
