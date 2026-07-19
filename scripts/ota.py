#!/usr/bin/env python3
"""Build the firmware and push it to the irrigator over MQTT OTA.

Builds with PlatformIO, serves the resulting firmware.bin from a throwaway
HTTP server bound to this machine's LAN IP, publishes that URL to
<topic-prefix>/cmd/ota, and waits for the device's /ota_completed or /alert
response. See README.md's MQTT section for the underlying protocol.

Usage:
    python scripts/ota.py --host 192.168.178.45 [--port 1883] [--user U] [--password P]
"""
import argparse
import http.server
import ipaddress
import os
import shutil
import socket
import subprocess
import sys
import threading
import time

import paho.mqtt.client as mqtt
import psutil

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def find_pio():
    found = shutil.which("pio")
    if found:
        return found
    candidates = [
        os.path.expanduser("~/.platformio/penv/Scripts/pio.exe"),
        os.path.expanduser("~/.platformio/penv/bin/pio"),
    ]
    for candidate in candidates:
        if os.path.isfile(candidate):
            return candidate
    sys.exit("Could not find the 'pio' executable on PATH or in ~/.platformio/penv")


def build_firmware(env):
    pio = find_pio()
    print(f"[build] {pio} run -e {env}")
    result = subprocess.run([pio, "run", "-e", env], cwd=REPO_ROOT)
    if result.returncode != 0:
        sys.exit(f"[build] pio run failed (exit {result.returncode})")


_IGNORED_ADAPTER_TAGS = ("tailscale", "zerotier", "vpn", "ppp", "loopback", "virtual", "vethernet", "docker", "wsl")
_PREFERRED_ADAPTER_TAGS = ("wi-fi", "wifi", "wireless", "ethernet", "en0", "eth")


def get_local_ip(remote_host):
    """LAN-facing IPv4 of this machine, preferring the real Wi-Fi/Ethernet NIC.

    A plain UDP connect()+getsockname() trick just returns whatever the OS routing
    table picks, which can be a VPN/mesh adapter (e.g. Tailscale's 100.64.0.0/10
    CGNAT range) instead of the actual LAN NIC the ESP32 can reach. Enumerate
    interfaces instead and pick a private-range address from a real NIC.
    """
    stats = psutil.net_if_stats()
    candidates = []
    for name, addrs in psutil.net_if_addrs().items():
        if name not in stats or not stats[name].isup:
            continue
        if any(tag in name.lower() for tag in _IGNORED_ADAPTER_TAGS):
            continue
        for addr in addrs:
            if addr.family != socket.AF_INET:
                continue
            ip = ipaddress.ip_address(addr.address)
            if ip.is_loopback or ip.is_link_local or not ip.is_private:
                continue
            candidates.append((name, addr.address))

    for name, ip in candidates:
        if any(tag in name.lower() for tag in _PREFERRED_ADAPTER_TAGS):
            return ip
    if candidates:
        return candidates[0][1]

    print("[warn] no real LAN NIC found via interface enumeration, falling back to routing-table guess", file=sys.stderr)
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((remote_host, 1))
        return s.getsockname()[0]
    finally:
        s.close()


def make_firmware_handler(firmware_path):
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            try:
                with open(firmware_path, "rb") as f:
                    data = f.read()
            except OSError as e:
                self.send_error(500, str(e))
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def log_message(self, fmt, *args):
            print(f"[http] {self.address_string()} - {fmt % args}")

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, help="MQTT broker LAN address")
    parser.add_argument("--port", type=int, default=1883, help="MQTT broker port (default 1883)")
    parser.add_argument("--user", default=None, help="MQTT username")
    parser.add_argument("--password", default=None, help="MQTT password")
    parser.add_argument("--topic-prefix", default="irrigatorF4", help="Device MQTT topic prefix")
    parser.add_argument("--env", default="esp32dev", help="PlatformIO environment (default esp32dev)")
    parser.add_argument("--http-port", type=int, default=8000, help="Local port to serve firmware.bin on")
    parser.add_argument("--skip-build", action="store_true", help="Serve the existing firmware.bin without rebuilding")
    parser.add_argument("--timeout", type=int, default=180, help="Seconds to wait for OTA confirmation (default 180)")
    args = parser.parse_args()

    if not args.skip_build:
        build_firmware(args.env)

    firmware_path = os.path.join(REPO_ROOT, ".pio", "build", args.env, "firmware.bin")
    if not os.path.isfile(firmware_path):
        sys.exit(f"No firmware.bin at {firmware_path} — build first or drop --skip-build")

    local_ip = get_local_ip(args.host)
    firmware_url = f"http://{local_ip}:{args.http_port}/firmware.bin"

    httpd = http.server.ThreadingHTTPServer(("0.0.0.0", args.http_port), make_firmware_handler(firmware_path))
    http_thread = threading.Thread(target=httpd.serve_forever, daemon=True)
    http_thread.start()
    print(f"[http] serving {firmware_path} at {firmware_url}")

    done = threading.Event()
    result = {}
    publish_acked = threading.Event()
    pending_mid = {}

    completed_topic = f"{args.topic_prefix}/ota_completed"
    alert_topic = f"{args.topic_prefix}/alert"
    cmd_topic = f"{args.topic_prefix}/cmd/ota"

    def on_connect(client, userdata, flags, rc):
        if rc != 0:
            sys.exit(f"[mqtt] connect failed (rc={rc})")
        client.subscribe([(completed_topic, 0), (alert_topic, 0)])
        print(f"[mqtt] connected, publishing {firmware_url} to {cmd_topic}")
        # QoS 1 so the broker's PUBACK proves it actually accepted the message
        # (QoS 0 would "succeed" even if an ACL silently drops it).
        info = client.publish(cmd_topic, firmware_url, qos=1)
        pending_mid["value"] = info.mid

    def on_publish(client, userdata, mid):
        if mid == pending_mid.get("value"):
            publish_acked.set()

    def on_message(client, userdata, msg):
        payload = msg.payload.decode(errors="replace")
        if msg.topic == completed_topic:
            result["ok"] = True
            result["message"] = payload
            done.set()
        elif msg.topic == alert_topic and "ota" in payload.lower():
            result["ok"] = False
            result["message"] = payload
            done.set()
        else:
            print(f"[mqtt] {msg.topic}: {payload}")

    client = mqtt.Client()
    if args.user:
        client.username_pw_set(args.user, args.password)
    client.on_connect = on_connect
    client.on_publish = on_publish
    client.on_message = on_message
    client.connect(args.host, args.port, keepalive=60)
    client.loop_start()

    if not publish_acked.wait(timeout=10):
        client.loop_stop()
        client.disconnect()
        httpd.shutdown()
        sys.exit(
            f"[mqtt] broker never acknowledged the publish to {cmd_topic} after 10s — "
            f"check that '{args.user}' has publish permission (ACL) for that topic"
        )
    print(f"[mqtt] broker acknowledged the OTA command")

    print(f"[wait] waiting up to {args.timeout}s for {completed_topic} (OTA is blocking on the device, this can take a bit)...")
    finished = done.wait(timeout=args.timeout)

    client.loop_stop()
    client.disconnect()
    httpd.shutdown()

    if not finished:
        sys.exit("[wait] timed out with no OTA confirmation — check the device directly")
    if result["ok"]:
        print(f"[done] OTA completed at {result['message']}")
    else:
        sys.exit(f"[fail] device reported: {result['message']}")


if __name__ == "__main__":
    main()
