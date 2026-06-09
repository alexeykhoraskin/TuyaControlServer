// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#include "server_api.h"
#include <curl/curl.h>
#include <cstring>
#include <map>

static size_t wcb(void* d, size_t s, size_t n, std::string* o) {
    o->append((char*)d, s*n); return s*n;
}

static std::string json_str(const std::string& json, const std::string& key) {
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
    auto end = json.find_first_of(",}", pos);
    return json.substr(pos, end - pos);
}

ServerApi::ServerApi(std::string base_url) : base_(std::move(base_url)) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

ServerApi::~ServerApi() {
    curl_global_cleanup();
}

std::string ServerApi::get(const std::string& path) {
    CURL* c = curl_easy_init();
    if (!c) return "";
    std::string r;
    curl_easy_setopt(c, CURLOPT_URL, (base_ + path).c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
    curl_easy_perform(c);
    curl_easy_cleanup(c);
    return r;
}

std::vector<DeviceInfo> ServerApi::list_devices() {
    std::vector<DeviceInfo> out;
    auto resp = get("/devices");
    if (resp.empty()) return out;

    auto arr_start = resp.find("\"devices\":[");
    if (arr_start == std::string::npos) return out;
    size_t pos = resp.find('[', arr_start) + 1;

    while (pos < resp.size()) {
        while (pos < resp.size() && (resp[pos]==' '||resp[pos]=='\n')) pos++;
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
            DeviceInfo d;
            d.id     = json_str(obj, "id");
            d.ip     = json_str(obj, "ip");
            d.name   = json_str(obj, "name");
            d.mode   = json_str(obj, "mode");
            d.online = json_str(obj, "online") == "true";
            d.ir     = json_str(obj, "ir") == "true";
            if (!d.id.empty()) out.push_back(d);
        } else pos++;
    }
    return out;
}

std::string ServerApi::post(const std::string& path, const std::string& body) {
    CURL* c = curl_easy_init();
    if (!c) return "";
    std::string r;
    curl_easy_setopt(c, CURLOPT_URL, (base_ + path).c_str());
    curl_easy_setopt(c, CURLOPT_POST, 1L);
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, wcb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &r);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 5L);
    struct curl_slist* h = nullptr;
    h = curl_slist_append(h, "Content-Type: application/json");
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    curl_easy_perform(c);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return r;
}

bool ServerApi::send_command(const std::string& device_id, const std::string& json_body) {
    auto resp = post("/devices/" + device_id + "/command", json_body);
    return resp.find("\"success\":true") != std::string::npos;
}

std::vector<std::pair<std::string, std::string>>
ServerApi::list_ir_keys(const std::string& device_id) {
    std::vector<std::pair<std::string, std::string>> out;
    auto resp = get("/devices/" + device_id + "/ir-keys");
    if (resp.empty()) return out;

    auto arr_start = resp.find("\"keys\":[");
    if (arr_start == std::string::npos) return out;
    size_t pos = resp.find('[', arr_start) + 1;

    while (pos < resp.size()) {
        while (pos < resp.size() && (resp[pos]==' '||resp[pos]=='\n')) pos++;
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
            auto name = json_str(obj, "key_name");
            auto key_id = json_str(obj, "key_id");
            if (!name.empty())
                out.push_back({name, key_id});
        } else pos++;
    }
    return out;
}

std::vector<CommandInfo> ServerApi::list_commands(const std::string& device_id) {
    std::vector<CommandInfo> out;
    auto resp = get("/devices/" + device_id + "/commands");
    if (resp.empty()) return out;

    auto arr_start = resp.find("\"commands\":[");
    if (arr_start == std::string::npos) return out;
    size_t pos = resp.find('[', arr_start) + 1;

    while (pos < resp.size()) {
        while (pos < resp.size() && (resp[pos]==' '||resp[pos]=='\n')) pos++;
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
            CommandInfo ci;
            ci.name   = json_str(obj, "name");
            ci.type   = json_str(obj, "type");
            ci.values = json_str(obj, "values");
            ci.extra  = json_str(obj, "extra");
            if (!ci.name.empty())
                out.push_back(ci);
        } else pos++;
    }
    return out;
}

DeviceStatus ServerApi::get_device_status(const std::string& device_id) {
    DeviceStatus s;
    auto resp = get("/devices/" + device_id);
    if (resp.empty()) return s;

    s.id   = json_str(resp, "id");
    s.name = json_str(resp, "name");
    s.ip   = json_str(resp, "ip");
    s.mode = json_str(resp, "mode");
    s.online = json_str(resp, "online") == "true";

    auto dps_start = resp.find("\"dps\":{");
    if (dps_start == std::string::npos) return s;
    size_t p = resp.find('{', dps_start) + 1;
    int depth = 1;
    size_t i = p;
    while (i < resp.size() && depth > 0) {
        if (resp[i] == '{') depth++;
        else if (resp[i] == '}') depth--;
        i++;
    }
    std::string dpso = resp.substr(p, i - p - 1);
    p = 0;
    while (p < dpso.size()) {
        auto ks = dpso.find('"', p);
        if (ks == std::string::npos) break;
        auto ke = dpso.find('"', ks + 1);
        if (ke == std::string::npos) break;
        std::string k = dpso.substr(ks + 1, ke - ks - 1);
        auto vs = dpso.find(':', ke + 1);
        if (vs == std::string::npos) break;
        vs++;
        while (vs < dpso.size() && dpso[vs] == ' ') vs++;
        if (dpso[vs] == '"') {
            vs++;
            auto ve = dpso.find('"', vs);
            s.dps[k] = (ve == std::string::npos) ? dpso.substr(vs) : dpso.substr(vs, ve - vs);
            p = (ve == std::string::npos) ? dpso.size() : ve + 1;
        } else if (dpso.substr(vs, 4) == "true") {
            s.dps[k] = "true"; p = vs + 4;
        } else if (dpso.substr(vs, 5) == "false") {
            s.dps[k] = "false"; p = vs + 5;
        } else {
            auto ve = dpso.find_first_of(",}", vs);
            s.dps[k] = dpso.substr(vs, ve - vs);
            p = (ve == std::string::npos) ? dpso.size() : ve;
        }
        if (p < dpso.size() && dpso[p] == ',') p++;
    }
    return s;
}
