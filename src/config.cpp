// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#include "config.h"
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace tuya {

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open config: " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string get_field(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + key.size() + 2);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    if (pos >= json.size()) return "";
    if (json[pos] == '"') {
        pos++;
        auto end = json.find('"', pos);
        return (end == std::string::npos) ? json.substr(pos) : json.substr(pos, end - pos);
    }
    auto end = json.find_first_of(",}\n", pos);
    return json.substr(pos, end - pos);
}

ServerConfig load_config(const std::string& path) {
    auto raw = read_file(path);
    ServerConfig cfg;

    // Parse simple JSON manually (no dependency)
    auto listen = get_field(raw, "listen");
    if (!listen.empty()) cfg.listen_addr = listen;

    auto port_str = get_field(raw, "port");
    if (!port_str.empty()) cfg.port = std::stoi(port_str);

    auto poll_str = get_field(raw, "poll_interval");
    if (!poll_str.empty()) cfg.poll_interval_s = std::stoi(poll_str);

    // Cloud config
    auto cid = get_field(raw, "access_id");
    auto csec = get_field(raw, "access_secret");
    if (!cid.empty() && !csec.empty()) {
        CloudConfig cc;
        cc.access_id = cid;
        cc.access_secret = csec;
        auto region = get_field(raw, "region");
        if (!region.empty()) cc.region = region;
        cc.base_url = "https://openapi.tuyaeu.com";
        cfg.cloud = cc;
    }

    // Parse devices array
    auto dev_key = raw.find("\"devices\"");
    if (dev_key != std::string::npos) {
        auto arr_start = raw.find('[', dev_key);
        if (arr_start != std::string::npos) {
            size_t pos = arr_start + 1;
            while (pos < raw.size()) {
                while (pos < raw.size() && (raw[pos] == ' ' || raw[pos] == '\n' || raw[pos] == '\r' || raw[pos] == '\t')) pos++;
                if (pos >= raw.size() || raw[pos] == ']') break;
                if (raw[pos] == '{') {
                    int depth = 1;
                    size_t obj_start = pos++;
                    while (pos < raw.size() && depth > 0) {
                        if (raw[pos] == '{') depth++;
                        else if (raw[pos] == '}') depth--;
                        pos++;
                    }
                    std::string obj = raw.substr(obj_start, pos - obj_start);
                    DeviceInfo di;
                    di.id        = get_field(obj, "id");
                    di.ip        = get_field(obj, "ip");
                    di.local_key = get_field(obj, "local_key");
                    di.name      = get_field(obj, "name");
                    di.version   = get_field(obj, "version");
                    di.ir_hub_id = get_field(obj, "ir_hub_id");
                    auto icat    = get_field(obj, "ir_category_id");
                    if (!icat.empty()) di.ir_category_id = std::stoi(icat);
                    auto iri     = get_field(obj, "ir_remote_index");
                    if (!iri.empty()) di.ir_remote_index = std::stoi(iri);
                    auto enc     = get_field(obj, "encrypt");
                    if (enc == "true") di.encrypt = true;
                    if (!di.id.empty())
                        cfg.devices.push_back(di);
                } else {
                    pos++;
                }
            }
        }
    }
    return cfg;
}

} // namespace tuya
