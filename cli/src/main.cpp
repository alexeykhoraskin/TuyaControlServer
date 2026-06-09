// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#include "server_api.h"
#include <iostream>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

static std::string find_device_id(ServerApi& api, const std::string& name) {
    auto devs = api.list_devices();
    if (devs.empty()) {
        std::cerr << "Server unreachable or no devices found." << std::endl;
        return "__error__";
    }
    for (auto& d : devs)
        if (d.name == name) return d.id;
    return "";
}

static bool is_ir_device(ServerApi& api, const std::string& name) {
    auto devs = api.list_devices();
    for (auto& d : devs)
        if (d.name == name) return d.ir;
    return false;
}

static void print_devices(const std::vector<DeviceInfo>& devs) {
    if (devs.empty()) {
        std::cout << "No devices found." << std::endl;
        return;
    }
    printf("%-26s %-18s %-14s %-8s  %s  %s\n", "ID", "IP", "Name", "Mode", "Type", "Online");
    printf("%s\n", std::string(90, '-').c_str());
    for (auto& d : devs) {
        printf("%-26s %-18s %-14s %-8s  %-4s  %s\n",
               d.id.substr(0, 25).c_str(),
               d.ip.substr(0, 17).c_str(),
               d.name.substr(0, 13).c_str(),
               d.mode.c_str(),
               d.ir ? "IR" : "DPS",
               d.online ? "yes" : "no");
    }
}

static void print_keys(const std::vector<std::pair<std::string, std::string>>& keys) {
    if (keys.empty()) {
        std::cout << "No keys found." << std::endl;
        return;
    }
    printf("%-30s  %s\n", "Key", "Key ID");
    printf("%s\n", std::string(50, '-').c_str());
    for (auto& [k, v] : keys)
        printf("%-30s  %s\n", k.c_str(), v.c_str());
}

static void print_status(const DeviceStatus& s) {
    printf("Device: %s (%s)\n", s.name.c_str(), s.id.c_str());
    printf("IP:     %s\n", s.ip.c_str());
    printf("Mode:   %s\n", s.mode.c_str());
    printf("Online: %s\n", s.online ? "yes" : "no");
    if (!s.dps.empty()) {
        printf("\nStatus:\n");
        for (auto& [k, v] : s.dps)
            printf("  %s = %s\n", k.c_str(), v.c_str());
    }
}

static void print_commands(const std::vector<CommandInfo>& cmds) {
    if (cmds.empty()) {
        std::cout << "No commands available." << std::endl;
        return;
    }
    printf("%-28s %-12s  %s\n", "Command", "Type", "Values");
    printf("%s\n", std::string(70, '-').c_str());
    for (auto& c : cmds) {
        std::string vals = c.values;
        if (vals.empty() && !c.extra.empty()) vals = "id:" + c.extra;
        printf("%-28s %-12s  %s\n", c.name.c_str(), c.type.c_str(), vals.c_str());
    }
}

static void print_help() {
    std::cout << "tuya_cli \u2014 TuyaControlServer CLI client" << std::endl;
    std::cout << std::endl;
    std::cout << "Usage: tuya_cli [options] <command> [args]" << std::endl;
    std::cout << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -s, --server URL  Server URL (default: http://localhost:8080)" << std::endl;
    std::cout << "  -n, --repeat N    Repeat command N times (default: 1)" << std::endl;
    std::cout << "  -d, --delay MS    Delay between repeats in ms (default: 500)" << std::endl;
    std::cout << "  -h, --help        Show this help" << std::endl;
    std::cout << std::endl;
    std::cout << "Commands:" << std::endl;
    std::cout << "  devices, list                    List all devices" << std::endl;
    std::cout << "  keys <name>                      List available commands for a device" << std::endl;
    std::cout << std::endl;
    std::cout << "Device commands (auto-detect IR vs DPS):" << std::endl;
    std::cout << "  <device_name>                    Show device status" << std::endl;
    std::cout << "  <device_name> <key>              Send IR key to device" << std::endl;
    std::cout << "  <device_name> <dps> <value>      Send DPS command" << std::endl;
    std::cout << std::endl;
    std::cout << "Examples:" << std::endl;
    std::cout << "  tuya_cli TH1" << std::endl;
    std::cout << "  tuya_cli TV Power" << std::endl;
    std::cout << "  tuya_cli Light PowerOn" << std::endl;
    std::cout << "  tuya_cli LightNew switch_led true" << std::endl;
    std::cout << "  tuya_cli -n 5 DVD Volume+          # Volume+ 5 times" << std::endl;
    std::cout << "  tuya_cli -n 3 -d 200 TV Channel-   # Channel- 3 times, 200ms apart" << std::endl;
}

int main(int argc, char** argv) {
    std::string server = "http://localhost:8080";
    int repeat = 1;
    int delay_ms = 500;
    std::vector<std::string> args;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            print_help();
            return 0;
        }
        if ((!strcmp(argv[i], "-s") || !strcmp(argv[i], "--server")) && i + 1 < argc)
            server = argv[++i];
        else if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "--repeat")) && i + 1 < argc)
            repeat = std::max(1, std::stoi(argv[++i]));
        else if ((!strcmp(argv[i], "-d") || !strcmp(argv[i], "--delay")) && i + 1 < argc)
            delay_ms = std::max(0, std::stoi(argv[++i]));
        else if (argv[i][0] != '-')
            args.push_back(argv[i]);
    }

    ServerApi api(server);

    if (args.empty() || args[0] == "devices" || args[0] == "list") {
        print_devices(api.list_devices());
        return 0;
    }

    if (args[0] == "keys") {
        if (args.size() < 2) {
            std::cerr << "Usage: tuya_cli keys <device_name>" << std::endl;
            return 1;
        }
        auto id = find_device_id(api, args[1]);
        if (id.empty()) {
            std::cerr << "Device not found: " << args[1] << std::endl;
            return 1;
        }
        if (id == "__error__") return 1;
        print_commands(api.list_commands(id));
        return 0;
    }

    auto id = find_device_id(api, args[0]);
    if (id == "__error__") return 1;
    if (id.empty()) {
        std::cerr << "Unknown command or device: " << args[0] << std::endl;
        print_help();
        return 1;
    }

    if (args.size() < 2) {
        print_status(api.get_device_status(id));
        return 0;
    }

    if (is_ir_device(api, args[0])) {
        bool all_ok = true;
        for (int i = 0; i < repeat; i++) {
            bool ok = api.send_command(id, "{\"ir_key\":\"" + args[1] + "\"}");
            if (ok)
                std::cout << "OK" << std::endl;
            else {
                std::cerr << "Failed" << std::endl;
                all_ok = false;
            }
            if (i + 1 < repeat && delay_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        return all_ok ? 0 : 1;
    }

    if (args.size() >= 3) {
        bool all_ok = true;
        for (int i = 0; i < repeat; i++) {
            bool ok = api.send_command(id, "{\"dps\":{\"" + args[1] + "\":" + args[2] + "}}");
            if (ok)
                std::cout << "OK" << std::endl;
            else {
                std::cerr << "Failed" << std::endl;
                all_ok = false;
            }
            if (i + 1 < repeat && delay_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        }
        return all_ok ? 0 : 1;
    }

    std::cerr << "DPS devices require: " << args[0] << " <dps> <value>" << std::endl;
    return 1;
}
