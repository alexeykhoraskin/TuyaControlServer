// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#include "local_device.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <chrono>
#include <thread>

namespace tuya {

LocalDevice::LocalDevice(DeviceState& state)
    : state_(state)
{
    auto key = parse_local_key(state.info.local_key);
    cipher_ = new AesEcb(key);
}

LocalDevice::~LocalDevice() {
    disconnect();
    delete cipher_;
    delete sess_cipher_;
}

void LocalDevice::close_sock() {
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
    connected_ = false;
}

bool LocalDevice::is_connected() const {
    return connected_;
}

// ── Connect ──

bool LocalDevice::connect() {
    std::lock_guard<std::mutex> lock(state_.mtx);
    close_sock();

    if (!state_.info.has_ip()) return false;

    sock_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd_ < 0) return false;

    // Non-blocking connect with timeout
    fcntl(sock_fd_, F_SETFL, O_NONBLOCK);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(DEFAULT_PORT);
    inet_pton(AF_INET, state_.info.ip.c_str(), &addr.sin_addr);

    ::connect(sock_fd_, (struct sockaddr*)&addr, sizeof(addr));

    struct timeval tv{CONNECT_TIMEOUT_S, 0};
    fd_set fdset;
    FD_ZERO(&fdset);
    FD_SET(sock_fd_, &fdset);

    if (select(sock_fd_ + 1, nullptr, &fdset, nullptr, &tv) != 1) {
        close_sock();
        return false;
    }

    // Back to blocking
    fcntl(sock_fd_, F_SETFL, 0);

    // TCP_NODELAY
    int one = 1;
    setsockopt(sock_fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    state_.mode = ConnMode::LOCAL_V2;
    connected_ = true;
    return true;
}

// ── Session key negotiation ──

bool LocalDevice::negotiate_session_key() {
    auto raw_key = parse_local_key(state_.info.local_key);
    AesEcb local_cipher(raw_key);

    // Step 1: send encrypted local nonce
    state_.local_nonce = std::vector<uint8_t>(16);
    const char* nonce = "0123456789abcdef";
    memcpy(state_.local_nonce.data(), nonce, 16);

    auto enc_nonce = local_cipher.encrypt(state_.local_nonce);
    uint32_t seq1 = ++seq_;
    auto frame1 = encode_frame_v34(seq1, CMD_SESS_KEY_START, enc_nonce, raw_key);
    if (::write(sock_fd_, frame1.data(), frame1.size()) < 0) return false;

    // Step 2: read response
    Frame resp;
    try {
        resp = read_v34_response();
    } catch (...) { return false; }
    if (resp.cmd != CMD_SESS_KEY_RESP) return false;

    // Decrypt with local_key (same as Go implementation)
    std::vector<uint8_t> decrypted;
    try {
        decrypted = local_cipher.decrypt(resp.payload);
    } catch (...) { return false; }

    if (decrypted.size() < 48) return false;

    state_.remote_nonce.assign(decrypted.begin(), decrypted.begin() + 16);
    auto expected_hmac = std::vector<uint8_t>(decrypted.begin() + 16, decrypted.begin() + 48);

    // Verify HMAC(local_key, local_nonce)
    auto computed = hmac_sha256(raw_key, state_.local_nonce);
    if (computed != expected_hmac) return false;

    // Step 3: send encrypted HMAC(remote_nonce)
    auto finish_hmac = hmac_sha256(raw_key, state_.remote_nonce);
    auto enc_finish = local_cipher.encrypt(finish_hmac);
    uint32_t seq3 = ++seq_;
    auto frame3 = encode_frame_v34(seq3, CMD_SESS_KEY_FIN, enc_finish, raw_key);
    if (::write(sock_fd_, frame3.data(), frame3.size()) < 0) return false;

    // Finalize: XOR nonces, encrypt with raw key (no padding)
    std::vector<uint8_t> xored(16);
    for (int i = 0; i < 16; i++)
        xored[i] = state_.local_nonce[i] ^ state_.remote_nonce[i];

    state_.session_key = local_cipher.encrypt_block(xored);
    sess_cipher_ = new AesEcb(state_.session_key);
    return true;
}

// ── Helpers ──

std::vector<uint8_t> LocalDevice::hmac_key() const {
    if (state_.mode == ConnMode::LOCAL_V34 && !state_.session_key.empty())
        return state_.session_key;
    return parse_local_key(state_.info.local_key);
}

std::vector<uint8_t> LocalDevice::version_header() const {
    if (state_.mode == ConnMode::LOCAL_V34) {
        auto hdr = std::vector<uint8_t>({'3', '.', '4'});
        hdr.insert(hdr.end(), 12, 0);
        return hdr;
    }
    return {};
}

std::vector<uint8_t> LocalDevice::encrypt(const std::vector<uint8_t>& plaintext) {
    if (sess_cipher_) return sess_cipher_->encrypt(plaintext);
    return cipher_->encrypt(plaintext);
}

std::vector<uint8_t> LocalDevice::decrypt(const std::vector<uint8_t>& ciphertext) {
    std::vector<uint8_t> pt;
    if (sess_cipher_) pt = sess_cipher_->decrypt(ciphertext);
    else              pt = cipher_->decrypt(ciphertext);

    // Strip version header for v3.4 responses
    if (state_.mode == ConnMode::LOCAL_V34 && pt.size() >= 3 &&
        pt[0] == '3' && pt[1] == '.') {
        auto pos = std::find(pt.begin(), pt.end(), '{');
        if (pos != pt.end()) pt.erase(pt.begin(), pos);
    }
    return pt;
}

bool LocalDevice::send_frame(uint32_t cmd, const std::vector<uint8_t>& encrypted_payload) {
    auto key = hmac_key();
    auto frame = (state_.mode == ConnMode::LOCAL_V34)
        ? encode_frame_v34(seq_, cmd, encrypted_payload, key)
        : encode_frame(seq_, cmd, encrypted_payload);
    return ::write(sock_fd_, frame.data(), frame.size()) > 0;
}

Frame LocalDevice::read_v34_response() {
    // Use poll with 3-second timeout (like Go's SetReadDeadline)
    struct pollfd pfd;
    pfd.fd = sock_fd_;
    pfd.events = POLLIN;
    int ret = ::poll(&pfd, 1, 3000);
    if (ret <= 0) throw std::runtime_error("read: timeout");

    std::vector<uint8_t> buf(4096);
    auto n = ::read(sock_fd_, buf.data(), buf.size());
    if (n <= 0) throw std::runtime_error("read: EOF");
    buf.resize(n);
    auto key = parse_local_key(state_.info.local_key);
    return decode_frame_v34(buf, key);
}

Frame LocalDevice::read_frame() {
    std::vector<uint8_t> buf(4096);
    auto n = ::read(sock_fd_, buf.data(), buf.size());
    if (n <= 0) throw std::runtime_error("read: EOF");
    buf.resize(n);


    if (state_.mode == ConnMode::LOCAL_V34) {
        auto key = hmac_key();
        return decode_frame_v34(buf, key);
    }
    return decode_frame(buf);
}

// ── Query Status ──

std::optional<DpsMap> LocalDevice::query_status() {
    if (!connected_) return std::nullopt;

    std::lock_guard<std::mutex> lock(state_.mtx);
    std::vector<uint8_t> payload = {'{', '}'};

    // Prepend version header if not in NO_PROTOCOL_HEADER_CMDS
    // DP_QUERY_NEW IS in the no-header list, so no version header
    auto enc = encrypt(payload);
    seq_++;
    if (!send_frame(CMD_DP_QUERY_NEW, enc)) return std::nullopt;

    Frame frame;
    try {
        frame = read_frame();
    } catch (...) { return std::nullopt; }

    if (frame.cmd != CMD_DP_QUERY_NEW && frame.cmd != CMD_STATUS_NOTIFY &&
        frame.cmd != CMD_DP_NOTIFY && frame.cmd != CMD_DP_QUERY)
        return std::nullopt;

    std::vector<uint8_t> plaintext;
    try {
        plaintext = decrypt(frame.payload);
    } catch (...) { return std::nullopt; }

    // Parse JSON — naive approach, find "dps" object in the response
    // For production: use a JSON library
    std::string json_str(plaintext.begin(), plaintext.end());
    DpsMap dps;

    // Extract "dps":{...}  — very simple parser for known format
    auto dps_pos = json_str.find("\"dps\":{");
    if (dps_pos != std::string::npos) {
        dps_pos += 7; // past "dps":{
        int depth = 1;
        size_t i = dps_pos;
        while (i < json_str.size() && depth > 0) {
            if (json_str[i] == '{') depth++;
            else if (json_str[i] == '}') depth--;
            i++;
        }
        std::string dps_json = json_str.substr(dps_pos, i - dps_pos - 1);
        // Parse key-value pairs
        size_t pos = 0;
        while (pos < dps_json.size()) {
            // Find key
            auto kstart = dps_json.find('"', pos);
            if (kstart == std::string::npos) break;
            auto kend = dps_json.find('"', kstart + 1);
            if (kend == std::string::npos) break;
            std::string key = dps_json.substr(kstart + 1, kend - kstart - 1);

            // Find value
            auto vstart = dps_json.find(':', kend + 1);
            if (vstart == std::string::npos) break;
            vstart++;
            while (vstart < dps_json.size() && dps_json[vstart] == ' ') vstart++;

            // Determine type and store
            if (dps_json[vstart] == '"') {
                // String value
                auto vend = dps_json.find('"', vstart + 1);
                if (vend == std::string::npos) break;
                dps[key] = dps_json.substr(vstart + 1, vend - vstart - 1);
                pos = vend + 1;
            } else if (dps_json.substr(vstart, 4) == "true") {
                dps[key] = true;
                pos = vstart + 4;
            } else if (dps_json.substr(vstart, 5) == "false") {
                dps[key] = false;
                pos = vstart + 5;
            } else {
                // Number
                auto vend = dps_json.find_first_of(",}", vstart);
                if (vend == std::string::npos) { vend = dps_json.size(); }
                auto num_str = dps_json.substr(vstart, vend - vstart);
                // Try int first
                try { dps[key] = std::stoi(num_str); }
                catch (...) { dps[key] = num_str; }
                pos = vend;
            }
            // Skip comma
            if (pos < dps_json.size() && dps_json[pos] == ',') pos++;
        }
    }

    state_.dps = dps;
    state_.last_seen = std::chrono::steady_clock::now();
    return dps;
}

// ── Send Commands ──

bool LocalDevice::send_commands(const DpsMap& commands) {
    if (!connected_) return false;
    std::lock_guard<std::mutex> lock(state_.mtx);

    // Build JSON payload
    std::string json;
    if (state_.mode == ConnMode::LOCAL_V34) {
        // v3.4 format: {"protocol":5,"t":<ts>,"data":{"dps":{...}}}
        auto ts = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        json = "{\"protocol\":5,\"t\":" + std::to_string(ts) + ",\"data\":{\"dps\":{";
        bool first = true;
        for (auto& [k, v] : commands) {
            if (!first) json += ",";
            first = false;
            json += "\"" + k + "\":";
            if (v.type() == typeid(bool)) {
                json += std::any_cast<bool>(v) ? "true" : "false";
            } else if (v.type() == typeid(int)) {
                json += std::to_string(std::any_cast<int>(v));
            } else if (v.type() == typeid(double)) {
                json += std::to_string(std::any_cast<double>(v));
            } else {
                try { json += "\"" + std::any_cast<std::string>(v) + "\""; }
                catch (...) { json += "\"null\""; }
            }
        }
        json += "}}}}";

        auto version_hdr = version_header();
        std::vector<uint8_t> payload(json.begin(), json.end());
        payload.insert(payload.begin(), version_hdr.begin(), version_hdr.end());
        auto enc = encrypt(payload);
        seq_++;
        if (!send_frame(CMD_CONTROL_NEW, enc)) return false;
    } else {
        // v2.x format
        json = "{\"devId\":\"" + state_.info.id + "\",\"uid\":\"" +
               state_.info.id + "\",\"dps\":{";
        bool first = true;
        for (auto& [k, v] : commands) {
            if (!first) json += ",";
            first = false;
            json += "\"" + k + "\":";
            if (v.type() == typeid(bool)) {
                json += std::any_cast<bool>(v) ? "true" : "false";
            } else if (v.type() == typeid(int)) {
                json += std::to_string(std::any_cast<int>(v));
            } else {
                try { json += "\"" + std::any_cast<std::string>(v) + "\""; }
                catch (...) { json += "\"null\""; }
            }
        }
        json += "}}";

        std::vector<uint8_t> payload(json.begin(), json.end());
        auto enc = encrypt(payload);
        seq_++;
        if (!send_frame(CMD_CONTROL_NEW, enc)) return false;
    }

    // Read response
    try {
        auto frame = read_frame();
        return frame.cmd == CMD_CONTROL_NEW;
    } catch (...) { return false; }
}

// ── Heartbeat ──

bool LocalDevice::heartbeat() {
    if (!connected_) return false;
    std::lock_guard<std::mutex> lock(state_.mtx);
    seq_++;
    if (!send_frame(CMD_HEARTBEAT, {})) return false;
    try {
        read_frame();
        return true;
    } catch (...) { return false; }
}

// ── Poll loop ──

void LocalDevice::poll_loop(int interval_s, StatusCallback cb) {
    // Immediate first poll
    auto dps = query_status();
    if (dps && cb) cb(*dps);

    while (connected_) {
        std::this_thread::sleep_for(std::chrono::seconds(interval_s));
        if (!connected_) break;

        dps = query_status();
        if (!dps) {
            // Reconnect
            if (!connect()) break;
            dps = query_status();
            if (!dps) break;
        }
        if (cb) cb(*dps);
    }
}

// ── Disconnect ──

void LocalDevice::disconnect() {
    if (poll_thread_.joinable()) poll_thread_.join();
    std::lock_guard<std::mutex> lock(state_.mtx);
    close_sock();
    delete sess_cipher_;
    sess_cipher_ = nullptr;
    state_.session_key.clear();
}

} // namespace tuya
