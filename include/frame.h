// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#pragma once

#include "types.h"
#include <vector>
#include <cstdint>

namespace tuya {

// ── v2.x CRC32 frame ──
std::vector<uint8_t> encode_frame(uint32_t seq, uint32_t cmd,
                                   const std::vector<uint8_t>& payload);

Frame decode_frame(const std::vector<uint8_t>& raw);

// ── v3.4 HMAC-SHA256 frame ──
std::vector<uint8_t> encode_frame_v34(uint32_t seq, uint32_t cmd,
                                       const std::vector<uint8_t>& encrypted_payload,
                                       const std::vector<uint8_t>& hmac_key);

Frame decode_frame_v34(const std::vector<uint8_t>& raw,
                        const std::vector<uint8_t>& hmac_key);

// ── CRC32 helper ──
uint32_t crc32(const std::vector<uint8_t>& data);

} // namespace tuya
