// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#pragma once

#include "types.h"
#include <vector>
#include <cstdint>
#include <string>

namespace tuya {

// ── AES-ECB-128 with PKCS7 ──
class AesEcb {
public:
    explicit AesEcb(const std::vector<uint8_t>& key);

    // Encrypt with PKCS7 padding
    std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plaintext) const;

    // Encrypt single 16-byte block (no padding)
    std::vector<uint8_t> encrypt_block(const std::vector<uint8_t>& plaintext) const;

    // Decrypt and strip PKCS7 padding
    std::vector<uint8_t> decrypt(const std::vector<uint8_t>& ciphertext) const;

private:
    std::vector<uint8_t> key_;
};

// ── HMAC-SHA256 ──
std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key,
                                  const std::vector<uint8_t>& data);

// ── Parse local key (16 raw ASCII or 32 hex) ──
std::vector<uint8_t> parse_local_key(const std::string& key_str);

// ── MD5 chunk [8:24] → hex decode (real_local_key) ──
std::vector<uint8_t> derive_real_key(const std::vector<uint8_t>& raw_key);

} // namespace tuya
