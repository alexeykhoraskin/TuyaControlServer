// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#include "server.h"
#include "device_manager.h"
#include "cloud_api.h"
#include "config.h"
#include <iostream>
#include <csignal>
#include <cstring>
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

static void print_help(const char* prog) {
    std::cout << "TuyaControlServer v" << TUYA_CONTROL_SERVER_VERSION << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: " << prog << " [options] [config_path]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help    Show this help" << std::endl;
    std::cout << "  -v, --version Show version" << std::endl;
    std::cout << std::endl;
    std::cout << "Config lookup order:" << std::endl;
    std::cout << "  1. CLI argument (path to JSON file)" << std::endl;
    std::cout << "  2. config.local.json (gitignored, local overrides)" << std::endl;
    std::cout << "  3. config.json (default template, committed)" << std::endl;
    std::cout << std::endl;
    std::cout << "Endpoints:" << std::endl;
    std::cout << "  GET  /devices              List all devices" << std::endl;
    std::cout << "  GET  /devices/{id}         Device detail with DPS" << std::endl;
    std::cout << "  POST /devices/{id}/command Send command" << std::endl;
    std::cout << "  GET  /devices/{id}/keys    List IR keys" << std::endl;
    std::cout << "  GET  /devices/{id}/commands List available commands" << std::endl;
    std::cout << "  GET  /api/status           Health check" << std::endl;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_help(argv[0]);
            return 0;
        }
        if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--version")) {
            std::cout << TUYA_CONTROL_SERVER_VERSION << std::endl;
            return 0;
        }
    }

    // Precedence: CLI arg > config.local.json > config.json
    std::string config_path;
    if (argc > 1 && argv[1][0] != '-') {
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
