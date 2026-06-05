// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#pragma once

#include "types.h"
#include "crypto.h"
#include "frame.h"
#include <string>
#include <functional>
#include <thread>

namespace tuya {

class LocalDevice {
public:
    using StatusCallback = std::function<void(const DpsMap&)>;

    explicit LocalDevice(DeviceState& state);
    ~LocalDevice();

    // Connect and try session key negotiation
    bool connect();

    // Send DP_QUERY_NEW → returns parsed DPS
    std::optional<DpsMap> query_status();

    // Send CONTROL_NEW command
    bool send_commands(const DpsMap& commands);

    // Send heartbeat
    bool heartbeat();

    // Poll loop (blocking, runs until disconnect)
    void poll_loop(int interval_s, StatusCallback cb);

    // Close connection
    void disconnect();

    bool is_connected() const;

private:
    DeviceState& state_;
    AesEcb*      cipher_ = nullptr;      // raw local key cipher
    AesEcb*      sess_cipher_ = nullptr; // session key cipher (v3.4)
    std::atomic<uint32_t> seq_{0};
    std::atomic<bool>     connected_{false};
    std::thread           poll_thread_;

    bool negotiate_session_key();
    bool send_frame(uint32_t cmd, const std::vector<uint8_t>& encrypted_payload);
    Frame read_frame();
    Frame read_v34_response();

    std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plaintext);
    std::vector<uint8_t> decrypt(const std::vector<uint8_t>& ciphertext);

    std::vector<uint8_t> hmac_key() const;
    std::vector<uint8_t> version_header() const;

    int  sock_fd_ = -1;
    void close_sock();
};

} // namespace tuya
