// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#pragma once

#include "types.h"
#include <string>

namespace tuya {

ServerConfig load_config(const std::string& path);

} // namespace tuya
