# Irrigator

An ESP32 garage irrigation controller: 11 sprinkler zones + a shared 24VAC pump, driven by a
rotary-encoder + OLED local UI and a full MQTT API for Home Assistant integration. Firmware is
Arduino/PlatformIO C++ (see [agents.md](agents.md) for the internal architecture and coding
rules if you're contributing code, not just using the device).

## Hardware

| Function                 | Pin/Bus                 | Notes                                              |
|---------------------------|--------------------------|-----------------------------------------------------|
| I2C bus (shared)          | SDA=GPIO21, SCL=GPIO22  | PCF8574 relay expander, DS3231 RTC, SSD1309 OLED   |
| Zones 1–8                 | PCF8574 (addr 0x20)     | Active low                                          |
| Zones 9–11                | GPIO 25, 26, 27         | Active low                                          |
| Pump                      | GPIO 16                 | Active low     |
| Status/fault LED          | GPIO 14                 | Active low                                          |
| Rotary encoder + button   | GPIO 34, 35, 39         | All pulled up externally (34/35/39 are input-only) |
| DS3231 RTC                | I2C, addr 0x68          |                                                      |
| SSD1309 OLED (128x64)     | I2C, addr 0x3C          |                                                      |
| 24VAC current sense       | GPIO 32 (ADC)           | Valve current, ~200mA nominal                       |
| 230VAC current sense      | GPIO 33 (ADC)           | Pump current, ~800mA nominal                        |

## Feature overview

- **11 independently-named sprinkler zones** plus pump-only runs, with the pump always
  pre-started ~500ms before a valve opens and kept on for a 60s graceful-shutdown window after
  the last valve closes (protects the pump from rapid-cycle wear).
- **Weekly schedule** (`schedule.json`, up to 32 records) with a per-zone watering-percentage
  multiplier, a pause/rain-skip mechanism, and a 10-deep queue so overlapping or MQTT-triggered
  runs during an active watering don't get lost — they queue and run in order.
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
           ZONE 3
        WATERING 4M
     LONG CLICK = MENU
```

Four lines: IP (last octet) + date/time top, the big status line (zone name / remaining-minutes
during a manual pick), a status line (`IDLE` / `SCHEDULED` / `WATERING xM` / `REMAINING yM`,
the last two alternating every 3s), and a help line.

- **Idle, nothing scheduled**: status line reads `IDLE`.
- **Idle, schedule active**: status line reads `SCHEDULED`, main line shows the next zone due to
  run.
- **Watering or pumping**: main line shows the active zone (or `PUMP`), status alternates
  `WATERING 4M` / `REMAINING 11M`; rotary ticks are ignored (only a long click, which opens the
  menu, does anything) so you can't accidentally bump the duration of a run already in progress.

### Manual watering (idle only)

1. Turn the knob — the display switches to zone selection, cycling `PUMP → Zone 1 → … → Zone 11
   → PUMP …` by name.
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
  Schedule
  Settings
  BACK
```

- **Stop `<zone>`** (only present while watering): opens a confirm screen —
  `STOP?  [NO]  YES` — turn the knob to move the selection box onto `YES`, click to confirm.
  Either choice returns you to the normal screen.
- **Schedule**: a flat, scrolling list — for each program `<n>`: zone, start day, start time,
  duration, active/paused. Every row is clickable and edits that field:
  - **Zone** / **start day**: click to edit, turn the knob to step through zones 1–11 (by name)
    or days Monday–Sunday, click to confirm.
  - **Start time**: click to edit the hour (0–23, wraps), click again to confirm and move to the
    minute (0–59, wraps), click once more to confirm the whole time.
  - **Duration**: same step formula as manual watering, capped 1–60 min.
  - **Status**: click toggles active/paused immediately, no separate confirm step.
  A long click at any point during an edit cancels it and discards the change.
- **Settings**: `Adjust: 100 %` (±10%/tick, click to edit, click to confirm), `Schedule:
  active/paused` (click toggles), `IP: ...` and `MQTT IP: ...` (display only, not clickable),
  `BACK`.

All menus wrap around (scrolling past the last item lands back on the first) and time out to the
normal screen, then to display-off, after 20s of no input — any tick or click while off just
wakes the screen back to normal without acting on that input.

## MQTT

Every topic is prefixed `irrigator<xx>` where `<xx>` is the last two hex digits of the device's
MAC address (e.g. `irrigator4f`). Substitute your device's actual prefix below.

### Status & availability

| Topic | Direction | Notes |
|---|---|---|
| `irrigator4f/availability` | published, retained | `online` (on connect) / `offline` (LWT on ungraceful disconnect, or explicitly before a deliberate restart). Matches Home Assistant's default MQTT `availability_topic` convention. |
| `irrigator4f/status` | published | ~15s after boot and on request. JSON: `firmware_version`, `boot_reason`, `ip`, `uptime_s`, `free_heap_bytes`, `free_storage_bytes`, `cpu_temp_c`, `outputs` (only lists zones/pump that are currently on, each with `current_ma`). |
| `irrigator4f/state` | published | On every start/stop of a valve or pump, *and* on demand via `cmd/state`: `{"zone":3,"name":"Back Lawn","on":true,"pump":true,"reason":"manual"}` (`reason` is one of `manual`/`schedule`/`mqtt`/`abort`; `zone:0` means pump-only). |
| `irrigator4f/alert` | published | Fire-and-forget notices: current out of expected range, watering queue full, OTA result, and any command/topic error (see **Error reporting** below). Not retained, not queued for delivery guarantees. |
| `irrigator4f/schedule` | published | Full `schedule.json` content, republished whenever a schedule record changes (from the local UI or MQTT). |

### Commands (subscribe topics, `irrigator4f/cmd/<name>`)

| Command | Payload | Effect |
|---|---|---|
| `cmd/run` | `{"zone":3,"duration":15}` | Starts zone 3 for 15 min (or queues it if something's already running). `zone:0` = pump only. |
| `cmd/stop` | empty, or `all` | Stops the current run. Plain/empty payload also pops and starts the next queued item, if any; `all` also empties the queue. |
| `cmd/state` | any | Publishes the current watering/pump status to `state` on demand (see the table above) — use this to get an immediate answer instead of waiting for the next start/stop transition. |
| `cmd/watering_percentage` | integer, e.g. `80` | Sets the schedule-wide watering multiplier (0–300%), persisted to `settings.json`. |
| `cmd/skip_rain` | `{"skip_until":"2026-07-12T06:00:00"}` | Suppresses **scheduled** runs until this UTC timestamp (manual/MQTT runs are never affected — this only gates the schedule engine). |
| `cmd/restart` | any | Publishes `offline` to `availability`, then restarts. |
| `cmd/ota` | firmware `.bin` URL | Downloads and flashes that URL, restarts on success. Blocking by design — see [agents.md](agents.md) for why. |
| `cmd/set_calibration` | `{"valve_zero_mv":1650,"valve_mv_per_ma":1.0,"pump_zero_mv":1650,"pump_mv_per_ma":1.0}` | Any subset of the four fields; updates the live current-sensor calibration and persists it to `settings.json`. See **Current-sensor calibration** below — the factory defaults are *not* real calibration values. |
| `cmd/download` | filename, e.g. `schedule.json` | Reads that file from flash and publishes it — see **File backup/restore** below. |
| `cmd/upload/<filename>` | new file content | Writes that file to flash — see **File backup/restore** below. |

#### Error reporting

Every command above rejects malformed or out-of-range input rather than guessing at intent
(e.g. clamping a duration, defaulting a missing field) — invalid input is reported, not
silently dropped or coerced. A malformed payload, an out-of-range value, an unrecognized
`cmd/<name>`, or a `download` for a file that doesn't exist all publish to `irrigator4f/alert`:

```json
{"command":"run","error":"duration out of range (1-120)"}
{"command":"set_calibration","error":"calibration value out of range"}
{"command":"nonexistent_command","error":"unknown command"}
{"download":"missing.json","error":"not_found"}
{"upload":"schedule.json","error":"invalid schedule.json content"}
```

### File backup/restore

**Download** — publish a filename as the payload to `cmd/download`, e.g. `config.json`,
`settings.json`, or `schedule.json`. The device reads it from flash and publishes the raw
content to `irrigator4f/download/<filename>` (not retained). If the file doesn't exist, an
`{"download":"<filename>","error":"not_found"}` message goes to `.../alert` instead.

```
mosquitto_pub -t irrigator4f/cmd/download -m schedule.json
mosquitto_sub -t 'irrigator4f/download/#'
```

**Upload** — publish new file content to `cmd/upload/<filename>` (the filename is the topic's
last segment, not the payload — the payload is the file's new content). `config.json`,
`settings.json`, and `schedule.json` are parsed and validated before being written — an invalid
upload is rejected (reported to `.../alert`) and the file on flash is left untouched, same as
loading a corrupt file at boot. Any other filename is written as-is, with no validation, since
the device has no schema for it. A successful `config.json` upload restarts the device (new
Wi-Fi/MQTT credentials only take effect on the next boot); `settings.json` and `schedule.json`
take effect immediately, no restart needed.

```
# Round-trip: fetch the current schedule, edit it locally, push it back.
mosquitto_sub -t irrigator4f/download/schedule.json -C 1 > schedule.json
mosquitto_pub -t irrigator4f/cmd/download -m schedule.json
# ...edit schedule.json...
mosquitto_pub -t irrigator4f/cmd/upload/schedule.json -f schedule.json
```

### Current-sensor calibration

`readMilliAmps()` on both current sensors is a linear `(mV_read - zero_offset_mv) / mv_per_mA`
conversion. The zero-offset and sensitivity are **placeholders** in firmware
(`board_config.h`) until you calibrate against your actual installed sensor hardware:

1. With the corresponding output (valve or pump) fully off, note the raw millivolt reading —
   that's your `zero_mv`.
2. Turn the output on with a known load and note the millivolt delta per known milliamp —
   that's `mv_per_ma`.
3. Push the result via `cmd/set_calibration`; it's saved to `settings.json` and survives
   reboots (and re-applied on every boot before anything reads current).

## Home Assistant integration

There's no MQTT auto-discovery yet, so entities are defined manually — either in
`configuration.yaml` under the `mqtt:` integration, or via **Settings → Devices & Services →
MQTT → Add entity** in the UI. A minimal starting set:

```yaml
mqtt:
  binary_sensor:
    - name: "Irrigator Online"
      state_topic: "irrigator4f/availability"
      payload_on: "online"
      payload_off: "offline"

  sensor:
    - name: "Irrigator Free Heap"
      state_topic: "irrigator4f/status"
      value_template: "{{ value_json.free_heap_bytes }}"
      unit_of_measurement: "B"
    - name: "Irrigator Active Zone"
      state_topic: "irrigator4f/state"
      value_template: "{{ value_json.name if value_json.on else 'idle' }}"

  # "Run zone 3 for 15 minutes" as a button — duration is fixed per button; add one per
  # zone/duration combo you actually use, or drive cmd/run from a script/automation instead.
  button:
    - name: "Water Back Lawn (15 min)"
      command_topic: "irrigator4f/cmd/run"
      payload_press: '{"zone":3,"duration":15}'
    - name: "Stop Irrigation"
      command_topic: "irrigator4f/cmd/stop"
      payload_press: ""

  number:
    - name: "Irrigator Watering %"
      command_topic: "irrigator4f/cmd/watering_percentage"
      state_topic: "irrigator4f/status"
      min: 0
      max: 300
      step: 10
```

For rain-based skipping, an automation reading a weather/rain-forecast entity and publishing to
`cmd/skip_rain` covers the spec's intent — HA computes the timestamp from its own forecast data:

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
          topic: "irrigator4f/cmd/skip_rain"
          payload: >-
            {"skip_until": "{{ (now() + timedelta(hours=24)).strftime('%Y-%m-%dT%H:%M:%S') }}"}
```

Alerts (`irrigator4f/alert`) are free-form JSON/text and not retained — a generic MQTT trigger
automation that notifies you on any message to that topic is the simplest way to surface them:

```yaml
automation:
  - alias: "Irrigator alert notification"
    trigger:
      - platform: mqtt
        topic: "irrigator4f/alert"
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
