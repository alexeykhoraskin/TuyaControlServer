# Copyright (c) 2026 Alexey.Khoraskin@gmail.com
# Licensed under the Apache License, Version 2.0
# https://github.com/alexeykhoraskin/TuyaControlServer

"""
MCP server for TuyaControlServer.

Provides AI agents with tools to discover, query, and control
Tuya/Smart Life devices through a running TuyaControlServer instance.

Usage:
    python3 -m venv .venv
    .venv/bin/pip install -r requirements.txt
    .venv/bin/python server.py [--server http://localhost:8080]
"""

import argparse
import asyncio
import json as jsonlib
import http.client
from urllib.parse import urlparse
from mcp.server import Server
from mcp.server.stdio import stdio_server
from mcp.types import Tool, TextContent

BASE = "http://localhost:8080"


async def _get(path: str) -> dict:
    return _request("GET", path)


async def _post(path: str, body: dict) -> dict:
    return _request("POST", path, jsonlib.dumps(body).encode())


def _request(method: str, path: str, body: bytes | None = None) -> dict:
    parsed = urlparse(BASE)
    conn = http.client.HTTPConnection(parsed.hostname, parsed.port, timeout=15)
    headers = {"Content-Type": "application/json"} if body else {}
    try:
        conn.request(method, path, body=body, headers=headers)
        r = conn.getresponse()
        return jsonlib.loads(r.read())
    except Exception:
        return {}
    finally:
        conn.close()


def _make_server() -> Server:
    server = Server("tuya-control")

    @server.list_tools()
    async def list_tools() -> list[Tool]:
        return [
            Tool(
                name="list_devices",
                description="List all Tuya devices with mode, type (IR/DPS), and online status",
                inputSchema={"type": "object", "properties": {}},
            ),
            Tool(
                name="get_device_status",
                description="Get detailed status of a device by name, including DPS values",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "name": {
                            "type": "string",
                            "description": "Device name (e.g. TH1, TV, LightNew)",
                        }
                    },
                    "required": ["name"],
                },
            ),
            Tool(
                name="send_command",
                description="Send a command to a device. Auto-detects IR vs DPS.",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "name": {
                            "type": "string",
                            "description": "Device name (e.g. TV, LightNew)",
                        },
                        "key": {
                            "type": "string",
                            "description": "IR key name (e.g. Power, Volume+) or DPS code (e.g. switch_led)",
                        },
                        "value": {
                            "type": "string",
                            "description": "DPS value (required for DPS devices, omit for IR)",
                        },
                    },
                    "required": ["name", "key"],
                },
            ),
            Tool(
                name="list_commands",
                description="List available commands for a device (IR keys or DPS codes)",
                inputSchema={
                    "type": "object",
                    "properties": {
                        "name": {
                            "type": "string",
                            "description": "Device name (e.g. TV, LightNew)",
                        }
                    },
                    "required": ["name"],
                },
            ),
        ]

    @server.call_tool()
    async def call_tool(name: str, args: dict) -> list[TextContent]:
        if name == "list_devices":
            data = await _get("/devices")
            devices = data.get("devices", [])
            if not devices:
                return [TextContent(type="text", text="No devices found.")]
            lines = [f"{'Name':<14} {'Mode':<8} {'Type':<5} {'Online':<7} {'ID':<26}",
                     "-" * 70]
            for d in devices:
                lines.append(
                    f"{d.get('name',''):<14} {d.get('mode',''):<8} "
                    f"{'IR' if d.get('ir') else 'DPS':<5} "
                    f"{'yes' if d.get('online') else 'no':<7} "
                    f"{d.get('id',''):<26}"
                )
            return [TextContent(type="text", text="\n".join(lines))]

        if name == "get_device_status":
            dev_name = args["name"]
            all_devs = (await _get("/devices")).get("devices", [])
            dev = next((d for d in all_devs if d.get("name") == dev_name), None)
            if not dev:
                return [TextContent(type="text", text=f"Device not found: {dev_name}")]
            data = await _get(f"/devices/{dev['id']}")
            lines = [f"Device: {data.get('name','')} ({data.get('id','')})",
                     f"IP:     {data.get('ip','')}",
                     f"Mode:   {data.get('mode','')}",
                     f"Online: {'yes' if data.get('online') else 'no'}"]
            dps = data.get("dps", {})
            if dps:
                lines.append("")
                lines.append("Status:")
                for k, v in dps.items():
                    lines.append(f"  {k} = {v}")
            return [TextContent(type="text", text="\n".join(lines))]

        if name == "send_command":
            dev_name = args["name"]
            ir_key = args.get("key")
            dps_val = args.get("value")
            all_devs = (await _get("/devices")).get("devices", [])
            dev = next((d for d in all_devs if d.get("name") == dev_name), None)
            if not dev:
                return [TextContent(type="text", text=f"Device not found: {dev_name}")]
            if dev.get("ir"):
                body = {"ir_key": ir_key}
            elif dps_val is not None:
                body = {"dps": {ir_key: dps_val}}
            else:
                return [TextContent(
                    type="text",
                    text=f"DPS device {dev_name} requires a value argument",
                )]
            result = await _post(f"/devices/{dev['id']}/command", body)
            if result.get("success"):
                return [TextContent(type="text", text="OK")]
            return [TextContent(type="text", text=f"Failed: {result}")]

        if name == "list_commands":
            dev_name = args["name"]
            all_devs = (await _get("/devices")).get("devices", [])
            dev = next((d for d in all_devs if d.get("name") == dev_name), None)
            if not dev:
                return [TextContent(type="text", text=f"Device not found: {dev_name}")]
            data = await _get(f"/devices/{dev['id']}/commands")
            cmds = data.get("commands", [])
            if not cmds:
                return [TextContent(type="text", text="No commands available.")]
            lines = [f"{'Command':<28} {'Type':<12}  {'Values':<30}",
                     "-" * 70]
            for c in cmds:
                vals = c.get("values", "")
                if not vals and c.get("extra"):
                    vals = f"id:{c['extra']}"
                lines.append(
                    f"{c.get('name',''):<28} {c.get('type',''):<12}  {vals:<30}"
                )
            return [TextContent(type="text", text="\n".join(lines))]

        raise ValueError(f"Unknown tool: {name}")

    return server


async def main() -> None:
    global BASE
    parser = argparse.ArgumentParser(description="TuyaControlServer MCP server")
    parser.add_argument("--server", default="http://localhost:8080",
                        help="TuyaControlServer URL")
    args = parser.parse_args()
    BASE = args.server.rstrip("/")

    server = _make_server()
    async with stdio_server() as (read, write):
        await server.run(read, write, server.create_initialization_options())


if __name__ == "__main__":
    asyncio.run(main())
