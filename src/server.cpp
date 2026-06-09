// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#include "server.h"
#include <cstring>
#include <sstream>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>

namespace tuya {

static const char* RESP_OK =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Connection: close\r\n"
    "\r\n";

static const char* RESP_NOTFOUND =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: application/json\r\n"
    "Connection: close\r\n"
    "\r\n"
    "{\"error\":\"not found\"}";

ControlServer::ControlServer(const ServerConfig& cfg, std::shared_ptr<DeviceManager> mgr)
    : cfg_(cfg), mgr_(mgr) {}

ControlServer::~ControlServer() { stop(); }

void ControlServer::run() {
    running_ = true;
    server_thread_ = std::thread(&ControlServer::server_loop, this);
}

void ControlServer::stop() {
    running_ = false;
    if (server_thread_.joinable()) server_thread_.join();
}

void ControlServer::server_loop() {
    int sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(cfg_.port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(sock);
        return;
    }
    ::listen(sock, 16);

    struct pollfd pfd{};
    pfd.fd = sock;
    pfd.events = POLLIN;

    while (running_) {
        int ret = poll(&pfd, 1, 1000);
        if (ret < 0) break;
        if (ret == 0) continue;

        int client = ::accept(sock, nullptr, nullptr);
        if (client < 0) continue;

        // Read request
        char buf[4096];
        auto n = ::read(client, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            std::string req(buf);
            std::string resp;

            // Route — order matters: more specific paths first
            if (req.find("GET /devices ") != std::string::npos ||
                req == "GET /devices HTTP/1.1\r\n") {
                resp = RESP_OK;
                auto devices = mgr_->list_devices();
                std::string json = "{\"devices\":[";
                bool first = true;
                for (auto& d : devices) {
                    if (!first) json += ",";
                    first = false;
                    json += "{\"id\":\"" + d->info.id + "\","
                            "\"ip\":\"" + d->info.ip + "\","
                            "\"name\":\"" + d->info.name + "\","
                            "\"mode\":\"" + (d->mode == ConnMode::LOCAL_V34 ? "v3.4" :
                                             d->mode == ConnMode::LOCAL_V2 ? "v2" :
                                             d->mode == ConnMode::CLOUD ? "cloud" : "none") + "\","
                            "\"ir\":" + (d->info.is_infrared() ? "true" : "false") + ","
                            "\"online\":" + (d->info.online ? "true" : "false") + ","
                            "\"error\":\"" + d->error + "\"}";
                }
                json += "]}";
                resp += json;
            }
            else if (req.find("GET /devices/") != std::string::npos &&
                     req.find("/commands") != std::string::npos) {
                // GET /devices/{id}/commands
                auto start = req.find("GET /devices/") + 13;
                auto slash = req.find("/commands", start);
                auto id = req.substr(start, slash - start);

                auto cmds = mgr_->list_commands(id);
                if (!cmds.empty()) {
                    std::string json = RESP_OK;
                    json += "{\"commands\":[";
                    bool first = true;
                    for (auto& c : cmds) {
                        if (!first) json += ",";
                        first = false;
                        json += "{\"name\":\"" + c.name + "\"";
                        if (!c.type.empty())
                            json += ",\"type\":\"" + c.type + "\"";
                        if (!c.values.empty())
                            json += ",\"values\":\"" + c.values + "\"";
                        if (!c.extra.empty())
                            json += ",\"extra\":\"" + c.extra + "\"";
                        json += "}";
                    }
                    json += "]}";
                    resp = json;
                } else {
                    resp = RESP_NOTFOUND;
                }
            }
            else if (req.find("GET /devices/") != std::string::npos &&
                     req.find("/ir-keys") != std::string::npos) {
                // GET /devices/{id}/ir-keys
                auto start = req.find("GET /devices/") + 13;
                auto slash = req.find("/ir-keys", start);
                auto id = req.substr(start, slash - start);

                auto keys = mgr_->list_ir_keys(id);
                if (keys) {
                    std::string json = RESP_OK;
                    json += "{\"keys\":[";
                    bool first = true;
                    for (auto& ki : *keys) {
                        if (!first) json += ",";
                        first = false;
                        json += "{\"key_name\":\"" + ki.key + "\",\"key_id\":\"" + std::to_string(ki.key_id) +
                                "\",\"standard\":" + (ki.standard ? "true" : "false") + "}";
                    }
                    json += "]}";
                    resp = json;
                } else {
                    resp = RESP_NOTFOUND;
                }
            }
            else if (req.find("GET /devices/") != std::string::npos) {
                // Extract device ID from path
                auto start = req.find("GET /devices/") + 13;
                auto end = req.find(' ', start);
                auto id = req.substr(start, end - start);

                auto dev = mgr_->get_device(id);
                if (dev) {
                    // Query status if cache is empty (e.g. sensors)
                    if (dev->dps.empty())
                        mgr_->query_status(id);

                    std::string json = RESP_OK;
                    json += "{\"id\":\"" + dev->info.id + "\","
                            "\"ip\":\"" + dev->info.ip + "\","
                            "\"name\":\"" + dev->info.name + "\","
                            "\"version\":\"" + dev->info.version + "\","
                            "\"mode\":\"" + (dev->mode == ConnMode::LOCAL_V34 ? "v3.4" :
                                             dev->mode == ConnMode::LOCAL_V2 ? "v2" :
                                             dev->mode == ConnMode::CLOUD ? "cloud" : "none") + "\","
                            "\"online\":" + (dev->info.online ? "true" : "false") + ","
                            "\"dps\":{";

                    bool first = true;
                    for (auto& [k, v] : dev->dps) {
                        if (!first) json += ",";
                        first = false;
                        json += "\"" + k + "\":";
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
                    }
                    json += "}}";
                    resp = json;
                } else {
                    resp = RESP_NOTFOUND;
                }
            }
            else if (req.find("POST /devices/") != std::string::npos &&
                     req.find("/command") != std::string::npos) {
                // POST /devices/{id}/command
                auto start = req.find("POST /devices/") + 14;
                auto slash = req.find("/command", start);
                auto id = req.substr(start, slash - start);

                // Find body
                auto body_start = req.find("\r\n\r\n");
                if (body_start == std::string::npos) {
                    resp = RESP_NOTFOUND;
                } else {
                    body_start += 4;
                    std::string body = req.substr(body_start);
                    // Parse simple JSON command: {"dps":{"1":true}} or {"ir_key":"PowerOn"}
                    DpsMap cmds;

                    // Check for top-level ir_key
                    auto irk = body.find("\"ir_key\":\"");
                    if (irk != std::string::npos) {
                        irk += 10;
                        auto ire = body.find('"', irk);
                        if (ire != std::string::npos)
                            cmds["ir_key"] = body.substr(irk, ire - irk);
                    }

                    auto dps_start = body.find("\"dps\":{");
                    if (dps_start != std::string::npos) {
                        dps_start += 7;
                        int depth = 1;
                        size_t i = dps_start;
                        while (i < body.size() && depth > 0) {
                            if (body[i] == '{') depth++;
                            else if (body[i] == '}') depth--;
                            i++;
                        }
                        std::string dps_json = body.substr(dps_start, i - dps_start - 1);
                        size_t pos = 0;
                        while (pos < dps_json.size()) {
                            auto kstart = dps_json.find('"', pos);
                            if (kstart == std::string::npos) break;
                            auto kend = dps_json.find('"', kstart + 1);
                            if (kend == std::string::npos) break;
                            std::string key = dps_json.substr(kstart + 1, kend - kstart - 1);
                            auto vstart = dps_json.find(':', kend + 1);
                            if (vstart == std::string::npos) break;
                            vstart++;
                            while (vstart < dps_json.size() && dps_json[vstart] == ' ') vstart++;
                            if (dps_json[vstart] == '"') {
                                size_t vend = vstart + 1;
                                while (vend < dps_json.size()) {
                                    if (dps_json[vend] == '\\') vend += 2;
                                    else if (dps_json[vend] == '"') break;
                                    else vend++;
                                }
                                cmds[key] = dps_json.substr(vstart + 1, vend - vstart - 1);
                                pos = vend + 1;
                            } else if (dps_json.substr(vstart, 4) == "true") {
                                cmds[key] = true;
                                pos = vstart + 4;
                            } else if (dps_json.substr(vstart, 5) == "false") {
                                cmds[key] = false;
                                pos = vstart + 5;
                            } else {
                                auto vend = dps_json.find_first_of(",}", vstart);
                                if (vend == std::string::npos) vend = dps_json.size();
                                try { cmds[key] = std::stoi(dps_json.substr(vstart, vend - vstart)); }
                                catch (...) { cmds[key] = dps_json.substr(vstart, vend - vstart); }
                                pos = vend;
                            }
                            if (pos < dps_json.size() && dps_json[pos] == ',') pos++;
                        }
                    }

                    bool ok = mgr_->send_command(id, cmds);
                    if (ok) {
                        resp = std::string(RESP_OK) + "{\"success\":true}";
                        // Also refresh status
                        mgr_->query_status(id);
                    } else {
                        resp = std::string(RESP_NOTFOUND);
                    }
                }
            }
            else if (req.find("GET /api/status") != std::string::npos) {
                resp = std::string(RESP_OK) + "{\"status\":\"running\"}";
            }
            else {
                resp = RESP_NOTFOUND;
            }

            ::write(client, resp.data(), resp.size());
        }
        ::close(client);
    }

    ::close(sock);
}

} // namespace tuya
