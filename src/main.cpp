// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#include "server.h"
#include "device_manager.h"
#include "cloud_api.h"
#include "config.h"
#include <iostream>
#include <csignal>
#include <unistd.h>
#include <sys/stat.h>

std::unique_ptr<tuya::ControlServer> g_server;

static void on_signal(int) {
    if (g_server) g_server->stop();
    _exit(0);
}

static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

int main(int argc, char** argv) {
    // Precedence: CLI arg > config.local.json > config.json
    std::string config_path;
    if (argc > 1) {
        config_path = argv[1];
    } else if (file_exists("config.local.json")) {
        config_path = "config.local.json";
    } else {
        config_path = "config.json";
    }

    tuya::ServerConfig cfg;
    try {
        cfg = tuya::load_config(config_path);
    } catch (std::exception& e) {
        std::cerr << "Config error: " << e.what() << std::endl;
        // Use defaults
    }

    auto mgr = std::make_shared<tuya::DeviceManager>();

    // Cloud API
    if (cfg.cloud) {
        auto cloud = std::make_shared<tuya::CloudApi>(*cfg.cloud);
        mgr->set_cloud(cloud);
    }

    // Load configured devices
    if (!cfg.devices.empty()) {
        mgr->load_devices(cfg.devices);

        // Refresh device details from cloud (gets correct local_key, status codes)
        if (cfg.cloud) {
            mgr->refresh_from_cloud();
            // Periodic cloud refresh every hour
            mgr->start_cloud_refresh(3600);
        }

        mgr->connect_all();
        mgr->start_polling(cfg.poll_interval_s);
    }

    // Start discovery
    mgr->start_discovery(5, [](const tuya::DeviceInfo& di) {
        std::cout << "Discovered: " << di.id << " @ " << di.ip
                  << " v" << di.version << std::endl;
    });

    g_server = std::make_unique<tuya::ControlServer>(cfg, mgr);
    g_server->run();

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    std::cout << "TuyaControlServer listening on "
              << cfg.listen_addr << ":" << cfg.port << std::endl;

    // Just wait
    pause();

    return 0;
}
