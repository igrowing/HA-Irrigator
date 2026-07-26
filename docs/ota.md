# Updating the firmware (OTA)

OTA ("over-the-air") lets you push a new firmware build to the Irrigator over Wi-Fi, straight from
your computer — no need to unscrew the enclosure or plug in a USB cable every time there's an
update. This guide walks through it assuming you're comfortable with GitHub but have only opened
VS Code a couple of times.

## Before you start

You'll need:

1. **This repository, open in VS Code.** If you haven't already, clone it and open the folder in
   VS Code (`File > Open Folder...`).
2. **The recommended extensions installed.** VS Code should prompt you to install them the first
   time you open this folder ("This workspace has extension recommendations") — click **Install
   All**. If it doesn't prompt you, open the **Extensions** panel (the four-squares icon in the
   left sidebar), search `@recommended`, and install everything listed there. This includes the
   PlatformIO extension, which is what actually builds the firmware.
3. **Python 3**, with two packages installed. Open a terminal (`Terminal > New Terminal` in VS
   Code) and run:
   ```
   pip install paho-mqtt psutil
   ```
4. **Your MQTT broker's details** — the same address/username/password your Irrigator is already
   configured to use (see [usage_homeassistant.md](usage_homeassistant.md) if you're not sure).
5. **Your device's topic prefix**, e.g. `irrigatorF4`. You can find it on the device itself:
   long-click the knob to open the menu, go to **Settings**, and look for the **Topic** line.
6. Your computer and the Irrigator need to be **on the same Wi-Fi/LAN** — OTA works by having the
   device download the new firmware directly from your computer.

## Running an update

There are two ways to trigger it — pick whichever's more comfortable.

### Option A: one-click status bar button

The repo already ships a VS Code task for this, in `.vscode/tasks.json`, with placeholder values:

```json
{
    "command": "python",
    "args": [
        "scripts/ota.py",
        "--host", "ha.local",
        "--user", "mqtt_user",
        "--password", "mqtt_user-password-here"
    ],
    "label": "OTA: Flash irrigator",
    ...
}
```

1. Open `.vscode/tasks.json` and edit the `--host`, `--user`, and `--password` values under the
   `"OTA: Flash irrigator"` task to match your own MQTT broker. If your device's topic prefix isn't
   the default `irrigatorF4`, add `"--topic-prefix", "irrigatorXX"` to the `args` list too.
2. Save the file. A button labeled **☁ OTA Flash** should appear in the blue status bar at the
   bottom of the VS Code window (this comes from the `actboy168.tasks` extension — it's one of the
   recommended extensions from step 2 above).
3. Click that button whenever you want to build and flash the latest code. A terminal panel opens
   and shows progress (see **What to expect**, below).

### Option B: from the terminal

Open a terminal in VS Code (`` Terminal > New Terminal ``) and run:

```
python scripts/ota.py --host <broker-address> --user <mqtt-user> --password <mqtt-password> --topic-prefix irrigatorXX
```

For example:

```
python scripts/ota.py --host 192.168.1.10 --user mqtt_user --password hunter2 --topic-prefix irrigatorF4
```

## What to expect

The script does four things, printing a line as it starts each one:

1. **`[build]`** — compiles the firmware with PlatformIO. Takes a minute or two the first time,
   faster afterwards.
2. **`[http]`** — starts a temporary web server on your own computer to serve the freshly-built
   firmware file. Windows may pop up a firewall prompt the first time ("Windows Defender Firewall
   has blocked some features of Python") — click **Allow access**, otherwise the Irrigator won't
   be able to download the file from your PC.
3. **`[mqtt]`** — connects to your MQTT broker and tells the device where to download the new
   firmware from. It then waits for the device itself to confirm receipt, and retries a couple of
   times if the device doesn't respond — this matters on flaky Wi-Fi, where the broker can accept
   the message even though it never actually reached the device (e.g. it arrived exactly while the
   device was mid Wi-Fi-reconnect).
4. **`[wait]`** — waits (up to 3 minutes by default) for the device to confirm it flashed
   successfully and restarted. The device is unresponsive to everything else while this happens —
   that's expected, not a bug.

A successful run ends with:

```
[done] OTA completed at 2026-07-20T14:32:07
```

## If something goes wrong

- **`Could not find the 'pio' executable`** — the PlatformIO extension hasn't finished installing,
  or hasn't installed its command-line tool yet. Reopen VS Code, wait for the PlatformIO icon in
  the sidebar to finish its first-time setup, and try again.
- **`broker never acknowledged the publish`** — the script couldn't reach your MQTT broker, or
  your username/password don't have permission to publish. Double check `--host`/`--user`/
  `--password`.
- **`device never confirmed receipt of the OTA command`** — the broker accepted the publish, but
  the device itself never acknowledged it after a few retries — usually weak/flaky Wi-Fi dropping
  the message in transit. Check that the device is powered on and connected (the screen should show
  its IP address, not a `_SETUP` network), then just try again.
- **`timed out with no OTA confirmation`** — the device took receipt of the command but never
  reported back on finishing. Check that it's powered on, connected to Wi-Fi (the screen should show
  its IP address, not a `_SETUP` network), and on the same network as your computer. You can also
  check the device's screen directly — if it's mid-flash it will say so.
- **`[fail] device reported: ...`** — the device received the update but rejected or failed it;
  the message printed alongside this tells you why.

## Useful extra options

All optional — add them to the `args` list (Option A) or the command line (Option B):

| Flag | Default | What it does |
|---|---|---|
| `--port` | `1883` | MQTT broker port |
| `--topic-prefix` | `irrigatorF4` | Your device's MQTT prefix (see **Before you start**) |
| `--env` | `esp32dev` | PlatformIO build environment |
| `--http-port` | `8000` | Local port used to serve the firmware file |
| `--skip-build` | off | Re-send the last build without recompiling — useful if the build already succeeded and you just want to retry sending it |
| `--timeout` | `180` | Seconds to wait for the device to confirm before giving up |
