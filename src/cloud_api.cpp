// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#include "cloud_api.h"
#include "crypto.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <map>
#include <mutex>
#include <openssl/sha.h>

#ifndef NO_CURL
#include <curl/curl.h>
#endif

namespace tuya {

#ifndef NO_CURL
static size_t write_cb(void* data, size_t size, size_t nmemb, std::string* out) {
    auto total = size * nmemb;
    out->append((char*)data, total);
    return total;
}
#endif

CloudApi::CloudApi(CloudConfig config) : config_(std::move(config)) {
    if (config_.base_url.empty())
        config_.base_url = "https://openapi.tuyaeu.com";
#ifndef NO_CURL
    curl_global_init(CURL_GLOBAL_DEFAULT);
#endif
}

bool CloudApi::is_token_expired() const {
    if (access_token_.empty()) return true;
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return (now - acquire_time_) >= (expire_s_ - 60);
}

std::string CloudApi::sign_request(const std::string& method,
                                    const std::string& path,
                                    const std::string& body,
                                    int64_t timestamp,
                                    const std::string& token) const {
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256((unsigned char*)body.data(), body.size(), md);
    std::string content_hex;   // lowercase hex for content hash
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        char hex[3];
        snprintf(hex, 3, "%02x", md[i]);
        content_hex += hex;
    }

    std::string sign_str = config_.access_id + token + std::to_string(timestamp) +
                           method + "\n" + content_hex + "\n\n" + path;

    std::vector<uint8_t> key_bytes(config_.access_secret.begin(), config_.access_secret.end());
    std::vector<uint8_t> data_bytes(sign_str.begin(), sign_str.end());
    auto hmac = hmac_sha256(key_bytes, data_bytes);

    std::string out;            // uppercase hex for final sign
    for (auto b : hmac) {
        char hex[3];
        snprintf(hex, 3, "%02X", b);
        out += hex;
    }
    return out;
}

std::string CloudApi::do_request(const std::string& method,
                                  const std::string& path,
                                  const std::string& body,
                                  bool auth) {
#ifdef NO_CURL
    (void)method; (void)path; (void)body; (void)auth;
    return "{}";
#else
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string url = config_.base_url + path;
    std::string response;
    auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto sign = sign_request(method, path, body, ts, auth ? access_token_ : "");

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, ("client_id: " + config_.access_id).c_str());
    headers = curl_slist_append(headers, ("sign: " + sign).c_str());
    headers = curl_slist_append(headers, ("t: " + std::to_string(ts)).c_str());
    headers = curl_slist_append(headers, "sign_method: HMAC-SHA256");
    if (auth && !access_token_.empty()) {
        headers = curl_slist_append(headers, ("access_token: " + access_token_).c_str());
    }
    if (method == "POST") {
        headers = curl_slist_append(headers, "Content-Type: application/json");
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    } else if (method == "GET") {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    // Disable SSL verification for simplicity
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_easy_perform(curl);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return response;
#endif
}

bool CloudApi::refresh_token() {
    // POST /v1.0/token?grant_type=1
    if (config_.access_id.empty()) return false;

    auto resp = do_request("GET", "/v1.0/token?grant_type=1", "", false);
    // Parse response for "access_token" and "expire_time"
    auto tok_start = resp.find("\"access_token\":\"");
    if (tok_start == std::string::npos) return false;
    tok_start += 16;
    auto tok_end = resp.find('"', tok_start);
    access_token_ = resp.substr(tok_start, tok_end - tok_start);

    auto exp_start = resp.find("\"expire_time\":");
    if (exp_start != std::string::npos) {
        exp_start += 14;
        auto exp_end = resp.find_first_of(",}", exp_start);
        expire_s_ = std::stoi(resp.substr(exp_start, exp_end - exp_start));
    }

    acquire_time_ = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    auto uid_start = resp.find("\"uid\":\"");
    if (uid_start != std::string::npos) {
        uid_start += 7;
        auto uid_end = resp.find('"', uid_start);
        uid_ = resp.substr(uid_start, uid_end - uid_start);
    }

    return !access_token_.empty();
}

std::optional<DpsMap> CloudApi::query_status(const std::string& device_id) {
    if (is_token_expired() && !refresh_token()) return std::nullopt;

    auto path = "/v1.0/iot-03/devices/" + device_id + "/status";
    auto resp = do_request("GET", path, "", true);

    // Parse result — {"code":"switch_led","value":true} array format
    DpsMap dps;
    size_t pos = 0;

    // Find "result": [
    auto arr_start = resp.find("\"result\":[");
    if (arr_start == std::string::npos) {
        arr_start = resp.find("\"result\": [");
        if (arr_start == std::string::npos) return dps;
    }
    pos = resp.find('[', arr_start);
    if (pos == std::string::npos) return dps;
    pos++;

    while (pos < resp.size()) {
        auto obj_start = resp.find('{', pos);
        if (obj_start == std::string::npos || obj_start >= resp.size()) break;
        auto obj_end = resp.find('}', obj_start);
        if (obj_end == std::string::npos) break;
        std::string obj = resp.substr(obj_start, obj_end - obj_start + 1);
        pos = obj_end + 1;

        // Extract "code" and "value"
        auto cpos = obj.find("\"code\":");
        if (cpos == std::string::npos) continue;
        cpos += 7;
        while (cpos < obj.size() && (obj[cpos] == ' ' || obj[cpos] == '"')) cpos++;
        std::string code;
        if (cpos < obj.size()) {
            size_t cend;
            if (obj[cpos-1] == '"') {
                // string code
                cend = obj.find('"', cpos);
                if (cend != std::string::npos) code = obj.substr(cpos, cend - cpos);
            } else {
                // integer code
                cend = obj.find_first_of(",}", cpos);
                if (cend != std::string::npos) code = obj.substr(cpos, cend - cpos);
            }
        }
        if (code.empty()) continue;

        // Extract "value"
        auto vpos = obj.find("\"value\":");
        if (vpos == std::string::npos) continue;
        vpos += 8;
        while (vpos < obj.size() && obj[vpos] == ' ') vpos++;
        if (vpos >= obj.size()) continue;

        if (obj.substr(vpos, 4) == "true") {
            dps[code] = true;
        } else if (obj.substr(vpos, 5) == "false") {
            dps[code] = false;
        } else if (obj[vpos] == '"') {
            auto vend = obj.find('"', vpos + 1);
            if (vend != std::string::npos) dps[code] = obj.substr(vpos + 1, vend - vpos - 1);
        } else {
            auto vend = obj.find_first_of(",}", vpos);
            if (vend != std::string::npos) {
                try { dps[code] = std::stoi(obj.substr(vpos, vend - vpos)); }
                catch (...) { dps[code] = obj.substr(vpos, vend - vpos); }
            }
        }
    }
    return dps;
}

// Known DPS → function code mapping (from device specs)
static std::string dps_to_function_code(const std::string& dps) {
    if (dps == "1") return "switch_led";
    if (dps == "2") return "bright_value";
    if (dps == "3") return "temp_value";
    if (dps == "4") return "colour_data";
    if (dps == "5") return "scene_data";
    if (dps == "6") return "work_mode";
    if (dps == "7") return "countdown";
    if (dps == "8") return "do_not_disturb";
    return dps;  // pass through unknown codes
}

bool CloudApi::send_commands(const std::string& device_id, const DpsMap& commands) {
    if (is_token_expired() && !refresh_token()) return false;

    std::string json = "{\"commands\":[";
    bool first = true;
    for (auto& [k, v] : commands) {
        if (!first) json += ",";
        first = false;
        std::string fcode = dps_to_function_code(k);
        json += "{\"code\":\"" + fcode + "\",\"value\":";
        if (v.type() == typeid(bool))
            json += std::any_cast<bool>(v) ? "true" : "false";
        else if (v.type() == typeid(int))
            json += std::to_string(std::any_cast<int>(v));
        else if (v.type() == typeid(double))
            json += std::to_string(std::any_cast<double>(v));
        else {
            try { json += "\"" + std::any_cast<std::string>(v) + "\""; }
            catch (...) { json += "\"null\""; }
        }
        json += "}";
    }
    json += "]}";

    auto path = "/v1.0/iot-03/devices/" + device_id + "/commands";
    auto resp = do_request("POST", path, json, true);

    auto success = resp.find("\"success\":true");
    return success != std::string::npos;
}

bool CloudApi::send_ir_command(const std::string& hub_device_id,
                                const std::string& remote_id,
                                int category_id,
                                int remote_index,
                                const std::string& key) {
    if (is_token_expired() && !refresh_token()) return false;

    auto keys_opt = list_ir_keys(hub_device_id, remote_id);
    bool is_standard = false;
    int key_id = 0;
    if (keys_opt) {
        for (auto& k : *keys_opt) {
            if (k.key == key) {
                is_standard = k.standard;
                key_id = k.key_id;
                break;
            }
        }
    }

    // Try the preferred endpoint, then fall back to the other
    std::string base = "/v2.0/infrareds/" + hub_device_id + "/remotes/" + remote_id;

    // First attempt: preferred endpoint
    std::string path1 = base + (is_standard ? "/command" : "/raw/command");
    std::string body1;
    if (is_standard) {
        body1 = "{\"categoryId\":" + std::to_string(category_id) +
                ",\"remoteIndex\":" + std::to_string(remote_index) +
                ",\"key\":\"" + key + "\"}";
    } else if (key_id != 0) {
        body1 = "{\"categoryId\":" + std::to_string(category_id) +
                ",\"remoteIndex\":" + std::to_string(remote_index) +
                ",\"key_id\":" + std::to_string(key_id) + "}";
    } else {
        return false;
    }

    auto resp1 = do_request("POST", path1, body1, true);
    if (resp1.find("\"success\":true") != std::string::npos) return true;

    // Fallback: try the other endpoint if we have the data for it
    if (is_standard && key_id != 0) {
        // Standard failed → try raw with key_id
        std::string body2 = "{\"categoryId\":" + std::to_string(category_id) +
                            ",\"remoteIndex\":" + std::to_string(remote_index) +
                            ",\"key_id\":" + std::to_string(key_id) + "}";
        auto resp2 = do_request("POST", base + "/raw/command", body2, true);
        return resp2.find("\"success\":true") != std::string::npos;
    }
    if (!is_standard) {
        // Raw failed → try standard with key name
        std::string body2 = "{\"categoryId\":" + std::to_string(category_id) +
                            ",\"remoteIndex\":" + std::to_string(remote_index) +
                            ",\"key\":\"" + key + "\"}";
        auto resp2 = do_request("POST", base + "/command", body2, true);
        return resp2.find("\"success\":true") != std::string::npos;
    }

    return false;
}

// ── Minimal JSON helpers (shared with config.cpp style) ──

static std::string json_trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    return s.substr(start);
}

// Unescape JSON string (handles \uXXXX, standard escapes)
static std::string json_unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char c = s[i + 1];
            switch (c) {
                case '"':  out += '"';  i++; break;
                case '\\': out += '\\'; i++; break;
                case '/':  out += '/';  i++; break;
                case 'b':  out += '\b'; i++; break;
                case 'f':  out += '\f'; i++; break;
                case 'n':  out += '\n'; i++; break;
                case 'r':  out += '\r'; i++; break;
                case 't':  out += '\t'; i++; break;
                case 'u':
                    if (i + 5 < s.size()) {
                        std::string hex = s.substr(i + 2, 4);
                        char32_t cp = std::strtoul(hex.c_str(), nullptr, 16);
                        out += (char)cp;  // assumes ASCII-range codepoints
                        i += 5;
                    }
                    break;
                default: out += s[i]; break;
            }
        } else {
            out += s[i];
        }
    }
    return out;
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
    return json_unescape((end == std::string::npos) ? json.substr(pos) : json.substr(pos, end - pos));
}

static std::string json_int(const std::string& json, const std::string& key) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + key.size() + 2);
    if (pos == std::string::npos) return "";
    pos++;
    while (pos < json.size() && json[pos] == ' ') pos++;
    auto end = json.find_first_of(",}", pos);
    return json.substr(pos, end - pos);
}

std::optional<std::vector<IrKeyInfo>>
CloudApi::list_ir_keys(const std::string& hub_device_id,
                        const std::string& remote_id) {
    if (is_token_expired() && !refresh_token()) return std::nullopt;

    auto path = "/v2.0/infrareds/" + hub_device_id + "/remotes/" + remote_id + "/keys";
    auto resp = do_request("GET", path, "", true);

    std::vector<IrKeyInfo> keys;
    auto res_start = resp.find("\"result\":{");
    if (res_start == std::string::npos) {
        res_start = resp.find("\"result\": {");
        if (res_start == std::string::npos) return keys;
    }
    auto kl_start = resp.find("\"key_list\":[", res_start);
    if (kl_start == std::string::npos) {
        kl_start = resp.find("\"key_list\": [", res_start);
        if (kl_start == std::string::npos) return keys;
    }
    size_t pos = resp.find('[', kl_start);
    if (pos == std::string::npos) return keys;
    pos++;

    while (pos < resp.size()) {
        while (pos < resp.size() && (resp[pos]==' '||resp[pos]=='\n'||resp[pos]=='\r'||resp[pos]=='\t'))
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
            IrKeyInfo k;
            k.key = json_str(obj, "key");
            if (k.key.empty()) k.key = json_str(obj, "key_name");
            auto kid_str = json_int(obj, "key_id");
            if (!kid_str.empty()) k.key_id = std::stoi(kid_str);
            auto std_str = json_int(obj, "standard_key");
            k.standard = (std_str == "1" || std_str == "true");
            if (!k.key.empty())
                keys.push_back(k);
        } else {
            pos++;
        }
    }
    return keys;
}

static std::vector<DeviceInfo> parse_device_list(const std::string& resp) {
    std::vector<DeviceInfo> devices;

    // Find "result": [
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
            DeviceInfo di;
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

std::optional<std::vector<DeviceInfo>> CloudApi::list_devices() {
    if (is_token_expired() && !refresh_token()) return std::nullopt;

    std::string path;
    if (!uid_.empty())
        path = "/v1.0/users/" + uid_ + "/devices";
    else
        path = "/v1.0/devices";

    auto resp = do_request("GET", path, "", true);
    auto devices = parse_device_list(resp);
    if (devices.empty()) return std::nullopt;
    return devices;
}

// ── Parse status codes from device info response ──
static std::vector<std::string> parse_status_codes(const std::string& resp) {
    std::vector<std::string> codes;
    auto arr_start = resp.find("\"status\":[");
    if (arr_start == std::string::npos) return codes;
    size_t pos = resp.find('[', arr_start) + 1;

    while (pos < resp.size()) {
        auto cpos = resp.find("\"code\":\"", pos);
        if (cpos == std::string::npos) break;
        cpos += 8;
        auto cend = resp.find('"', cpos);
        if (cend == std::string::npos) break;
        codes.push_back(resp.substr(cpos, cend - cpos));
        pos = cend + 1;
    }
    return codes;
}

std::optional<DeviceInfo> CloudApi::fetch_device_details(const std::string& device_id) {
    if (is_token_expired() && !refresh_token()) return std::nullopt;

    auto path = "/v1.0/devices/" + device_id;
    auto resp = do_request("GET", path, "", true);

    // Find the "result" object
    auto res_start = resp.find("\"result\":{");
    if (res_start == std::string::npos) return std::nullopt;

    int depth = 1;
    size_t pos = res_start + 9;
    while (pos < resp.size() && depth > 0) {
        if (resp[pos] == '{') depth++;
        else if (resp[pos] == '}') depth--;
        pos++;
    }
    std::string obj = resp.substr(res_start + 9, pos - res_start - 9 - 1);

    DeviceInfo di;
    di.id           = json_str(obj, "id");
    di.name         = json_str(obj, "name");
    di.local_key    = json_str(obj, "local_key");
    di.ip           = json_str(obj, "ip");
    di.version      = json_str(obj, "version");
    di.product_id   = json_str(obj, "product_id");
    di.product_name = json_str(obj, "product_name");
    di.mac          = json_str(obj, "mac");
    di.status_codes = parse_status_codes(obj);

    // Parse "online" field
    auto ol = obj.find("\"online\":");
    if (ol != std::string::npos) {
        ol += 9;
        di.online = (obj.substr(ol, 4) == "true");
    }

    return di;
}

// ── Parse specification functions into CommandInfo ──
void CloudApi::update_cached_codes(const std::string& device_id,
                                    const std::vector<std::string>& codes) {
    std::lock_guard<std::mutex> lock(cache_mtx_);
    status_code_cache_[device_id] = codes;
}

std::vector<std::string> CloudApi::get_cached_codes(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(cache_mtx_);
    auto it = status_code_cache_.find(device_id);
    if (it != status_code_cache_.end()) return it->second;
    return {};
}

std::vector<CommandInfo> CloudApi::get_cached_commands(const std::string& device_id) {
    std::lock_guard<std::mutex> lock(cache_mtx_);
    auto it = command_cache_.find(device_id);
    if (it != command_cache_.end()) return it->second;
    return {};
}

bool CloudApi::fetch_device_specs(const std::string& device_id) {
    auto info = fetch_device_details(device_id);
    if (!info) return false;

    // Infer types from status values in the device detail response
    auto path = "/v1.0/devices/" + device_id;
    auto resp = do_request("GET", path, "", true);
    auto res_start = resp.find("\"result\":{");
    if (res_start == std::string::npos) return false;
    int depth = 1;
    size_t pos2 = res_start + 9;
    while (pos2 < resp.size() && depth > 0) {
        if (resp[pos2] == '{') depth++;
        else if (resp[pos2] == '}') depth--;
        pos2++;
    }
    std::string obj = resp.substr(res_start + 9, pos2 - res_start - 9 - 1);

    // Parse status array, inferring types from values
    std::vector<CommandInfo> cmds;
    auto arr_start = obj.find("\"status\":[");
    if (arr_start == std::string::npos) return false;
    size_t p = obj.find('[', arr_start) + 1;

    while (p < obj.size()) {
        while (p < obj.size() && (obj[p]==' '||obj[p]=='\n'||obj[p]=='\r'||obj[p]=='\t')) p++;
        if (p >= obj.size() || obj[p] == ']') break;
        if (obj[p] == '{') {
            depth = 1;
            size_t ost = p++;
            while (p < obj.size() && depth > 0) {
                if (obj[p] == '{') depth++;
                else if (obj[p] == '}') depth--;
                p++;
            }
            std::string o = obj.substr(ost, p - ost);
            CommandInfo ci;
            ci.name = json_str(o, "code");

            // Infer type from value
            auto vp = o.find("\"value\":");
            if (vp != std::string::npos) {
                vp += 8;
                while (vp < o.size() && o[vp] == ' ') vp++;
                if (vp < o.size()) {
                    if (o.substr(vp, 4) == "true" || o.substr(vp, 5) == "false") {
                        ci.type = "Boolean";
                        ci.values = "true/false";
                    } else if (o[vp] == '"') {
                        ci.type = "Enum";
                    } else if (o[vp] == '{' || o[vp] == '[') {
                        ci.type = "String";
                    } else {
                        ci.type = "Integer";
                    }
                }
            }
            // Add value hints for known codes
            if (ci.name == "bright_value" || ci.name == "temp_value")
                ci.values = "0-1000";
            else if (ci.name == "countdown")
                ci.values = "0-86400";

            if (!ci.name.empty())
                cmds.push_back(ci);
        } else p++;
    }

    if (!cmds.empty()) {
        std::lock_guard<std::mutex> lock(cache_mtx_);
        command_cache_[device_id] = cmds;
    }
    return !cmds.empty();
}

} // namespace tuya
