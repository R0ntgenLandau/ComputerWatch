#pragma once

#include "common.h"
#include <stop_token>

namespace cw {

std::vector<std::wstring> readDiskModels();
std::vector<DiskDetail> readDiskSmart(std::stop_token stop = {});

} // namespace cw
