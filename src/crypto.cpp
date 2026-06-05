// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#include "crypto.h"
#include <cstring>
#include <stdexcept>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/md5.h>

namespace tuya {

// ── AES-ECB-128 ──

AesEcb::AesEcb(const std::vector<uint8_t>& key)
    : key_(key)
{
    if (key.size() != 16)
        throw std::invalid_argument("AES key must be 16 bytes");
}

static std::vector<uint8_t> pkcs7_pad(const std::vector<uint8_t>& data, size_t block_size) {
    size_t pad = block_size - (data.size() % block_size);
    auto out = data;
    out.insert(out.end(), pad, static_cast<uint8_t>(pad));
    return out;
}

static std::vector<uint8_t> pkcs7_unpad(const std::vector<uint8_t>& data) {
    if (data.empty()) return data;
    uint8_t pad = data.back();
    if (pad == 0 || pad > 16 || pad > data.size())
        throw std::runtime_error("invalid PKCS7 padding");
    for (size_t i = data.size() - pad; i < data.size(); i++)
        if (data[i] != pad)
            throw std::runtime_error("invalid PKCS7 padding byte");
    return std::vector<uint8_t>(data.begin(), data.end() - pad);
}

std::vector<uint8_t> AesEcb::encrypt(const std::vector<uint8_t>& plaintext) const {
    // EVP with PKCS7 padding may add an extra block, allocate extra space
    std::vector<uint8_t> out(plaintext.size() + 32, 0);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key_.data(), nullptr);
    int len = 0, total = 0;
    EVP_EncryptUpdate(ctx, out.data(), &len, plaintext.data(), (int)plaintext.size());
    total += len;
    EVP_EncryptFinal_ex(ctx, out.data() + total, &len);
    total += len;
    out.resize(total);
    EVP_CIPHER_CTX_free(ctx);
    return out;
}

std::vector<uint8_t> AesEcb::encrypt_block(const std::vector<uint8_t>& plaintext) const {
    if (plaintext.size() != 16)
        throw std::invalid_argument("encrypt_block requires exactly 16 bytes");
    std::vector<uint8_t> out(32, 0);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key_.data(), nullptr);
    EVP_CIPHER_CTX_set_padding(ctx, 0);
    int len = 0;
    EVP_EncryptUpdate(ctx, out.data(), &len, plaintext.data(), 16);
    EVP_CIPHER_CTX_free(ctx);
    out.resize(len);
    return out;
}

std::vector<uint8_t> AesEcb::decrypt(const std::vector<uint8_t>& ciphertext) const {
    if (ciphertext.size() % 16 != 0)
        throw std::runtime_error("ciphertext length not multiple of 16");

    std::vector<uint8_t> tmp(ciphertext.size() + 16, 0);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    EVP_DecryptInit_ex(ctx, EVP_aes_128_ecb(), nullptr, key_.data(), nullptr);
    int len = 0, total = 0;
    if (!EVP_DecryptUpdate(ctx, tmp.data(), &len, ciphertext.data(), (int)ciphertext.size())) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptUpdate failed");
    }
    total += len;
    if (EVP_DecryptFinal_ex(ctx, tmp.data() + total, &len) <= 0) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptFinal_ex failed (bad padding)");
    }
    total += len;
    EVP_CIPHER_CTX_free(ctx);
    tmp.resize(total);
    return tmp;
}

// ── HMAC-SHA256 ──

std::vector<uint8_t> hmac_sha256(const std::vector<uint8_t>& key,
                                  const std::vector<uint8_t>& data) {
    unsigned int len = 0;
    std::vector<uint8_t> out(EVP_MAX_MD_SIZE);
    HMAC(EVP_sha256(), key.data(), (int)key.size(),
         data.data(), data.size(), out.data(), &len);
    out.resize(len);
    return out;
}

// ── Parse key ──

std::vector<uint8_t> parse_local_key(const std::string& key_str) {
    // If 32 hex chars → decode
    if (key_str.size() == 32) {
        auto is_hex = [](char c) {
            return (c >= '0' && c <= '9') ||
                   (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        };
        bool all_hex = true;
        for (auto c : key_str) if (!is_hex(c)) { all_hex = false; break; }
        if (all_hex) return hex_to_bytes(key_str);
    }
    // Otherwise — 16 raw ASCII bytes
    if (key_str.size() != 16)
        throw std::invalid_argument("local key must be 16 raw chars or 32 hex chars");
    return std::vector<uint8_t>(key_str.begin(), key_str.end());
}

// ── Derive real_local_key: MD5(raw_key)[8:24] ──

std::vector<uint8_t> derive_real_key(const std::vector<uint8_t>& raw_key) {
    // MD5 the key
    std::vector<uint8_t> md5_out(EVP_MAX_MD_SIZE);
    unsigned int md5_len = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx, raw_key.data(), raw_key.size());
    EVP_DigestFinal_ex(ctx, md5_out.data(), &md5_len);
    EVP_MD_CTX_free(ctx);

    // Take hex string, chars [8:24], hex-decode to 16 bytes
    std::string hex_str = bytes_to_hex({md5_out.begin(), md5_out.begin() + md5_len});
    std::string chunk = hex_str.substr(8, 16);
    return hex_to_bytes(chunk);
}

} // namespace tuya
