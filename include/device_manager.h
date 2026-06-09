// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#pragma once

#include "types.h"
#include "local_device.h"
#include "cloud_api.h"
#include <memory>
#include <map>
#include <string>
#include <functional>
#include <thread>

namespace tuya {

class DeviceManager {
public:
    DeviceManager();
    ~DeviceManager();

    // Load devices from config
    void load_devices(const std::vector<DeviceInfo>& devices);

    // Set cloud API
    void set_cloud(std::shared_ptr<CloudApi> cloud);

    // Connect all devices (local-first, cloud fallback)
    void connect_all();

    // Get device state
    std::shared_ptr<DeviceState> get_device(const std::string& id);

    // List all devices
    std::vector<std::shared_ptr<DeviceState>> list_devices();

    // Query device status
    std::optional<DpsMap> query_status(const std::string& id);

    // Send command
    bool send_command(const std::string& id, const DpsMap& dps);

    // List IR keys for an infrared device
    std::optional<std::vector<IrKeyInfo>>
    list_ir_keys(const std::string& id);

    // List all available commands for a device (IR keys or DPS codes)
    std::vector<CommandInfo> list_commands(const std::string& id);

    // Start background polling
    void start_polling(int interval_s);

    // Fetch device details from cloud and update cached info + local_key
    void refresh_from_cloud();

    // Start periodic cloud refresh (~1 hour interval)
    void start_cloud_refresh(int interval_s = 3600);

    // Stop all
    void stop_all();

    // Discovery callback
    using DiscoverCallback = std::function<void(const DeviceInfo&)>;
    void start_discovery(int timeout_s, DiscoverCallback cb);

private:
    struct ManagedDevice {
        std::shared_ptr<DeviceState>  state;
        std::shared_ptr<LocalDevice>  local;
        std::shared_ptr<CloudApi>     cloud_override; // per-device cloud (if any)
        std::thread                   poll_thread;
    };

    std::map<std::string, std::shared_ptr<ManagedDevice>> devices_;
    std::shared_ptr<CloudApi> cloud_api_;
    std::mutex mtx_;
    std::atomic<bool> running_{false};

    void do_poll(std::shared_ptr<ManagedDevice> dev, int interval_s);
};

} // namespace tuya
