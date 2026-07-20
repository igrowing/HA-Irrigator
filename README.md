# Irrigator

An ESP32 garage irrigation controller: 11 sprinkler zones + a shared 24VAC pump, driven by a
rotary-encoder + OLED local UI and a full MQTT API for Home Assistant integration. Firmware is
Arduino/PlatformIO C++ (see [agents.md](agents.md) for the internal architecture and coding
rules if you're contributing code, not just using the device).

## Hardware

| Function                 | Pin/Bus                 | Notes                                              |
|---------------------------|--------------------------|-----------------------------------------------------|
| I2C bus (shared)          | SDA=GPIO21, SCL=GPIO22  | DS3231 RTC, SSD1309 OLED                            |
| Zones 1–11                 | GPIO 4, 16, 17, 5, 18, 19, 23, 25, 26, 27, 13 | Active low, all pulled up externally to allow ESP32 boot  |
| Pump                      | GPIO 15                 | Active low     |
| Status/fault LED          | GPIO 14                 | Active high                                         |
| Rotary encoder + button   | GPIO 34, 39, 35 (BTN)         | All pulled up externally (34/35/39 are input-only), A and B are low-pass protected (RC). Button is debounced with 220nF. |
| DS3231 RTC                | I2C, addr 0x68          |                                                      |
| SSD1309 OLED (128x64)     | I2C, addr 0x3C          |                                                      |
| 24VAC current sense       | GPIO 32 (ADC)           | Valve current, ~200mA nominal                       |
| 230VAC current sense      | GPIO 33 (ADC)           | Pump current, ~800mA nominal                        |

## Feature overview

- **11 independently-named sprinkler zones** plus pump-only runs, with the pump always
  pre-started ~500ms before a valve opens and kept on for a 60s graceful-shutdown window after a
  valve closes (protects the pump from rapid-cycle wear) — if another watering is queued up, it
  hands off to the still-running pump immediately instead of waiting out that window. Each zone
  can also be individually disabled (`settings.json`'s `zone_names.<n>.enabled`) to exclude it
  from scheduled, manual, and MQTT-triggered watering alike — an MQTT `cmd/run` against a disabled
  zone is rejected with a `/alert` instead of running, and the local UI's manual zone picker (see
  below) skips disabled zones entirely so they can't be dialed up in the first place.
- **Weekly schedule** (`schedule.json`, up to 32 records) with a per-zone watering-percentage
  multiplier, a pause/rain-skip mechanism, and a 15-deep queue so overlapping or MQTT-triggered
  runs during an active watering don't get lost — they queue and run in order, back-to-back on
  the same pump cycle whenever the pump is still spinning down from the previous one.
- **Local UI**: SSD1309 OLED + rotary encoder, auto-dimming after 20s of inactivity, full menu
  for stopping, viewing/adjusting the schedule, and settings.
- **MQTT**: status/availability reporting (Home-Assistant-compatible LWT), remote run/stop/
  schedule control, current-fault alerting, OTA updates, calibration, and file backup.
- **Captive-portal setup**: first boot (or after a factory-reset-style config wipe) serves a
  Wi-Fi AP with a setup page; it tests both Wi-Fi *and* MQTT connectivity before committing the
  new config, and rolls back to AP mode if either fails.
- **DS3231-backed clock** with NTP sync and automatic EU DST switching (last Sunday of March/
  October, 01:00 UTC).

## First-time setup (captive portal)

1. Power on a unit with no `config.json` on flash (a fresh unit, or one after a config wipe). It
   starts an access point named `irrigator<xx>_SETUP` (where `<xx>` is the last two hex digits of
   its MAC address) — the LED blinks 300ms on / 700ms off while in this mode, and the OLED shows
   the AP name and IP (`192.168.4.1`).
2. Connect to that AP and open `http://192.168.4.1/`. Fill in:
   - Wi-Fi SSID / password
   - MQTT broker address, port (default 1883), username/password (optional)
   - Timezone as a plain UTC offset, `-13`..`+13` (EU DST is applied automatically on top of
     this — you don't need to account for it here)
3. Submitting the form does a **one-shot** Wi-Fi connect attempt (no retries), then, if that
   succeeds, a **one-shot** MQTT connect attempt. If both succeed, `config.json` is saved and the
   device restarts into normal operation. If either fails, `config.json` is deleted and the
   device stays in AP mode so you can try again.

## Local UI (rotary + OLED)

### Screen 1 — normal screen

```
IP.42    2026-07-11 14:32:07
----
           ZONE 3
      WATERING 4:32
     LONG CLICK = MENU
```

Four lines: IP (last octet) + date/time top, the big status line (zone name / remaining-minutes
during a manual pick), a status line (`IDLE-READY` / `SCHEDULED` / `WATERING m:ss` /
`REMAINING m:ss`, the last two alternating every 3s), and a help line. A row of 0–4 dashes under
the IP shows WiFi signal quality at a glance — no dashes is miserable/no signal, four is
excellent. RSSI is sampled every 5s (not read live), so it won't visibly flicker; the number and
verbal quality (`WiFi: good (-62dB)`) are also in **Settings** (below) for the exact reading.

- **Idle, nothing scheduled**: status line reads `IDLE-READY`.
- **Idle, schedule active**: status line reads `SCHEDULED`, main line shows the next zone due to
  run.
- **Watering or pumping**: main line shows the active zone (or `PUMP`), status alternates
  `WATERING 4:32` / `REMAINING 11:05` (minutes:seconds elapsed/remaining in the current run);
  rotary ticks are ignored (only a long click, which opens the menu, does anything) so you can't
  accidentally bump the duration of a run already in progress.

### Manual watering (idle only)

1. Turn the knob — the display switches to zone selection, cycling `PUMP → Zone 1 → … → Zone 11
   → PUMP …` by name. Disabled zones (see **Feature overview** above) are skipped over — if every
   zone is disabled, the picker just stays on `PUMP`.
2. Click to confirm the zone. The display switches to `<default> MINs`.
3. Turn the knob to adjust — clockwise/counter-clockwise step by
   `±(duration/10 + 1)` minutes (so small durations move by 2 min/tick, larger ones move faster),
   clamped 1–120 minutes.
4. Click to start (valve + pump, or pump alone for the `PUMP` selection), or hold the button
   down 1s (long click) at any point to cancel back to the normal screen without starting
   anything.

### Menu (long click from the normal screen)

```
MENU
> Stop ZONE 3        (only shown while something is running)
  Stop all            (only shown while something is running)
  Schedule
  Test all zones
  Settings
  BACK
```

- **Stop `<zone>`** (only present while watering): opens a confirm screen —
  `STOP?  [NO]  YES` — turn the knob to move the selection box onto `YES`, click to confirm.
  Either choice returns you to the normal screen. Confirming closes the valve immediately but,
  for a zone run, gives the pump the same graceful-shutdown tail a normal end-of-duration
  completion gets (see **Feature overview** above) — it does not cut the pump immediately, only
  a pump-only run does that. It also pops and starts the next queued item, if any — same hand-off
  as a plain (non-`all`) MQTT `cmd/stop`; if there is one, it starts immediately off the
  still-running pump rather than waiting out the tail.
- **Stop all** (only present while watering): opens the same confirm screen as **Stop `<zone>`**
  (`STOP ALL?  [NO]  YES`). Confirming aborts whatever's currently running (same graceful pump
  tail as above) *and* empties the watering queue — unlike **Stop `<zone>`**, it does not hand
  off to the next queued item, since there isn't one left.
- **Schedule**: a flat, scrolling list — for each program `<n>`: zone, start day, start time,
  duration, active/paused. Every row is clickable and edits that field:
  - **Zone** / **start day**: click to edit, turn the knob to step through zones 1–11 (by name)
    or days Monday–Sunday, click to confirm.
  - **Start time**: click to edit the hour (0–23, wraps), click again to confirm and move to the
    minute (0–59, wraps), click once more to confirm the whole time.
  - **Duration**: same step formula as manual watering, capped 1–60 min.
  - **Status**: click toggles active/paused immediately, no separate confirm step.
  A long click at any point during an edit cancels it and discards the change.
- **Test all zones**: a 3-item submenu —
  ```
  TEST ALL ZONES
  > Run
    Dur./zone: 5 min
    BACK
  ```
  - **Run**: queues every *enabled* zone (see per-zone `enabled` in Settings/`settings.json`) for
    `Dur./zone` minutes each, one after another — same queue as any other watering request, so
    the first zone starts immediately if idle and the rest follow back-to-back. The screen
    returns to the normal screen right away, same as starting a manual watering.
  - **Dur./zone**: click to edit, same step formula as manual watering (±`(duration/10 + 1)`
    min/tick, clamped 1–120), click to confirm. Defaults to 5 min and is session-only (resets to
    5 on reboot, not persisted to flash).
  - **BACK**: returns to the main menu.
- **Settings**: `Adjust: 100 %` (±10%/tick, click to edit, click to confirm), `Schedule:
  active/paused` (click toggles), `WiFi: good (-62dB)` (verbal quality + RSSI, display only, same
  5s-sampled reading as the normal screen's dashes), `IP: ...`, `MQTT: ...`, `Topic: irrigatorF4`
  (the MQTT prefix — see [MQTT](#mqtt) below), and `FW: v.<version>` (display only, not
  clickable), `BACK`.

### Reset to factory defaults

|  **  DANGER ZONE **  |
|---|
| Long - 10 seconds - click on "Boot" button (GPIO 0)  deletes all configuration files from the local storage if comfirmed on the display. Then irrigator reboots in Captive portal mode, ready for new configuration. |
|  |
| Implemented only for manual execution when the irrigator is open and accessible |

All menus wrap around (scrolling past the last item lands back on the first) and time out to the
normal screen, then to display-off, after 20s of no input — any tick or click while off just
wakes the screen back to normal without acting on that input.

## MQTT

Every topic is prefixed `irrigator<xx>` where `<xx>` is the last two hex digits of the device's
MAC address (e.g. `irrigatorF4`). Substitute your device's actual prefix below.

### Command/response

|  Command topic | Command msg | Response topic | Response msg | Comment |
|--|--|--|--|--|
| `/cmd/state` | "" | `/state` | `{"datetime":"2026-07-12T06:00:00",`<br>`"zone":3,"name":"Back Lawn",`<br>`"on":true,"pump":true,`<br>`"reason":"manual","scheduled":true,"remaining_min":12}` | On every start/stop of a valve or pump, and on demand via `cmd/state` (reason is one of manual/schedule/mqtt/abort; zone:0 means pump-only; scheduled reflects whether the schedule engine is currently active/paused; remaining_min is minutes left in the current run, 0 when off). Also fires the instant the valve closes even while the pump is still in its graceful-shutdown tail (`on:false,pump:true`), so `remaining_min`/`on` track the requested duration, not the pump's extra runtime. |
| `/cmd/queue` | "" | `/queue` | list of waiting waterings from the queue | On every push and pull from watering queue or by `cmd/queue` request. |
| `/cmd/run` | `{"zone":3,"duration":15}` |  |  | **No special response** Response will be either adding to the queue or alert. Starts zone 3 for 15 min (or queues it if something's already running). `zone:0` = pump only. |
| `/cmd/stop` | empty, or `all`  |  |  | **No special response** Response will be either adding to the queue or alert. Stops the current run — closes the valve immediately, but a zone run's pump still gets its normal graceful-shutdown tail rather than cutting immediately (a pump-only run has no such tail). Plain/empty payload also pops and starts the next queued item, if any; `all` also empties the queue. |
| `/cmd/scheduled` | bool | `/scheduled` | bool | Turn the schedule on/off, write the schedule state in settings.json if changed, report back true/false. | 
| `/cmd/watering_percentage` | integer | `/watering_percentage` | integer | Sets the schedule-wide watering multiplier (0–300%), persisted to `settings.json`. |
| `/cmd/skip_rain_until` | `"2026-07-12T06:00:00"` |  `/skip_rain_until` | `"2026-07-12T06:00:00"` | Suppresses **scheduled** runs until this UTC timestamp (manual/MQTT runs are never affected — this only gates the schedule engine). |
|  |  | `/online` |  boolean (LWT on ungraceful disconnect, or explicitly before a deliberate restart). | **Retained message**. Matches Home Assistant's default MQTT `availability_topic` convention. |
| `/cmd/status` | "" | `/status`  | JSON: `datetime`, `firmware_version`, `boot_reason`, `ip`, `uptime_s`, `rssi_db`, `wifi_quality` (verbal, `miserable`/`poor`/`fair`/`good`/`excellent`), `free_heap_bytes`, `free_storage_bytes`, `cpu_temp_c`, `outputs` (only lists zones/pump that are currently on, each with `current_ma`). | ~15s after boot and on request.  |
|  |  | `/alert` | see **Error reporting** below | Fire-and-forget notices: current out of expected range, watering queue full, OTA result, and any command/topic error. Not retained, not queued for delivery guarantees. Every alert carries a `datetime` field — JSON alerts get it spliced in alongside their other keys, plain-text alerts are wrapped as `{"datetime":"...","message":"..."}`. |
| `/cmd/restart` | "" |  |  | **No special response** Publishes `false` to `online`, then restarts. |
| `/cmd/ota` | FW `.bin` URL | `/ota_completed` | `"2026-07-12T06:00:00"` |  Downloads and flashes that URL, restarts on success. Blocking by design — see [agents.md](agents.md) for why. |
| `/cmd/set_calibration` | `{"valve_zero_mv":1650,`<br>`"valve_mv_per_ma":1.0,`<br>`"pump_zero_mv":1650,`<br>`"pump_mv_per_ma":1.0}` |   |  | **No special response** Any subset of the four fields; updates the live current-sensor calibration and persists it to `settings.json`. See **Current-sensor calibration** below — the factory defaults are *not* real calibration values. |
| `/cmd/download/<filename>` | "" | `/download/<filename>` | String | Reads that file from flash and publishes it. |
| `/cmd/upload/<filename>` | new file content |  |  | Writes that file to flash. |

#### Error reporting

Every command above rejects malformed or out-of-range input rather than guessing at intent
(e.g. clamping a duration, defaulting a missing field) — invalid input is reported, not
silently dropped or coerced. A malformed payload, an out-of-range value, an unrecognized
`cmd/<name>`, or a `download` for a file that doesn't exist all publish to `irrigatorF4/alert`
(each also carries a `datetime` field, omitted below for brevity):

```json
{"command":"run","error":"duration out of range (1-120)"}
{"command":"set_calibration","error":"calibration value out of range"}
{"command":"nonexistent_command","error":"unknown command"}
{"download":"missing.json","error":"not found"}
{"download":"settings.json","error":"failed to read"}
{"upload":"schedule.json","error":"invalid schedule.json content"}
{"upload":"schedule.json","error":"failed to write"}
```

### Examples

```
# Round-trip: fetch the current schedule, edit it locally, push it back.
mosquitto_sub -t irrigatorF4/download/schedule.json -C 1 > schedule.json
mosquitto_pub -t irrigatorF4/cmd/download/schedule.json -m ""
# ...edit schedule.json...
mosquitto_pub -t irrigatorF4/cmd/upload/schedule.json -f schedule.json
```

### Current-sensor calibration

`readMilliAmps()` on both current sensors is a linear `(mV_read - zero_offset_mv) / mv_per_mA`
conversion. The zero-offset and sensitivity are **placeholders** in firmware
(`board_config.h`) until you calibrate against your actual installed sensor hardware:

1. With the corresponding output (valve or pump) fully off, note the raw millivolt reading —
   that's your `zero_mv`.
2. Turn the output on with a known load and note the millivolt delta per known milliamp —
   that's `mv_per_ma`.
3. Push the result via `/cmd/set_calibration`; it's saved to `settings.json` and survives
   reboots (and re-applied on every boot before anything reads current).

## Building and uploading firmware (OTA)

`scripts/ota.py` builds the firmware with PlatformIO and pushes it to the device over the
`/cmd/ota` protocol documented above, instead of you having to build, host, and publish the URL
by hand each time:

1. Runs `pio run -e <env>` (skip with `--skip-build` to just re-serve an existing
   `.pio/build/<env>/firmware.bin`).
2. Serves that `firmware.bin` from a throwaway HTTP server bound to this machine's real LAN
   NIC (it deliberately skips VPN/virtual adapters — see the script for why).
3. Publishes that URL to `<prefix>/cmd/ota` (QoS 1, so a broker ACK proves it was actually
   accepted) and waits for the device's `/ota_completed` or `/alert` response.

Requires the `pio` CLI on `PATH` (or under `~/.platformio/penv`) and the `paho-mqtt` and
`psutil` Python packages. Basic usage:

```
python scripts/ota.py --host 192.168.178.45 --user mqtt_user --password <pw>
```

Useful flags: `--port` (MQTT broker port, default 1883), `--topic-prefix` (device prefix, e.g.
`irrigatorF4`, default `irrigatorF4`), `--env` (PlatformIO environment, default `esp32dev`),
`--http-port` (local port serving `firmware.bin`, default 8000), `--skip-build`, `--timeout`
(seconds to wait for OTA confirmation, default 180).

### VS Code status bar button

The repo's `.vscode/tasks.json` already wires this up as a one-click status bar button, using
the recommended `actboy168.tasks` extension (see `.vscode/extensions.json`) which renders any
task's `options.statusbar` as a clickable item in VS Code's status bar. To add your own (e.g.
for a different broker or device prefix), add a task like this to `.vscode/tasks.json`:

```json
{
    "type": "shell",
    "command": "python",
    "args": [
        "scripts/ota.py",
        "--host", "ha.local",
        "--user", "mqtt_user",
        "--password", "<pw>",
        "--topic-prefix", "irrigatorF4"
    ],
    "problemMatcher": [],
    "presentation": {
        "reveal": "always",
        "panel": "dedicated"
    },
    "label": "OTA: Flash irrigator",
    "options": {
        "statusbar": {
            "label": "$(cloud-upload) OTA Flash",
            "tooltip": "Build firmware and push it to the irrigator over MQTT OTA"
        }
    }
}
```

Reload the window (or the extension picks it up automatically) and the label appears as a
clickable button in the status bar — no need to open the Command Palette or a terminal to
flash a new build.

## Home Assistant integration

There's no MQTT auto-discovery yet, so entities are defined manually — either in
`configuration.yaml` under the `mqtt:` integration, or via **Settings → Devices & Services →
MQTT → Add entity** in the UI. A minimal starting set:

```yaml
mqtt:
  binary_sensor:
    - name: "Irrigator Online"
      state_topic: "irrigatorF4/online"
      payload_on: true
      payload_off: false

  sensor:
    - name: "Irrigator Free Heap"
      state_topic: "irrigatorF4/status"
      value_template: "{{ value_json.free_heap_bytes }}"
      unit_of_measurement: "B"
    - name: "Irrigator Active Zone"
      state_topic: "irrigatorF4/state"
      value_template: "{{ value_json.name if value_json.on else 'idle' }}"

  # "Run zone 3 for 15 minutes" as a button — duration is fixed per button; add one per
  # zone/duration combo you actually use, or drive cmd/run from a script/automation instead.
  button:
    - name: "Water Back Lawn (15 min)"
      command_topic: "irrigatorF4/cmd/run"
      payload_press: '{"zone":3,"duration":15}'
    - name: "Stop Irrigation"
      command_topic: "irrigatorF4/cmd/stop"
      payload_press: ""

  number:
    - name: "Irrigator Watering %"
      command_topic: "irrigatorF4/cmd/watering_percentage"
      state_topic: "irrigatorF4/status"
      min: 0
      max: 300
      step: 10
```

For rain-based skipping, an automation reading a weather/rain-forecast entity and publishing to
`cmd/skip_rain_until` covers the spec's intent — HA computes the timestamp from its own forecast
data. The payload is a bare ISO8601 UTC timestamp string, not JSON:

```yaml
automation:
  - alias: "Skip irrigation after rain forecast"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rain_forecast_mm
        above: 5
    action:
      - service: mqtt.publish
        data:
          topic: "irrigatorF4/cmd/skip_rain_until"
          payload: "{{ (now() + timedelta(hours=24)).strftime('%Y-%m-%dT%H:%M:%S') }}"
```

Alerts (`irrigatorF4/alert`) are free-form JSON/text and not retained — a generic MQTT trigger
automation that notifies you on any message to that topic is the simplest way to surface them:

```yaml
automation:
  - alias: "Irrigator alert notification"
    trigger:
      - platform: mqtt
        topic: "irrigatorF4/alert"
    action:
      - service: notify.mobile_app
        data:
          message: "Irrigator alert: {{ trigger.payload }}"
```

## Known limitations

- Current-sensor calibration constants ship as placeholders — readings are meaningless until
  calibrated per **Current-sensor calibration** above.
- The SSD1309 U8g2 driver variant (`NONAME0` vs `NONAME2`) hasn't been verified against a real
  panel; if the display shows garbage or inverted colors on first boot, swap the constructor in
  `src/ui/IrrigatorUi.h`.
