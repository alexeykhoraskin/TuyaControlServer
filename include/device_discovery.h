// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#pragma once

#include "types.h"
#include <vector>
#include <string>

namespace tuya {

// UDP broadcast discovery for Tuya devices on LAN
std::vector<DeviceInfo> tuya_discover(int timeout_s = 5);

} // namespace tuya
