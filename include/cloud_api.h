// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#pragma once

#include "types.h"
#include <string>
#include <optional>

namespace tuya {

class CloudApi {
public:
    explicit CloudApi(CloudConfig config);
    ~CloudApi() = default;

    // Return true if credentials are configured
    bool valid() const { return !config_.access_id.empty(); }

    // Refresh token (called automatically)
    bool refresh_token();

    // Send infrared command via IR Control Hub
    bool send_ir_command(const std::string& hub_device_id,
                         const std::string& remote_id,
                         int category_id,
                         int remote_index,
                         const std::string& key);

    // List available infrared keys for a remote
    std::optional<std::vector<std::pair<std::string, std::string>>>
    list_ir_keys(const std::string& hub_device_id,
                 const std::string& remote_id);

    // Query device status → returns DPS map
    std::optional<DpsMap> query_status(const std::string& device_id);

    // Send commands to device
    bool send_commands(const std::string& device_id, const DpsMap& commands);

    // List devices in the account → returns list of DeviceInfo
    std::optional<std::vector<DeviceInfo>> list_devices();

    // Fetch single device details (local_key, ip, status codes) from cloud
    std::optional<DeviceInfo> fetch_device_details(const std::string& device_id);

    // Fetch device specification (function codes with types & ranges)
    bool fetch_device_specs(const std::string& device_id);

    // Update cached status codes for a device (called periodically)
    void update_cached_codes(const std::string& device_id,
                              const std::vector<std::string>& codes);

    // Get cached status codes for a device
    std::vector<std::string> get_cached_codes(const std::string& device_id);

    // Get cached command info (richer than codes) for a device
    std::vector<CommandInfo> get_cached_commands(const std::string& device_id);

private:
    CloudConfig config_;
    std::string access_token_;
    std::string uid_;
    int         expire_s_ = 0;
    int64_t     acquire_time_ = 0;  // seconds since epoch
    std::map<std::string, std::vector<std::string>> status_code_cache_;
    std::map<std::string, std::vector<CommandInfo>> command_cache_;
    std::mutex cache_mtx_;

    std::string sign_request(const std::string& method,
                              const std::string& path,
                              const std::string& body,
                              int64_t timestamp,
                              const std::string& token = "") const;

    std::string do_request(const std::string& method,
                            const std::string& path,
                            const std::string& body,
                            bool auth = false);

    bool is_token_expired() const;
};

} // namespace tuya
