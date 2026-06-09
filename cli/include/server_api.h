// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#pragma once

#include <string>
#include <vector>
#include <map>

struct DeviceInfo {
    std::string id;
    std::string ip;
    std::string name;
    std::string mode;
    bool        online = false;
    bool        ir = false;
};

struct DeviceStatus {
    std::string id;
    std::string name;
    std::string ip;
    std::string mode;
    bool        online = false;
    std::map<std::string, std::string> dps;
};

struct CommandInfo {
    std::string name;
    std::string type;
    std::string values;
    std::string extra;
};

class ServerApi {
public:
    explicit ServerApi(std::string base_url = "http://localhost:8080");
    ~ServerApi();

    std::vector<DeviceInfo> list_devices();

    std::vector<std::pair<std::string, std::string>> list_ir_keys(const std::string& device_id);

    std::vector<CommandInfo> list_commands(const std::string& device_id);

    bool send_command(const std::string& device_id, const std::string& json_body);

    DeviceStatus get_device_status(const std::string& device_id);

private:
    std::string base_;
    std::string get(const std::string& path);
    std::string post(const std::string& path, const std::string& body);
};
