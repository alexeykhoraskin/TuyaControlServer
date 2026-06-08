// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#include "device_manager.h"
#include "device_discovery.h"
#include <algorithm>
#include <iostream>
#include <thread>
#include <arpa/inet.h>

// Keep config (private) IP over cloud (public) IP for LAN access
static bool is_private_ip(const std::string& ip) {
    if (ip.empty() || ip == "0.0.0.0") return false;
    struct in_addr addr;
    if (inet_pton(AF_INET, ip.c_str(), &addr) != 1) return false;
    unsigned long h = ntohl(addr.s_addr);
    // 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 127.0.0.0/8
    return (h & 0xFF000000) == 0x0A000000 ||
           (h & 0xFFF00000) == 0xAC100000 ||
           (h & 0xFFFF0000) == 0xC0A80000 ||
           (h & 0xFF000000) == 0x7F000000;
}

namespace tuya {

DeviceManager::DeviceManager() = default;
DeviceManager::~DeviceManager() { stop_all(); }

void DeviceManager::set_cloud(std::shared_ptr<CloudApi> cloud) {
    cloud_api_ = cloud;
}

void DeviceManager::load_devices(const std::vector<DeviceInfo>& devices) {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& d : devices) {
        auto dev = std::make_shared<ManagedDevice>();
        dev->state = std::make_shared<DeviceState>();
        dev->state->info = d;
        dev->state->cloud_only = !d.has_ip();
        devices_[d.id] = dev;
    }
}

void DeviceManager::connect_all() {
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& [id, dev] : devices_) {
        if (dev->state->cloud_only) {
            dev->state->mode = ConnMode::CLOUD;
            continue;
        }
        try {
            dev->local = std::make_shared<LocalDevice>(*dev->state);
            if (!dev->local->connect()) {
                dev->state->error = "local connect failed";
                dev->state->mode = ConnMode::NONE;
            }
        } catch (std::exception& e) {
            dev->state->error = std::string("local init: ") + e.what();
            dev->state->mode = ConnMode::NONE;
        }
    }
}

std::shared_ptr<DeviceState> DeviceManager::get_device(const std::string& id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = devices_.find(id);
    return it != devices_.end() ? it->second->state : nullptr;
}

std::vector<std::shared_ptr<DeviceState>> DeviceManager::list_devices() {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<std::shared_ptr<DeviceState>> out;
    for (auto& [id, dev] : devices_)
        out.push_back(dev->state);
    return out;
}

std::optional<DpsMap> DeviceManager::query_status(const std::string& id) {
    auto it = devices_.find(id);
    if (it == devices_.end()) return std::nullopt;
    auto& dev = it->second;
    if (!dev->state->cloud_only && dev->local && dev->local->is_connected()) {
        auto dps = dev->local->query_status();
        if (dps) return dps;
        // Local failed — fall through to cloud
    }
    if (cloud_api_ && cloud_api_->valid())
        return cloud_api_->query_status(id);
    return std::nullopt;
}

std::optional<std::vector<std::pair<std::string, std::string>>>
DeviceManager::list_ir_keys(const std::string& id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = devices_.find(id);
    if (it == devices_.end()) return std::nullopt;
    auto& dev = it->second;
    if (!dev->state->info.is_infrared() || !cloud_api_ || !cloud_api_->valid())
        return std::nullopt;
    return cloud_api_->list_ir_keys(dev->state->info.ir_hub_id, dev->state->info.id);
}

std::vector<CommandInfo> DeviceManager::list_commands(const std::string& id) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = devices_.find(id);
    if (it == devices_.end()) return {};
    auto& dev = it->second;
    if (!cloud_api_) return {};
    if (dev->state->info.is_infrared()) {
        auto keys = cloud_api_->list_ir_keys(dev->state->info.ir_hub_id, dev->state->info.id);
        if (!keys) return {};
        std::vector<CommandInfo> cmds;
        for (auto& [k, v] : *keys)
            cmds.push_back({k, "ir_key", "", v});
        return cmds;
    }
    auto cmds = cloud_api_->get_cached_commands(id);
    if (cmds.empty()) {
        cloud_api_->fetch_device_specs(id);
        cmds = cloud_api_->get_cached_commands(id);
    }
    return cmds;
}

bool DeviceManager::send_command(const std::string& id, const DpsMap& dps) {
    auto it = devices_.find(id);
    if (it == devices_.end()) return false;
    auto& dev = it->second;

    // Infrared device → use IR Control Hub API
    if (dev->state->info.is_infrared() && cloud_api_ && cloud_api_->valid()) {
        for (auto& [k, v] : dps) {
            if (v.type() == typeid(std::string)) {
                std::string key = std::any_cast<std::string>(v);
                return cloud_api_->send_ir_command(
                    dev->state->info.ir_hub_id,
                    dev->state->info.id,
                    dev->state->info.ir_category_id,
                    dev->state->info.ir_remote_index,
                    key);
            }
        }
        return false;
    }

    // Try local first (only if not cloud_only)
    if (!dev->state->cloud_only && dev->local && dev->local->is_connected()) {
        if (dev->local->send_commands(dps)) return true;
    }

    // Fallback to cloud API
    if (cloud_api_ && cloud_api_->valid())
        return cloud_api_->send_commands(id, dps);
    return false;
}

void DeviceManager::start_polling(int interval_s) {
    running_ = true;
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& [id, dev] : devices_) {
        if (dev->local && dev->local->is_connected()) {
            dev->poll_thread = std::thread(&DeviceManager::do_poll, this, dev, interval_s);
        }
    }
}

void DeviceManager::do_poll(std::shared_ptr<ManagedDevice> dev, int interval_s) {
    while (running_) {
        auto dps = dev->local->query_status();
        if (dps) {
            std::lock_guard<std::mutex> lock(dev->state->mtx);
            dev->state->dps = *dps;
            dev->state->last_seen = std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(std::chrono::seconds(interval_s));
    }
}

void DeviceManager::refresh_from_cloud() {
    if (!cloud_api_) return;

    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& [id, dev] : devices_) {
        auto info = cloud_api_->fetch_device_details(id);
        if (!info) continue;

        // Update stored info (only overwrite local_key if cloud returned one)
        if (!info->local_key.empty())
            dev->state->info.local_key = info->local_key;
        // Only overwrite IP from cloud if current IP is NOT a private/local address
        if (info->has_ip() && !is_private_ip(dev->state->info.ip))
            dev->state->info.ip = info->ip;
        if (!info->version.empty())
            dev->state->info.version = info->version;

        // Update cloud API cache with status codes
        if (!info->status_codes.empty())
            cloud_api_->update_cached_codes(id, info->status_codes);
    }
}

void DeviceManager::start_cloud_refresh(int interval_s) {
    running_ = true;
    std::thread([this, interval_s]() {
        while (running_) {
            refresh_from_cloud();
            for (int i = 0; i < interval_s && running_; i++)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }).detach();
}

void DeviceManager::stop_all() {
    running_ = false;
    std::lock_guard<std::mutex> lock(mtx_);
    for (auto& [id, dev] : devices_) {
        if (dev->local) dev->local->disconnect();
        if (dev->poll_thread.joinable()) dev->poll_thread.join();
    }
    devices_.clear();
}

void DeviceManager::start_discovery(int timeout_s, DiscoverCallback cb) {
    std::thread([this, timeout_s, cb]() {
        auto discovered = tuya_discover(timeout_s);
        for (auto& d : discovered) {
            std::lock_guard<std::mutex> lock(mtx_);
            if (devices_.find(d.id) == devices_.end()) {
                auto dev = std::make_shared<ManagedDevice>();
                dev->state = std::make_shared<DeviceState>();
                dev->state->info = d;
                devices_[d.id] = dev;
            }
            if (cb) cb(d);
        }
    }).detach();
}

} // namespace tuya
