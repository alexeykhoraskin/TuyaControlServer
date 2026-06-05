// Copyright (c) 2026 Alexey.Khoraskin@gmail.com
// Licensed under the Apache License, Version 2.0
// https://github.com/alexeykhoraskin/TuyaControlServer
#include "device_discovery.h"
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>

namespace tuya {

static const char* DISCOVERY_MSG =
    "{\"gwId\":\"\",\"devId\":\"\",\"uid\":\"\",\"t\":\"1\"}";

std::vector<DeviceInfo> tuya_discover(int timeout_s) {
    std::vector<DeviceInfo> devices;

    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return devices;

    int broadcast = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(DISCOVERY_PORT);
    inet_pton(AF_INET, "255.255.255.255", &addr.sin_addr);

    // Send discovery broadcast 3 times
    for (int i = 0; i < 3; i++) {
        ::sendto(sock, DISCOVERY_MSG, strlen(DISCOVERY_MSG), 0,
                 (struct sockaddr*)&addr, sizeof(addr));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    // Set receive timeout
    struct timeval tv{timeout_s, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[2048];
    struct sockaddr_in from{};
    socklen_t from_len = sizeof(from);

    while (true) {
        auto n = ::recvfrom(sock, buf, sizeof(buf) - 1, 0,
                            (struct sockaddr*)&from, &from_len);
        if (n <= 0) break;
        buf[n] = 0;

        // Parse JSON response
        std::string json(buf);
        auto get = [&](const std::string& key) -> std::string {
            auto pos = json.find("\"" + key + "\":\"");
            if (pos == std::string::npos) return "";
            pos += key.size() + 4;
            auto end = json.find('"', pos);
            return (end == std::string::npos) ? "" : json.substr(pos, end - pos);
        };

        DeviceInfo di;
        di.id         = get("gwId");
        di.ip         = inet_ntoa(from.sin_addr);
        di.version    = get("version");
        di.product_id = get("productKey");
        di.mac        = get("mac");

        if (!di.id.empty()) devices.push_back(di);
    }

    ::close(sock);
    return devices;
}

} // namespace tuya
