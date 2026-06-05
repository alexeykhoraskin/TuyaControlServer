# TuyaControlServer

Standalone C++17 HTTP server for controlling Tuya/Smart Life devices over LAN (local protocol) and cloud API, with IR Control Hub support for infrared devices.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                   main.cpp                          │
│  loads config → creates DeviceManager → starts HTTP │
└──────────────────────┬──────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────┐
│                  DeviceManager                      │
│  manages devices, routes commands, background polls │
│  ┌─────────────────┐  ┌──────────────────────────┐  │
│  │   LocalDevice    │  │        CloudApi          │  │
│  │  v2/v3.4 TCP     │  │  REST (token, sign, IR)  │  │
│  │  AES-ECB/HMAC    │  │  /v1.0/iot-03/*          │  │
│  │  session keys    │  │  /v2.0/infrareds/*       │  │
│  └────────┬────────┘  └───────────┬──────────────┘  │
│           │                       │                  │
└───────────┼───────────────────────┼──────────────────┘
            │                       │
    ┌───────▼───────┐       ┌──────▼───────┐
    │ Tuya Device   │       │ Tuya Cloud   │
    │ (LAN, 6668)   │       │ openapi.tuya │
    └───────────────┘       └──────────────┘
```

### Layers

- **`types.h`** — shared types: `DeviceInfo`, `DeviceState`, `DpsMap`, frame/command enums, connection modes
- **`crypto.{h,cpp}`** — AES-ECB-128 (PKCS7), HMAC-SHA256, local-key derivation (MD5→hex slice)
- **`frame.{h,cpp}`** — Tuya network frame encoding/decoding for v2.x (CRC32) and v3.4 (HMAC)
- **`local_device.{h,cpp}`** — LAN client: TCP connect, session-key negotiation (v3.4), DP_QUERY_NEW, CONTROL_NEW, heartbeat, poll loop
- **`cloud_api.{h,cpp}`** — Tuya Cloud API client: token management, HMAC-SHA256 signing, standard DP commands (`/v1.0/iot-03/`), IR Control Hub commands (`/v2.0/infrareds/`), device listing, device details (local_key/ip/status codes from cloud)
- **`device_discovery.{h,cpp}`** — UDP broadcast discovery on port 6666
- **`device_manager.{h,cpp}`** — orchestrator: multi-device lifecycle, local-first with cloud fallback, periodic cloud refresh (3600s), background polling
- **`server.{h,cpp}`** — single-threaded poll-based HTTP server with JSON REST API
- **`config.{h,cpp}`** — JSON config parser (no external dependencies)

### Command routing

```
send_command(device_id, dps)
  │
  ├── device.is_infrared() ──→ CloudApi::send_ir_command()
  │                               POST /v2.0/infrareds/{hub}/remotes/{remote}/command
  │
  ├── local connected ──────→ LocalDevice::send_commands()
  │                               v2:  {"devId":"...","dps":{...}}
  │                               v3.4: {"protocol":5,"t":N,"data":{"dps":{...}}}
  │
  └── fallback ─────────────→ CloudApi::send_commands()
                                  POST /v1.0/iot-03/devices/{id}/commands
```

## Dependencies

- **C++17** compiler (gcc >= 8, clang >= 7)
- **OpenSSL** (libssl-dev, libcrypto-dev) — AES, HMAC-SHA256, MD5
- **libcurl** (libcurl4-openssl-dev) — cloud API HTTP client (optional; stub if absent)

### Install on Debian/Ubuntu

```bash
sudo apt install build-essential cmake libssl-dev libcurl4-openssl-dev
```

## Build

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
```

Produces `build/tuya_control_server` and `build/fetch_devices` (helper tool).

## Configuration

Create `config.local.json` (gitignored, real credentials):

```json
{
    "listen": "0.0.0.0",
    "port": 8080,
    "poll_interval": 30,
    "access_id": "YOUR_TUYA_ACCESS_ID",
    "access_secret": "YOUR_TUYA_ACCESS_SECRET",
    "region": "eu",
    "devices": [
{
    "id": "DEVICE_ID",
    "ip": "192.168.1.100",
    "local_key": "YOUR_LOCAL_KEY_16B",
    "name": "Living Room Light",
    "version": "3.4",
    "encrypt": true
},
{
    "id": "IR_LIGHT_ID",
    "ip": "0.0.0.0",
    "name": "IR Light",
    "ir_hub_id": "IR_HUB_DEVICE_ID",
    "ir_category_id": 10,
    "ir_remote_index": 1234567890
}
    ]
}
```

Config lookup order: CLI arg → `config.local.json` → `config.json`.

### Device fields

| Field | Required | Description |
|-------|----------|-------------|
| `id` | yes | Tuya device ID (20-char hex) |
| `ip` | yes | LAN IP or `"0.0.0.0"` for cloud-only/IR |
| `name` | no | Human-readable label |
| `local_key` | conditional | Required for LAN control (16 bytes) |
| `version` | no | Protocol version: `"3.4"` or `"2.x"` |
| `encrypt` | no | `true`/`false` (default `false`) |
| `ir_hub_id` | conditional | Parent Smart IR hub device ID (infrared only) |
| `ir_category_id` | conditional | IR category ID (10=Light, 2=TV, 5=AC...) |
| `ir_remote_index` | conditional | Remote index from IR hub |

IP handling:
- Private IPs (`192.168.x.x`, `10.x.x.x`, `172.16-31.x.x`) are preserved
- `0.0.0.0` is overwritten by cloud API public IP, or marked `cloud_only` if empty
- Infrared devices use `0.0.0.0` and route via the IR Control Hub API

### fetch_devices tool

Queries the Tuya cloud for all devices in your account and prints a ready-to-use config fragment:

```bash
./build/fetch_devices
```

## Run

```bash
./build/tuya_control_server [config_path]
```

Default: listens on `0.0.0.0:8080`. On startup:
1. Loads devices from config
2. Refreshes device details from cloud (local_key, IP, status codes)
3. Connects to LAN devices (v2 or v3.4 protocol)
4. Starts background cloud refresh (every 3600s)
5. Starts UDP discovery (5s, logs found devices)
6. Starts HTTP server

## API

### `GET /devices`

List all devices with connection mode and online status.

```json
{
    "devices": [
        {
            "id": "bf6ef6e5ae1e4747c6dntb",
            "ip": "192.168.1.12",
            "name": "LightNew",
            "mode": "v2",
            "online": true,
            "error": ""
        }
    ]
}
```

### `GET /devices/{id}`

Device detail with current DPS values.

```json
{
    "id": "bf6ef6e5ae1e4747c6dntb",
    "ip": "192.168.1.12",
    "name": "LightNew",
    "version": "3.4",
    "mode": "v2",
    "dps": {
        "1": true,
        "2": 500
    }
}
```

### `POST /devices/{id}/command`

Send commands to a device.

**Standard DPS (LAN / cloud DP devices):**

```json
{"dps": {"1": true}}
{"dps": {"1": false, "2": 500}}
```

**IR infrared devices:**

```json
{"ir_key": "PowerOn"}
{"ir_key": "PowerOff"}
{"ir_key": "Brightness+"}
```

### `GET /api/status`

Health check.

```json
{"status": "running"}
```

## IR Control Hub

The server integrates with Tuya's **IR Control Hub Open Service** (`/v2.0/infrareds/`) to control infrared devices through a Smart IR hub.

### How it works

1. An IR hub device (category `wnykq`) has multiple **remote controls** bound to it
2. Each remote represents an infrared device (e.g., a light bulb, TV, AC)
3. The remote ID is the same as the sub-device ID in your Tuya account
4. Commands are sent as standard IR keys via the cloud API

### Configuration

Add IR metadata to the sub-device's config entry:

```json
{
    "id": "bf9ee3bd844a45f0dfhaat",
    "ip": "0.0.0.0",
    "name": "IR Light",
    "ir_hub_id": "bfd8f18c7f954a0d24hscu",
    "ir_category_id": 10,
    "ir_remote_index": 1735882494
}
```

Find your IR hub's remote list:

```bash
curl -s "https://openapi.tuyaeu.com/v2.0/infrareds/{hub_id}/remotes"
```

### Standard IR keys by category

| Category | ID | Keys |
|----------|----|------|
| **Light** | 10 | `PowerOn`, `PowerOff`, `ColdLight`, `WarmLight`, `Brightness+`, `Brightness-`, `RedLight`, `GreenLight`, `BlueLight` |
| **TV** | 2 | `Power`, `OK`, `Channel+`, `Channel-`, `Volume+`, `Volume-`, `Menu`, `Up`, `Down`, `Left`, `Right`, `0`-`9` |
| **AC** | 5 | `PowerOn`, `PowerOff`, `M0`–`M4` (modes), `T16`–`T30` (temp), `F0`–`F3` (fan) |
| **Fan** | 8 | `PowerOn`, `PowerOff`, `Swing`, `Speed`, `Timing`, `NegativeIon` |
| **Set-top Box** | 1 | `Power`, `OK`, `Menu`, `0`-`9`, `*`, `#` |
| **Projector** | 6 | `PowerOn`, `PowerOff`, `OK`, `Menu`, `Volume+`, `Volume-`, `Mute`, `Back`, `Home` |

## Protocol support

### v2.x (CRC32)

- Frame format: `[prefix(4) seq(4) cmd(4) len(4) payload(N) crc32(4) suffix(4)]`
- Encryption: AES-ECB-128 with raw local_key
- Commands: `CMD_CONTROL_NEW` (0x0D), `CMD_DP_QUERY_NEW` (0x10)

### v3.4 (HMAC + session key)

- Extends v2 with 3-way session key negotiation (`0x03`→`0x04`→`0x05`)
- Session key: XOR(local_nonce, remote_nonce) encrypted with AES-ECB
- Frame integrity: HMAC-SHA256 over header + encrypted payload
- Command payload: `{"protocol":5,"t":<ts>,"data":{"dps":{...}}}`

### Cloud API

- Token-based auth with HMAC-SHA256 signing
- Signing: `HMAC(secret, client_id + token + timestamp + method + "\n" + SHA256(body) + "\n\n" + path)`
- Endpoints: `/v1.0/iot-03/devices/{id}/commands` (standard), `/v2.0/infrareds/{hub}/remotes/{remote}/command` (IR)

## Project structure

```
├── CMakeLists.txt
├── config.json              # Template (committed, placeholders)
├── config.local.json        # Real credentials (gitignored)
├── include/
│   ├── cloud_api.h
│   ├── config.h
│   ├── crypto.h
│   ├── device_discovery.h
│   ├── device_manager.h
│   ├── frame.h
│   ├── local_device.h
│   ├── server.h
│   └── types.h
├── src/
│   ├── cloud_api.cpp
│   ├── config.cpp
│   ├── crypto.cpp
│   ├── device_discovery.cpp
│   ├── device_manager.cpp
│   ├── frame.cpp
│   ├── local_device.cpp
│   ├── main.cpp
│   └── server.cpp
├── tools/
│   └── fetch_devices.cpp     # Cloud device lister
└── third_party/              # Vendored dependencies (none currently)
```

## License

Apache 2.0
