# Crash dumps (core dumps)

When the firmware panics — a null dereference, a stack overflow, an unhandled exception, an
interrupt watchdog — the ESP32's panic handler writes a full ELF core dump into flash *before*
rebooting. It contains the crashing task's registers and every task's stack and TCB, which is
enough to reconstruct the exact line of code that died and how it got there.

The dump survives reboots and power cycles. It is only overwritten by the next crash, or erased
when you explicitly ask for it — so a crash that happened overnight is still there in the morning.

This matters here because the device lives in a garage with no attached terminal. Without this,
the only record of a panic is a backtrace printed to a serial console nobody was watching.

## Is it already on?

Yes, on any unit flashed after the `coredump` partition was added to
[`src/partitions.csv`](../src/partitions.csv). The capture itself is pure ESP-IDF: the prebuilt
Arduino core already ships with `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` and
`CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y`, and the panic handler looks for a `coredump`-subtype
partition on its own. Before that partition existed the handler simply found nowhere to write and
silently did nothing, which is what every boot on this device did historically.

> **Flashing the new partition table wipes SPIFFS once.** The `spiffs` entry shrank from
> `0x0F0000` to `0x0E0000` at the same offset to make room, so the existing filesystem fails its
> mount check and `FileStore::begin()` reformats it. Back up `settings.json` and `schedule.json`
> via `cmd/download/` first, then upload them back. This is a one-time cost paid on whichever boot
> first sees the new table, and it does **not** affect OTA (`app0`/`app1` are untouched).

## Retrieving a dump

### The quick version

```bash
python scripts/coredump.py --host 192.168.178.45 --topic-prefix irrigatorF4
```

That publishes the request, reassembles the chunks, verifies the transfer, writes `coredump.bin`,
and runs `esp-coredump` against `.pio/build/esp32dev/firmware.elf`. Useful flags:

| Flag | What it does |
|--|--|
| `--summary-only` | Just prints the task, PC, backtrace and panic reason — no image transfer. |
| `--elf <path>` | Decode against an archived build instead of the current one. **Read the warning below.** |
| `--no-decode` | Save the raw image and stop. |
| `--clear` | Erase the dump on the device — but only if this run produced a CRC-clean image that `esp-coredump` then decoded successfully. |

### You need the exact firmware.elf that crashed

Every address in a dump is an offset into one specific build. Decode against a different
`firmware.elf` — even one commit later — and you get function names that are confidently,
silently wrong. Since this device is updated over the air, the build that crashed is often *not*
the one in `.pio/build/` any more.

The summary reports `elf_sha256`, the truncated SHA256 the crashing firmware was built with, for
exactly this reason. **Archive `.pio/build/esp32dev/firmware.elf` alongside every `.bin` you push
over OTA** — it costs a few MB and it is the difference between a diagnosable crash and a
mystery.

### Doing it by hand over MQTT

```bash
# Summary only — task, PC, backtrace, panic reason.
mosquitto_pub -t irrigatorF4/cmd/coredump -m ""
mosquitto_sub -t irrigatorF4/coredump -C 1

# The whole image.
mosquitto_sub -t irrigatorF4/download/coredump &
mosquitto_pub -t irrigatorF4/cmd/download/coredump -m ""
```

### Over USB instead

If you have physical access, you don't need any of the above — the image can be read straight off
the chip, and this path also works on a unit whose Wi-Fi or MQTT is broken:

```bash
pip install esp-coredump
esp-coredump --chip esp32 --port COM5 info_corefile .pio/build/esp32dev/firmware.elf
```

## Erasing a dump

Nothing erases a dump automatically — not even a successful download. Every MQTT publish in this
firmware is QoS 0 with no delivery acknowledgement anywhere, so auto-erasing after a transfer
would destroy the only evidence of a crash the moment one chunk dropped in transit. Erase
deliberately, once you have confirmed a decodable copy:

```bash
mosquitto_pub -t irrigatorF4/cmd/coredump_clear -m "true"
```

The `true` payload is a required confirmation, the same gate `cmd/delete/<filename>` uses. You do
not have to erase at all — the next crash overwrites the partition regardless. Erasing only buys
you certainty that whatever you read next came from a *new* crash.

## What the device tells you unprompted

* At boot, ~15s in alongside `/status`, if a dump is stored: the full summary on `/coredump`, plus
  an `/alert` telling you to come get it. You learn about an overnight crash from Home Assistant,
  without asking.
* `/status` carries `coredump_available` on every publish. Read it together with `boot_reason`:
  `boot_reason: "panic"` says this boot followed a crash, `coredump_available: true` says there is
  still something to read about it.

## Wire format

The transfer is a stream of JSON messages on `<prefix>/download/coredump`, distinguished by their
keys. The image is base64-encoded in 3072-byte raw chunks (4096 chars each), one published per
`loop()` iteration so the transfer never stalls irrigation or the UI.

```jsonc
// 1. header — carries the summary too, so a truncated transfer is still worth something
{"coredump":"begin","size":21504,"chunks":7,"chunk_bytes":3072,"encoding":"base64",
 "firmware_version":"1.1.8","summary":{ /* same shape as cmd/coredump's reply */ }}

// 2. data chunks, seq 1..chunks
{"seq":1,"data":"f0VMRgEBAQ..."}

// 3. trailer — CRC32 (IEEE 802.3, what zlib.crc32 computes) over the raw image
{"coredump":"end","chunks":7,"size":21504,"crc32":"0x9a3f1c07"}
```

Everything is QoS 0. A dropped chunk shows up as a gap in the sequence numbers; a corrupted one
shows up in the CRC. Either way the fix is to run it again — the dump is untouched on the device
until you clear it.

The image itself is the raw flash blob (a 4-byte length header, the ELF, then the ESP-IDF
checksum), which is why it is decoded with `--core-format raw` rather than `elf`.

## A note on what's inside

A core dump is raw RAM. This build has `CONFIG_ESP_COREDUMP_CAPTURE_DRAM` off, so it captures
task stacks and TCBs rather than all of DRAM — but a stack still holds whatever recently passed
through it, which can include the Wi-Fi PSK, the MQTT password, or the weather API key. The
transfer is plain MQTT on port 1883 with no TLS. On a trusted LAN that is fine; just don't
publish a dump to a broker you don't control, and don't use a retained topic for it.

## Reading the output

`esp-coredump info_corefile` prints the crashing task, its registers, a symbolized backtrace, and
a summary of every other task's state. Start with the backtrace of the crashed task. To resolve a
single address by hand:

```bash
xtensa-esp32-elf-addr2line -pfiaC -e .pio/build/esp32dev/firmware.elf 0x400d1234
```

The toolchain binary ships with the platform, under
`~/.platformio/packages/toolchain-xtensa-esp32/bin/`.
