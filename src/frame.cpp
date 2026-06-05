// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#include "frame.h"
#include "crypto.h"
#include <cstring>
#include <stdexcept>
#include <arpa/inet.h>

namespace tuya {

static uint32_t crc32_table[256];
static bool crc32_initialized = false;

static void init_crc32() {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++)
            c = (c >> 1) ^ (c & 1 ? 0xEDB88320 : 0);
        crc32_table[i] = c;
    }
    crc32_initialized = true;
}

uint32_t crc32(const std::vector<uint8_t>& data) {
    if (!crc32_initialized) init_crc32();
    uint32_t c = 0xFFFFFFFF;
    for (auto byte : data)
        c = crc32_table[(c ^ byte) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFF;
}

// ── v2.x frame ──

std::vector<uint8_t> encode_frame(uint32_t seq, uint32_t cmd,
                                   const std::vector<uint8_t>& payload) {
    size_t total = 16 + payload.size() + 8;  // header + payload + CRC + suffix
    std::vector<uint8_t> buf(total, 0);

    auto put32 = [&](size_t off, uint32_t v) {
        buf[off+0] = (v >> 24) & 0xFF;
        buf[off+1] = (v >> 16) & 0xFF;
        buf[off+2] = (v >>  8) & 0xFF;
        buf[off+3] =  v        & 0xFF;
    };

    put32(0,  PREFIX_MAGIC);
    put32(4,  seq);
    put32(8,  cmd);
    put32(12, (uint32_t)payload.size());

    if (!payload.empty())
        memcpy(buf.data() + 16, payload.data(), payload.size());

    uint32_t crc = crc32(payload);
    put32(16 + payload.size(), crc);
    put32(16 + payload.size() + 4, SUFFIX_MAGIC);

    return buf;
}

Frame decode_frame(const std::vector<uint8_t>& raw) {
    if (raw.size() < 16 + 8)
        throw std::runtime_error("frame too short");

    auto get32 = [&](size_t off) {
        return ((uint32_t)raw[off] << 24) |
               ((uint32_t)raw[off+1] << 16) |
               ((uint32_t)raw[off+2] << 8) |
               (uint32_t)raw[off+3];
    };

    uint32_t prefix = get32(0);
    if (prefix != PREFIX_MAGIC)
        throw std::runtime_error("invalid prefix");

    uint32_t seq  = get32(4);
    uint32_t cmd  = get32(8);
    uint32_t plen = get32(12);

    if (16 + plen + 8 > raw.size())
        throw std::runtime_error("payload exceeds frame");

    std::vector<uint8_t> payload(raw.begin() + 16, raw.begin() + 16 + plen);

    // Verify CRC
    uint32_t expected = get32(16 + plen);
    uint32_t actual = crc32(payload);
    if (expected != actual)
        throw std::runtime_error("CRC mismatch");

    uint32_t suffix = get32(16 + plen + 4);
    if (suffix != SUFFIX_MAGIC)
        throw std::runtime_error("invalid suffix");

    return {seq, cmd, std::move(payload)};
}

// ── v3.4 frame ──

std::vector<uint8_t> encode_frame_v34(uint32_t seq, uint32_t cmd,
                                       const std::vector<uint8_t>& encrypted_payload,
                                       const std::vector<uint8_t>& hmac_key) {
    // length = encrypted_payload + HMAC(32) + suffix(4)
    uint32_t total_len = (uint32_t)(encrypted_payload.size() + 36);
    size_t frame_size = 16 + total_len;
    std::vector<uint8_t> buf(frame_size, 0);

    auto put32 = [&](size_t off, uint32_t v) {
        buf[off+0] = (v >> 24) & 0xFF;
        buf[off+1] = (v >> 16) & 0xFF;
        buf[off+2] = (v >>  8) & 0xFF;
        buf[off+3] =  v        & 0xFF;
    };

    put32(0,  PREFIX_MAGIC);
    put32(4,  seq);
    put32(8,  cmd);
    put32(12, total_len);

    if (!encrypted_payload.empty())
        memcpy(buf.data() + 16, encrypted_payload.data(), encrypted_payload.size());

    // HMAC over header + encrypted_payload
    std::vector<uint8_t> hmac_data(buf.begin(), buf.begin() + 16 + encrypted_payload.size());
    auto hmac_val = hmac_sha256(hmac_key, hmac_data);

    size_t hmac_off = 16 + encrypted_payload.size();
    memcpy(buf.data() + hmac_off, hmac_val.data(), 32);
    put32(hmac_off + 32, SUFFIX_MAGIC);

    return buf;
}

Frame decode_frame_v34(const std::vector<uint8_t>& raw,
                        const std::vector<uint8_t>& hmac_key) {
    if (raw.size() < 16 + 36)
        throw std::runtime_error("v34 frame too short");

    auto get32 = [&](size_t off) {
        return ((uint32_t)raw[off] << 24) |
               ((uint32_t)raw[off+1] << 16) |
               ((uint32_t)raw[off+2] << 8) |
               (uint32_t)raw[off+3];
    };

    uint32_t prefix = get32(0);
    if (prefix != PREFIX_MAGIC)
        throw std::runtime_error("v34 invalid prefix");

    uint32_t seq    = get32(4);
    uint32_t cmd    = get32(8);
    uint32_t length = get32(12);  // retcode(4) + encrypted_payload + HMAC(32) + suffix(4)

    int payload_len = length - 40;
    if (payload_len < 0)
        throw std::runtime_error("v34 payload length negative");

    // Skip 4-byte retcode between header and encrypted payload
    std::vector<uint8_t> encrypted_payload(raw.begin() + 20,
                                            raw.begin() + 20 + payload_len);

    // HMAC verification: data = header + retcode + encrypted_payload
    size_t hmac_span = 20 + payload_len;
    std::vector<uint8_t> hmac_data(raw.begin(), raw.begin() + hmac_span);
    auto expected = hmac_sha256(hmac_key, hmac_data);

    auto actual = std::vector<uint8_t>(raw.begin() + hmac_span,
                                        raw.begin() + hmac_span + 32);
    if (expected != actual)
        throw std::runtime_error("v34 HMAC mismatch");

    return {seq, cmd, std::move(encrypted_payload)};
}

} // namespace tuya
