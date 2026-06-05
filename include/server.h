// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#pragma once

#include "device_manager.h"
#include "types.h"
#include <string>
#include <memory>
#include <thread>

namespace tuya {

class ControlServer {
public:
    ControlServer(const ServerConfig& cfg, std::shared_ptr<DeviceManager> mgr);
    ~ControlServer();

    void run();
    void stop();

private:
    ServerConfig cfg_;
    std::shared_ptr<DeviceManager> mgr_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};

    void setup_routes();
    void server_loop();
};

} // namespace tuya
