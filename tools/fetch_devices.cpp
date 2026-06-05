// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#include "cloud_api.h"
#include "config.h"
#include "crypto.h"
#include <iostream>
#include <curl/curl.h>
#include <chrono>
#include <openssl/sha.h>

static size_t write_cb(void* data, size_t size, size_t nmemb, std::string* out) {
    out->append((char*)data, size * nmemb);
    return size * nmemb;
}

static std::string sha256_empty_hex() {
    unsigned char md[32];
    SHA256((unsigned char*)"", 0, md);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i*2, 3, "%02x", md[i]);
    return std::string(hex, 64);
}

static std::string hmac_sign(const std::string& str, const std::string& key) {
    std::vector<uint8_t> k(key.begin(), key.end());
    std::vector<uint8_t> d(str.begin(), str.end());
    auto h = tuya::hmac_sha256(k, d);
    char hex[65];
    for (size_t i = 0; i < h.size(); i++) snprintf(hex + i*2, 3, "%02X", h[i]);
    return std::string(hex, 64);
}

static std::string sign_for(const std::string& access_id, const std::string& access_secret,
                             const std::string& method, const std::string& path,
                             int64_t ts, const std::string& token = "") {
    auto content_hex = sha256_empty_hex();
    auto sign_str = access_id + token + std::to_string(ts) +
                    method + "\n" + content_hex + "\n\n" + path;
    return hmac_sign(sign_str, access_secret);
}

static std::string raw_get(const std::string& url, const std::string& access_id,
                            const std::string& access_secret, const std::string& token = "") {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Extract path from URL
    auto slash = url.find('/', url.find("://") + 3);
    auto path = (slash == std::string::npos) ? "/" : url.substr(slash);

    auto sign = sign_for(access_id, access_secret, "GET", path, ts, token);

    struct curl_slist* h = nullptr;
    h = curl_slist_append(h, ("client_id: " + access_id).c_str());
    h = curl_slist_append(h, ("sign: " + sign).c_str());
    h = curl_slist_append(h, ("t: " + std::to_string(ts)).c_str());
    h = curl_slist_append(h, "sign_method: HMAC-SHA256");
    if (!token.empty())
        h = curl_slist_append(h, ("access_token: " + token).c_str());

    std::string resp;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, h);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_perform(curl);
    curl_slist_free_all(h);
    curl_easy_cleanup(curl);
    return resp;
}

static std::string json_str(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + key.size() + 2);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    auto end = json.find('"', pos);
    return (end == std::string::npos) ? json.substr(pos) : json.substr(pos, end - pos);
}

static std::vector<tuya::DeviceInfo> parse_device_list(const std::string& resp) {
    std::vector<tuya::DeviceInfo> devices;
    auto res_start = resp.find("\"result\":");
    if (res_start == std::string::npos) return devices;
    auto arr_start = resp.find('[', res_start);
    if (arr_start == std::string::npos) return devices;
    size_t pos = arr_start + 1;
    while (pos < resp.size()) {
        while (pos < resp.size() && (resp[pos] == ' ' || resp[pos] == '\n' || resp[pos] == '\r' || resp[pos] == '\t'))
            pos++;
        if (pos >= resp.size() || resp[pos] == ']') break;
        if (resp[pos] == '{') {
            int depth = 1;
            size_t obj_start = pos++;
            while (pos < resp.size() && depth > 0) {
                if (resp[pos] == '{') depth++;
                else if (resp[pos] == '}') depth--;
                pos++;
            }
            std::string obj = resp.substr(obj_start, pos - obj_start);
            tuya::DeviceInfo di;
            di.id           = json_str(obj, "id");
            di.name         = json_str(obj, "name");
            di.local_key    = json_str(obj, "local_key");
            di.ip           = json_str(obj, "ip");
            di.version      = json_str(obj, "version");
            di.product_id   = json_str(obj, "product_id");
            di.product_name = json_str(obj, "product_name");
            di.mac          = json_str(obj, "mac");
            if (!di.id.empty())
                devices.push_back(di);
        } else {
            pos++;
        }
    }
    return devices;
}

int main() {
    auto cfg = tuya::load_config("config.local.json");
    if (!cfg.cloud) {
        std::cerr << "No cloud config in config.local.json" << std::endl;
        return 1;
    }

    auto base = cfg.cloud->base_url.empty()
        ? "https://openapi.tuyaeu.com" : cfg.cloud->base_url;

    // Get token
    auto tok_resp = raw_get(base + "/v1.0/token?grant_type=1",
                             cfg.cloud->access_id, cfg.cloud->access_secret);

    auto tok_start = tok_resp.find("\"access_token\":\"");
    if (tok_start == std::string::npos) {
        std::cerr << "Token failed: " << tok_resp << std::endl;
        return 1;
    }
    tok_start += 16;
    auto tok_end = tok_resp.find('"', tok_start);
    auto token = tok_resp.substr(tok_start, tok_end - tok_start);
    std::cerr << "Token: " << token << std::endl;

    // Extract uid from token response
    std::string uid;
    {
        auto uid_start = tok_resp.find("\"uid\":\"");
        if (uid_start != std::string::npos) {
            uid_start += 7;
            auto uid_end = tok_resp.find('"', uid_start);
            uid = tok_resp.substr(uid_start, uid_end - uid_start);
        }
    }
    std::cerr << "UID: " << uid << std::endl;

    // List devices via user endpoint
    auto dev_resp = raw_get(base + "/v1.0/users/" + uid + "/devices",
                             cfg.cloud->access_id, cfg.cloud->access_secret, token);

    std::cerr << "Raw devices response: " << dev_resp << std::endl;

    auto devs = parse_device_list(dev_resp);
    if (devs.empty()) {
        std::cerr << "No devices found in response" << std::endl;
        return 1;
    }

    std::cout << "{"
              << "\"listen\": \"0.0.0.0\","
              << "\"port\": 8080,"
              << "\"poll_interval\": 30,"
              << "\"access_id\": \"" << cfg.cloud->access_id << "\","
              << "\"access_secret\": \"" << cfg.cloud->access_secret << "\","
              << "\"region\": \"" << cfg.cloud->region << "\","
              << "\"devices\": [";

    bool first = true;
    for (auto& d : devs) {
        if (!first) std::cout << ",";
        first = false;
        std::cout << "{"
                  << "\"id\":\"" << d.id << "\","
                  << "\"ip\":\"" << d.ip << "\","
                  << "\"local_key\":\"" << d.local_key << "\","
                  << "\"name\":\"" << d.name << "\","
                  << "\"version\":\"" << d.version << "\","
                  << "\"product_id\":\"" << d.product_id << "\","
                  << "\"mac\":\"" << d.mac << "\","
                  << "\"encrypt\":true"
                  << "}";
    }
    std::cout << "]}" << std::endl;

    return 0;
}
