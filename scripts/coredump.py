#!/usr/bin/env python3
"""Pull the irrigator's stored crash dump over MQTT and decode it.

The ESP32 panic handler writes an ELF core dump into the `coredump` flash
partition on every crash (see src/partitions.csv). This script asks the device
for it over MQTT, reassembles the base64 chunks, verifies the transfer, writes
the raw image to a file, and hands it to `esp-coredump` for decoding against the
firmware.elf that was running at the time. See docs/coredump.md for the wire
format and the whole workflow.

Usage:
    python scripts/coredump.py --host 192.168.178.45
    python scripts/coredump.py --host 192.168.178.45 --summary-only
    python scripts/coredump.py --host 192.168.178.45 --elf builds/1.1.7/firmware.elf
"""
import argparse
import base64
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import zlib

import paho.mqtt.client as mqtt

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class CoreDumpCollector:
    """Reassembles the `<prefix>/download/coredump` message stream.

    Three message shapes arrive on that one topic, distinguished by their keys:
    a `{"coredump":"begin",...}` header carrying the size/chunk count and the
    decoded summary, then `{"seq":N,"data":"<base64>"}` chunks, then a
    `{"coredump":"end",...,"crc32":"0x..."}` trailer. Everything is QoS 0 and
    fire-and-forget, so gaps are detected here rather than assumed away.
    """

    def __init__(self):
        self.header = None
        self.trailer = None
        self.chunks = {}
        self.error = None
        self.started = threading.Event()
        self.finished = threading.Event()
        self.last_message_at = time.monotonic()

    def on_payload(self, payload):
        self.last_message_at = time.monotonic()
        try:
            message = json.loads(payload)
        except json.JSONDecodeError:
            print(f"[warn] ignoring non-JSON message on the download topic: {payload[:120]}")
            return

        kind = message.get("coredump")
        if kind == "begin":
            self.header = message
            self.chunks.clear()
            self.started.set()
            summary = message.get("summary", {})
            print(f"[recv] header: {message.get('size')} bytes in {message.get('chunks')} chunks "
                  f"(device firmware {message.get('firmware_version')})")
            print_summary(summary)
        elif kind == "end":
            self.trailer = message
            self.finished.set()
        elif "seq" in message and "data" in message:
            self.chunks[int(message["seq"])] = message["data"]
            if self.header:
                print(f"\r[recv] chunk {len(self.chunks)}/{self.header.get('chunks', '?')}",
                      end="", flush=True)

    def assemble(self):
        """Returns (image_bytes, crc_verified).

        A missing chunk raises — there is no image to hand back. A CRC mismatch does not: the
        bytes still made the round trip, and a corrupt dump you can inspect beats no dump at all.
        The caller decides what a False means (here: non-zero exit, and never auto-clear).
        """
        if self.header is None:
            raise RuntimeError("never received the transfer header")
        expected_chunks = int(self.header["chunks"])
        expected_size = int(self.header["size"])

        missing = [n for n in range(1, expected_chunks + 1) if n not in self.chunks]
        if missing:
            raise RuntimeError(
                f"missing chunk(s) {missing} of {expected_chunks} — every message is QoS 0, so a "
                f"Wi-Fi dip drops them silently. Just run this again; the dump stays on the device "
                f"until you erase it with --clear."
            )

        image = b"".join(base64.b64decode(self.chunks[n]) for n in range(1, expected_chunks + 1))
        if len(image) != expected_size:
            raise RuntimeError(f"reassembled {len(image)} bytes but the header promised {expected_size}")

        if self.trailer is None:
            print("[warn] no trailer received — chunk count and size check out, but the "
                  "end-to-end CRC could not be verified")
            return image, False

        # esp_rom_crc32_le() seeded with 0 is plain CRC-32 (IEEE 802.3), the same value zlib
        # computes — the same pairing ESP-IDF itself relies on, since the core dump image's own
        # trailing checksum is written with that function and verified by esp-coredump with
        # zlib.crc32. Kept non-fatal anyway: esp-coredump will independently reject a genuinely
        # damaged image when it decodes, so this check is a faster, more specific signal rather
        # than the last line of defence.
        expected_crc = int(self.trailer["crc32"], 16)
        actual_crc = zlib.crc32(image) & 0xFFFFFFFF
        if actual_crc != expected_crc:
            print(f"[warn] CRC32 mismatch: device said 0x{expected_crc:08x}, got 0x{actual_crc:08x}. "
                  f"The image is saved regardless — try decoding it, and re-run the transfer if "
                  f"esp-coredump also rejects it.")
            return image, False
        print("[ok]   transfer CRC32 verified")
        return image, True


def print_summary(summary):
    if not summary:
        return
    if summary.get("coredump") != "present":
        print("[info] device reports no stored core dump")
        return
    print(f"       task           : {summary.get('task')}")
    print(f"       panic reason   : {summary.get('panic_reason') or '(none recorded)'}")
    print(f"       PC             : {summary.get('pc')}")
    print(f"       exc cause      : {summary.get('exc_cause')}")
    print(f"       fault address  : {summary.get('fault_addr')}")
    print(f"       app elf sha256 : {summary.get('elf_sha256')}")
    backtrace = summary.get("backtrace") or []
    if backtrace:
        corrupt = " (CORRUPTED — treat as a hint, not gospel)" if summary.get("bt_corrupted") else ""
        print(f"       backtrace{corrupt}: {' '.join(backtrace)}")


def find_esp_coredump():
    return shutil.which("esp-coredump") or shutil.which("espcoredump.py")


def decode(image_path, elf_path, chip="esp32"):
    """Runs esp-coredump against the saved image. Returns True if it ran successfully."""
    tool = find_esp_coredump()
    if tool is None:
        print("\n[decode] esp-coredump is not on PATH — install it with `pip install esp-coredump`, "
              "then run:")
        print(f"    esp-coredump --chip {chip} info_corefile "
              f"--core {image_path} --core-format raw {elf_path}")
        return False
    if not os.path.isfile(elf_path):
        print(f"\n[decode] no firmware.elf at {elf_path} — decoding needs the *exact* build that "
              f"was running when it crashed (see the app elf sha256 above). Point --elf at your "
              f"archived copy, then run:")
        print(f"    esp-coredump --chip {chip} info_corefile "
              f"--core {image_path} --core-format raw {elf_path}")
        return False

    command = [tool, "--chip", chip, "info_corefile",
               "--core", image_path, "--core-format", "raw", elf_path]
    print(f"\n[decode] {' '.join(command)}\n")
    result = subprocess.run(command, cwd=REPO_ROOT)
    if result.returncode != 0:
        print(f"[decode] esp-coredump exited {result.returncode}")
        return False
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--host", required=True, help="MQTT broker LAN address")
    parser.add_argument("--port", type=int, default=1883, help="MQTT broker port (default 1883)")
    parser.add_argument("--user", default=None, help="MQTT username")
    parser.add_argument("--password", default=None, help="MQTT password")
    parser.add_argument("--topic-prefix", default="irrigatorF4", help="Device MQTT topic prefix")
    parser.add_argument("--output", default=os.path.join(REPO_ROOT, "coredump.bin"),
                        help="Where to write the raw image (default ./coredump.bin)")
    parser.add_argument("--elf", default=os.path.join(REPO_ROOT, ".pio", "build", "esp32dev", "firmware.elf"),
                        help="firmware.elf to decode against — must be the exact build that crashed")
    parser.add_argument("--summary-only", action="store_true",
                        help="Ask for cmd/coredump and print the summary; skip the image transfer")
    parser.add_argument("--no-decode", action="store_true", help="Save the image without running esp-coredump")
    parser.add_argument("--clear", action="store_true",
                        help="Erase the dump on the device after a verified, successfully decoded "
                             "download (never happens automatically — see docs/coredump.md)")
    parser.add_argument("--timeout", type=int, default=90, help="Seconds to wait for the transfer (default 90)")
    args = parser.parse_args()

    download_topic = f"{args.topic_prefix}/download/coredump"
    summary_topic = f"{args.topic_prefix}/coredump"
    alert_topic = f"{args.topic_prefix}/alert"

    collector = CoreDumpCollector()
    connected = threading.Event()
    summary_seen = threading.Event()
    device_error = {}

    def on_connect(client, userdata, flags, rc):
        if rc != 0:
            sys.exit(f"[mqtt] connect failed (rc={rc})")
        client.subscribe([(download_topic, 0), (summary_topic, 0), (alert_topic, 0)])
        connected.set()

    def on_message(client, userdata, msg):
        payload = msg.payload.decode(errors="replace")
        if msg.topic == download_topic:
            collector.on_payload(payload)
        elif msg.topic == summary_topic:
            try:
                print_summary(json.loads(payload))
            except json.JSONDecodeError:
                print(f"[mqtt] {msg.topic}: {payload}")
            summary_seen.set()
        elif msg.topic == alert_topic and "coredump" in payload.lower():
            # The device reports every command failure this way (see technical.md's Error
            # reporting) — surface it instead of sitting out the full timeout.
            print(f"\n[device] {payload}")
            if '"error"' in payload:
                device_error["message"] = payload
                collector.finished.set()
                summary_seen.set()

    client = mqtt.Client()
    if args.user:
        client.username_pw_set(args.user, args.password)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(args.host, args.port, keepalive=60)
    client.loop_start()

    if not connected.wait(timeout=10):
        client.loop_stop()
        client.disconnect()
        sys.exit("[mqtt] never got a CONNACK from the broker after 10s")

    def publish_command(topic, payload=""):
        # QoS 1 so the broker's PUBACK proves it accepted the message; the device still subscribes
        # at QoS 0, so this proves nothing about delivery to the device — hence the waits below.
        info = client.publish(topic, payload, qos=1)
        info.wait_for_publish(timeout=10)
        print(f"[mqtt] published to {topic}")

    exit_code = 0
    try:
        if args.summary_only:
            publish_command(f"{args.topic_prefix}/cmd/coredump")
            if not summary_seen.wait(timeout=15):
                sys.exit("[wait] no summary from the device within 15s — is it online?")
            return

        publish_command(f"{args.topic_prefix}/cmd/download/coredump")
        if not collector.started.wait(timeout=20):
            if device_error:
                sys.exit(f"[fail] device refused the request: {device_error['message']}")
            sys.exit("[wait] the device never started a transfer within 20s — is it online, and "
                     "does it actually have a dump? Try --summary-only.")

        deadline = time.monotonic() + args.timeout
        while not collector.finished.wait(timeout=1.0):
            if time.monotonic() > deadline:
                print("\n[warn] transfer timed out; assembling whatever arrived")
                break
            # The device pushes one chunk per loop() iteration with no pacing, so a gap this long
            # means it stopped sending, not that it's being slow.
            if time.monotonic() - collector.last_message_at > 15:
                print("\n[warn] 15s with no chunks; assembling whatever arrived")
                break
        print()

        if device_error:
            sys.exit(f"[fail] device reported: {device_error['message']}")

        image, crc_ok = collector.assemble()
        with open(args.output, "wb") as handle:
            handle.write(image)
        print(f"[save] {len(image)} bytes -> {args.output}")
        if not crc_ok:
            exit_code = 1

        decoded = False
        if not args.no_decode:
            decoded = decode(args.output, args.elf)

        if args.clear:
            # Erasing is irreversible and the whole transfer was QoS 0, so it takes proof the copy
            # is good — a clean CRC *and* a decode esp-coredump was happy with. See docs/coredump.md.
            if decoded and crc_ok:
                publish_command(f"{args.topic_prefix}/cmd/coredump_clear", "true")
                time.sleep(2)
            else:
                print("[skip] --clear ignored: this run did not produce a verified, decoded copy, "
                      "so erasing the device's only record would be a gamble. Confirm the dump is "
                      "usable, then re-run with --clear, or publish cmd/coredump_clear yourself.")
    except RuntimeError as error:
        print(f"[fail] {error}")
        exit_code = 1
    finally:
        client.loop_stop()
        client.disconnect()

    sys.exit(exit_code)


if __name__ == "__main__":
    main()
