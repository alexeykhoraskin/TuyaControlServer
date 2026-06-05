// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <any>
#include <chrono>
#include <mutex>
#include <atomic>
#include <memory>
#include <functional>
#include <optional>

namespace tuya {

// ── Constants ──
constexpr uint16_t DEFAULT_PORT      = 6668;
constexpr uint16_t DISCOVERY_PORT    = 6666;
constexpr uint32_t PREFIX_MAGIC      = 0x000055AA;
constexpr uint32_t SUFFIX_MAGIC      = 0x0000AA55;
constexpr size_t   FRAME_HEADER_LEN  = 16;
constexpr size_t   HMAC_SUFFIX_LEN   = 36;  // HMAC(32) + suffix(4)
constexpr size_t   CRC_SUFFIX_LEN    = 8;   // CRC(4) + suffix(4)
constexpr int      CONNECT_TIMEOUT_S = 5;

// ── Commands ──
enum Cmd : uint32_t {
    CMD_HEARTBEAT      = 0x09,
    CMD_QUERY_STATUS   = 0x06,
    CMD_CONTROL        = 0x07,
    CMD_CONTROL_NEW    = 0x0D,
    CMD_STATUS_NOTIFY  = 0x08,
    CMD_DP_QUERY       = 0x0A,
    CMD_DP_QUERY_NEW   = 0x10,
    CMD_DP_NOTIFY      = 0x0B,
    CMD_DP_RESET       = 0x0C,
    CMD_SESS_KEY_START = 0x03,
    CMD_SESS_KEY_RESP  = 0x04,
    CMD_SESS_KEY_FIN   = 0x05,
};

// ── Device info ──
struct DeviceInfo {
    std::string id;
    std::string ip;
    std::string name;
    std::string local_key;   // 16 raw ASCII characters
    std::string product_id;
    std::string product_name;
    std::string version;     // e.g. "3.4"
    std::string mac;
    bool        encrypt = false;
    bool        online = true;   // cloud connectivity status (from cloud API)
    std::vector<std::string> status_codes; // function codes ordered as from cloud

    // Infrared device fields
    std::string ir_hub_id;       // parent Smart IR hub device ID
    int         ir_category_id = 0;   // category ID (10=Light, etc.)
    int         ir_remote_index = 0;  // remote index from IR hub

    bool has_ip() const { return !ip.empty() && ip != "0.0.0.0"; }
    bool is_infrared() const { return !ir_hub_id.empty(); }
};

// ── Connection mode ──
enum class ConnMode {
    NONE,
    LOCAL_V2,
    LOCAL_V34,
    CLOUD
};

// ── Device status ──
using DpsMap = std::map<std::string, std::any>;

// ── Tuya frame ──
struct Frame {
    uint32_t seq = 0;
    uint32_t cmd = 0;
    std::vector<uint8_t> payload;
};

// ── Cloud config ──
struct CloudConfig {
    std::string access_id;
    std::string access_secret;
    std::string region = "eu";
    std::string base_url;  // auto-built from region
};

// ── Server config ──
struct ServerConfig {
    std::string listen_addr = "0.0.0.0";
    int         port        = 8080;
    int         poll_interval_s = 30;
    std::optional<CloudConfig> cloud;
    std::vector<DeviceInfo>    devices;
};

// ── Device client (shared state) ──
struct DeviceState {
    DeviceInfo       info;
    ConnMode         mode = ConnMode::NONE;
    DpsMap           dps;
    std::string      error;
    std::chrono::steady_clock::time_point last_seen;
    int              sock_fd = -1;
    std::mutex       mtx;

    // v3.4 session state
    std::vector<uint8_t> local_nonce;
    std::vector<uint8_t> remote_nonce;
    std::vector<uint8_t> session_key;

    bool cloud_only = false;  // device configured with ip=0.0.0.0, use cloud only
};

// ── Helpers ──
inline std::string bytes_to_hex(const std::vector<uint8_t>& data) {
    static const char hex[] = "0123456789abcdef";
    std::string out(data.size() * 2, 0);
    for (size_t i = 0; i < data.size(); i++) {
        out[i*2]   = hex[data[i] >> 4];
        out[i*2+1] = hex[data[i] & 0xF];
    }
    return out;
}

inline std::vector<uint8_t> hex_to_bytes(const std::string& hex) {
    std::vector<uint8_t> out(hex.size() / 2);
    for (size_t i = 0; i < out.size(); i++) {
        auto h = [](char c) { return c <= '9' ? c - '0' : (c & 0xDF) - 'A' + 10; };
        out[i] = (h(hex[i*2]) << 4) | h(hex[i*2+1]);
    }
    return out;
}

} // namespace tuya
