# Technical Reference

This is the developer-facing reference for the firmware itself — hardware/GPIO assignments,
firmware architecture, and how to build, flash, and test it. For end-user setup and usage, see
[usage_irrigator.md](usage_irrigator.md); for updating an already-deployed unit over Wi-Fi, see
[ota.md](ota.md).

## Hardware

| Function                 | Pin/Bus                 | Notes                                              |
|---------------------------|--------------------------|-----------------------------------------------------|
| I2C bus (shared)          | SDA=GPIO21, SCL=GPIO22  | DS3231 RTC, SSD1309 OLED, 400kHz                    |
| Zones 1–11                 | GPIO 4, 16, 17, 5, 18, 19, 23, 25, 26, 27, 13 | Active low, direct GPIO, all pulled up externally to allow ESP32 boot (avoids the boot-strapping pins 0/2/12) |
| Pump                      | GPIO 15                 | Active low     |
| Status/fault LED          | GPIO 14                 | Active high                                         |
| Rotary encoder + button   | GPIO 34, 39, 35 (BTN)         | All pulled up externally (34/35/39 are input-only), A and B are low-pass protected (RC). Button is debounced with 220nF. Long-click threshold: 1000ms. |
| Factory-reset (BOOT) button | GPIO 0                | Board's built-in BOOT button, repurposed — hold 10s to wipe config (see `FactoryResetButton`) |
| DS3231 RTC                | I2C, addr 0x68          |                                                      |
| SSD1309 OLED (128x64)     | I2C, addr 0x3C          |                                                      |
| 24VAC current sense       | GPIO 32 (ADC)           | Valve current, expected range 20–800mA               |
| 230VAC current sense      | GPIO 33 (ADC)           | Pump current, expected range 200–1800mA              |

All of the above lives exclusively in `include/board_config.h` — it's the **only** file in the
project allowed to hardcode a GPIO number, I2C address, or similar board-specific constant. If
this firmware is ever ported to a different board revision, that file (and only that file) should
need to change. It also holds the operating constants worth knowing about: current-fault
thresholds, JSON file size caps, schedule/duration limits, and the current-sensor calibration
placeholders (`*_ZERO_OFFSET_MV` / `*_MV_PER_MA`) — those two are **not real calibration values**,
just defaults; see **Current-sensor calibration** under **MQTT**, below, for calibrating them
against your actual installed sensor hardware.

## Firmware architecture

The codebase enforces a strict layering — hardware access, business logic, networking, and UI
rendering are never mixed in the same function or file:

- **`src/drivers/`** — thin wrappers around individual physical peripherals (relay outputs, the
  DS3231 RTC, the SSD1309 display, rotary encoder + button input, the ADC current sensors). This is
  the *only* layer allowed to call `Wire.h`, `digitalWrite`, `analogRead`, or similar direct
  hardware APIs, and each driver exposes a small interface (e.g. `IClock`, `ICurrentSensor`) rather
  than a concrete type.
- **`src/domain/`** — pure C++ business logic: the irrigation state machine, the UI/menu state
  machine, the schedule queue, the duration-adjustment formula, current-fault threshold checks.
  This layer only ever depends on the driver *interfaces*, never on `Arduino.h` or a concrete
  Arduino type directly — that's what makes host-run unit testing possible (see **Testing**,
  below).
- **`src/network/`** — Wi-Fi connection handling, the captive portal, the MQTT client wrapper,
  OTA, NTP sync.
- **`src/ui/`** — display rendering and encoder input dispatch. Reads state from `domain/`; never
  contains irrigation logic itself.
- **`include/board_config.h`** — all pin numbers, I2C addresses, and board-specific constants (see
  **Hardware**, above).

A few architectural rules worth knowing before touching the code (see [agents.md](../agents.md)
for the full, non-negotiable list this project holds itself to):

- **Composition root, not scattered globals.** Every long-lived object (relay bank, RTC, display,
  MQTT client, Wi-Fi manager, the irrigation state machine) is instantiated exactly once, in
  `main.cpp`, and passed by reference/pointer into whatever needs it.
- **The watering queue is the choke point, not a side buffer.** What *does* hold the architecture together is `main.ino`'s watering queue:
  a manual start from the rotary UI, a scheduled run firing due, an MQTT `cmd/run`, and "Test all
  zones" queuing up every enabled zone all funnel through the exact same
  `enqueueOrStart()` → `WateringQueue::push()` → `pumpQueue()` → `WateringQueue::pop()` path — even
  a request that can start immediately gets pushed and immediately popped back off, rather than
  short-circuiting straight into `startIrrigation()`. That's a deliberate, if non-obvious, choice:
  - **One choke point enforces every invariant.** Whether a zone is disabled, whether the queue is
    full, and whether it's actually safe to start right now (idle, or mid pump-graceful-shutdown
    for a seamless hand-off — `IrrigationStateMachine::canStartImmediately()`) are each checked in
    exactly one place. A design where manual/scheduled/MQTT triggers each decided "start now vs.
    reject" independently would need that same logic duplicated (and kept in sync) at every call
    site — a natural place for one path to drift and skip a check the others still enforce.
  - **Reporting is uniform for free.** Every request gets both a push report and a pop report,
    even the common case of starting immediately with nothing else running — callers never need
    a separate "started immediately" vs. "queued" code path.
  - **Overlapping requests are just... next in line.** Because "arriving" and "starting" are
    already two separate steps, a schedule firing while an MQTT run is active, or "Test all zones"
    handing over 11 zones at once, don't need special-cased merge logic — they queue up and drain
    back-to-back through the same `pumpQueue()` call, in order.
  - **It's fully unit-testable.** `WateringQueue` and `IrrigationStateMachine` are pure `domain/`
    code with no hardware dependency, so the entire "can this start now, and what happens to
    everything behind it" logic is covered by the native test suite (see **Testing**, below)
    rather than only being verifiable on real hardware.
- **Fail loud, not silently plausible.** Malformed or out-of-range data (a bad `schedule.json`, an
  out-of-range MQTT command) is rejected and alerted on, never clamped or defaulted into something
  that looks reasonable — it's better for the device to stay idle than to water the wrong zone (or
  the right zone for the wrong duration) from corrupted data.

## Clock sync and daylight saving

The device keeps time from three sources, layered as fallbacks (`src/drivers/Clock.cpp`):

1. On boot, `restoreFromRtc()` reads the DS3231 RTC as an immediate baseline, so the clock is
   already sane before the network is even up.
2. `configTime(0, 0, "pool.ntp.org")` then requests NTP over a bounded ~5s wait. NTP is
   deliberately requested in plain UTC (no TZ string) rather than asking the library to do
   timezone/DST math itself.
3. Once NTP succeeds, the result is written back to the DS3231, so the RTC stays accurate across
   power loss even between successful syncs.

Your configured UTC offset (`config.json`'s `timezone` field, entered as a plain `-13`..`+13`
hour offset during Wi-Fi setup) and daylight saving are both applied in software on top of that
UTC baseline, via `domain::isEuDstActive()`:

- DST is the fixed **EU rule**: clocks go forward on the last Sunday of March at 01:00 UTC, and
  back on the last Sunday of October at 01:00 UTC.
- It's deliberately evaluated in UTC (not "02:00 local time", which is how the rule is often
  phrased) — the UTC framing is what actually keeps the transition simultaneous across every EU
  country regardless of their individual standard-time offset, and it's a pure function of the
  current UTC timestamp with no extra state to track.
- This is hardcoded to the EU rule specifically — a non-EU deployment observing a different DST
  rule (or none at all) would need a different `isEuDstActive()`-equivalent function; there's no
  region setting that switches this.

## MQTT

Every topic is prefixed `irrigator<xx>`, where `<xx>` is the last two hex digits of the device's
MAC address (e.g. `irrigatorF4`, from `makeDevicePrefix()` in `src/main.ino`). Substitute your
device's actual prefix in every example below.

### Command/response

|  Command topic | Command msg | Response topic | Response msg | Comment |
|--|--|--|--|--|
| `/cmd/state` | "" | `/state` | `{"datetime":"2026-07-12T06:00:00",`<br>`"zone":3,"name":"Back Lawn",`<br>`"on":true,"pump":true,`<br>`"reason":"manual","scheduled":true,"remaining_min":12}` | On every start/stop of a valve or pump, and on demand via `cmd/state` (reason is one of manual/schedule/mqtt/abort; zone:0 means pump-only; scheduled reflects whether the schedule engine is currently active/paused; remaining_min is minutes left in the current run, 0 when off). Also fires the instant the valve closes even while the pump is still in its graceful-shutdown tail (`on:false,pump:true`), so `remaining_min`/`on` track the requested duration, not the pump's extra runtime. |
| `/cmd/queue` | "" | `/queue` | list of waiting waterings from the queue | On every push and pull from watering queue or by `cmd/queue` request. |
| `/cmd/run` | `{"zone":3,"duration":15}` |  |  | **No special response** Response will be either adding to the queue or alert. Starts zone 3 for 15 min (or queues it if something's already running). `zone:0` = pump only. A **disabled** zone (`zone_names.<n>.enabled: false` in `settings.json`) is rejected outright with an `/alert` — `{"message":"Zone 3 (Back Lawn) is disabled. Enable it in settings first."}` — it's never silently ignored, and it never reaches the watering queue at all (see `enqueueOrStart()` in `src/main.ino`). |
| `/cmd/stop` | empty, or `all`  |  |  | **No special response** Response will be either adding to the queue or alert. Stops the current run — closes the valve immediately, but a zone run's pump still gets its normal graceful-shutdown tail rather than cutting immediately (a pump-only run has no such tail). Plain/empty payload also pops and starts the next queued item, if any; `all` also empties the queue. |
| `/cmd/scheduled` | bool | `/scheduled` | bool | Turn the schedule on/off, write the schedule state in settings.json if changed, report back true/false. |
| `/cmd/watering_percentage` | integer | `/watering_percentage` | integer | Sets the schedule-wide watering multiplier (0–300%), persisted to `settings.json`. |
| `/cmd/skip_rain_until` | `"2026-07-12T06:00:00"` |  `/skip_rain_until` | `"2026-07-12T06:00:00"` | Suppresses **scheduled** runs until this UTC timestamp (manual/MQTT runs are never affected — this only gates the schedule engine). |
|  |  | `/online` |  boolean (LWT on ungraceful disconnect, or explicitly before a deliberate restart). | **Retained message**. Matches Home Assistant's default MQTT `availability_topic` convention. |
| `/cmd/status` | "" | `/status`  | JSON: `datetime`, `firmware_version`, `boot_reason`, `ip`, `uptime_s`, `rssi_db`, `wifi_quality` (verbal, `miserable`/`poor`/`fair`/`good`/`excellent`), `free_heap_bytes`, `free_storage_bytes`, `cpu_temp_c`, `outputs` (only lists zones/pump that are currently on, each with `current_ma`). | ~15s after boot and on request.  |
|  |  | `/alert` | see **Error reporting** below | Fire-and-forget notices: current out of expected range, watering queue full, OTA result, and any command/topic error. Not retained, not queued for delivery guarantees. Every alert carries a `datetime` field — JSON alerts get it spliced in alongside their other keys, plain-text alerts are wrapped as `{"datetime":"...","message":"..."}`. |
| `/cmd/restart` | "" |  |  | **No special response** Publishes `false` to `online`, then restarts. |
| `/cmd/ota` | FW `.bin` URL | `/ota_started` then `/ota_completed` | `"true"`, then `"2026-07-12T06:00:00"` |  `/ota_started` fires the instant the command is accepted (before the HTTP GET), separately from success/failure - `cmd/#` is only delivered at QoS 0, so a caller can't tell "broker accepted my publish" from "device actually received it" without a distinct receipt signal (see [ota.md](ota.md)'s retry logic, built on exactly this). Downloads and flashes the URL, restarts on success. Blocking by design — see [agents.md](../agents.md) for why. See [ota.md](ota.md) for the higher-level tooling built on top of this. |
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

```
# Start zone 3 for 15 minutes, then check what's running.
mosquitto_pub -t irrigatorF4/cmd/run -m '{"zone":3,"duration":15}'
mosquitto_sub -t irrigatorF4/state -C 1
```

```
# Stop everything and clear the queue.
mosquitto_pub -t irrigatorF4/cmd/stop -m "all"
```

### JSON file formats

The three files under `/cmd/download/<filename>` and `/cmd/upload/<filename>` (see the table
above) always round-trip as a whole file — there's no partial-field update, so any write replaces
the entire file. Missing fields on upload fall back to firmware defaults, never to whatever was
previously on flash (see `parseSettings()`/`parseSchedule()`).

**`settings.json`** (see `src/domain/Settings.h`/`.cpp`):
```json
{
  "watering_percentage": 100,
  "schedule_active": false,
  "default_manual_duration": 5,
  "valve_current_zero_offset_mv": 1650,
  "valve_current_mv_per_ma": 1,
  "pump_current_zero_offset_mv": 1650,
  "pump_current_mv_per_ma": 1,
  "zone_names": {
    "1": {"name": "Trees left", "enabled": true},
    "2": {"name": "Triangle", "enabled": true},
    "3": {"name": "Back left", "enabled": true},
    "4": {"name": "Mid-left", "enabled": true},
    "5": {"name": "Mid-right", "enabled": true},
    "6": {"name": "Back gate", "enabled": true},
    "7": {"name": "Back right", "enabled": true},
    "8": {"name": "Trees right", "enabled": true},
    "9": {"name": "Ginko", "enabled": true},
    "10": {"name": "Front", "enabled": true},
    "11": {"name": "Zone 11", "enabled": false}
  }
}
```

**`schedule.json`** — a flat array of records, up to `SCHEDULE_MAX_RECORDS` (32), `day_of_week`
0-indexed Monday=0..Sunday=6 (see `src/domain/Schedule.h`/`.cpp`):
```json
[
  {"zone": 1, "day_of_week": 2, "start_time": "06:00", "duration_min": 30, "active": true},
  {"zone": 8, "day_of_week": 2, "start_time": "06:30", "duration_min": 30, "active": true},
  {"zone": 2, "day_of_week": 3, "start_time": "06:00", "duration_min": 9, "active": true},
  {"zone": 3, "day_of_week": 3, "start_time": "06:09", "duration_min": 9, "active": true}
]
```

**`config.json`** (Wi-Fi/MQTT/timezone, written by the captive portal — see
`src/domain/ConfigStore.h`/`.cpp`):
```json
{
  "ssid": "MyHomeWifi",
  "password": "hunter2",
  "mqtt_broker": "192.168.1.10",
  "mqtt_port": 1883,
  "mqtt_user": "mqtt_user",
  "mqtt_password": "hunter2",
  "timezone": 2
}
```

### Current-sensor calibration

`readMilliAmps()` on both current sensors is a linear `(mV_read - zero_offset_mv) / mv_per_mA`
conversion (see `src/drivers/CurrentSensor.cpp`). The zero-offset and sensitivity are
**placeholders** in firmware (`board_config.h`) until you calibrate against your actual installed
sensor hardware:

1. With the corresponding output (valve or pump) fully off, note the raw millivolt reading — read
   it via `readRawMilliVolts()` (exposed over `/status`'s current sensor data while a zone/pump is
   on, or add a temporary log line) — that's your `zero_mv`.
2. Turn the output on with a known load and note the millivolt delta per known milliamp — that's
   `mv_per_ma`.
3. Push the result via `/cmd/set_calibration`, e.g.:
   ```
   mosquitto_pub -t irrigatorF4/cmd/set_calibration -m '{"valve_zero_mv":1650,"valve_mv_per_ma":2.2,"pump_zero_mv":1650,"pump_mv_per_ma":1.65}'
   ```
   It's saved to `settings.json` and survives reboots (re-applied on every boot before anything
   reads current), and you can send any subset of the four fields — omitted ones keep their
   current value.

### Current-fault alerting

`CurrentFaultMonitor` (`src/domain/CurrentFaultMonitor.h`, wired up in `updateCurrentMonitoring()`
in `src/main.ino`) watches each running output's current draw against a `{minMa, maxMa}` band —
`VALVE_CURRENT_MIN_MA`/`MAX_MA` (20–800mA) for zones and `PUMP_CURRENT_MIN_MA`/`MAX_MA`
(200–1800mA) for the pump, both in `board_config.h`. A reading outside that band (too low usually
means a disconnected/failed valve or dry-running pump; too high usually means a stuck valve,
jammed pump, or wiring fault) triggers `/alert`:
```json
{"valve":3,"current":950,"units":"mA"}
{"pump":0,"current":50,"units":"mA"}
```

The **measurement window** for a given output starts the instant it's commanded on and keeps
running until `CURRENT_MEASURE_POST_OFF_MS` (10s) after it turns off — not just while it's
nominally "on" — which also catches a valve that fails to fully close. `CurrentFaultMonitor`'s
`beginActiveCycle()`/`endActiveCycle()` bracket that exact same window, which is what gives **one
alert per cycle, not one per bad reading**, for free: `checkAndConsumeAlert()` only fires once
until the window closes and reopens, so a valve stuck out-of-range for an entire 15-minute run
raises exactly one `/alert`, not one on every sample. Don't mistake "no repeated alerts" for "the
fault stopped happening."

A live current fault also overrides the status LED's normal pattern — in `updateOutputsFromDomain()`,
`led.setMode(Alert)` takes priority over the watering (`Breathe`) and pump-only (`SolidOn`)
patterns, so a fault is visible on the unit itself even with no MQTT client watching.

## Building and flashing over USB (first time)

1. Install [VS Code](https://code.visualstudio.com/) and open this repository as a folder. Accept
   the recommended-extensions prompt (or install everything under the Extensions panel's
   `@recommended` filter) — this pulls in the PlatformIO IDE extension, which provides the build/
   upload/monitor tooling and its own status-bar icons (checkmark to build, right-arrow to upload,
   plug icon to open the serial monitor).

   > Note: `platformio.ini` points the `esp32dev` environment at the community `pioarduino` fork of
   > the espressif32 platform (not the stock PlatformIO one) — this is deliberate, not a typo, and
   > the IDE extension handles it transparently.
2. Connect the board over USB.
3. Build and flash: either click the PlatformIO IDE's upload arrow in the status bar, or from a
   terminal:
   ```
   pio run -e esp32dev -t upload
   ```
4. Watch the boot log: `pio device monitor` (or the plug icon). A healthy first boot has no I2C
   scan failure or crash-loop, and — once connected — publishes its MQTT status/availability topics
   within about 15 seconds.

There's no separate filesystem image to upload — `config.json`/`settings.json`/`schedule.json` are
created on the device's own flash filesystem at runtime (captive portal / first commands), not
baked in at build time.

Once a unit is flashed and on your network, subsequent updates don't need USB at all — see
[ota.md](ota.md) for pushing new builds over Wi-Fi/MQTT instead.

## Testing

Everything under `src/domain/` is hardware-abstracted (see **Firmware architecture** above), which
is what makes it possible to unit-test on your own machine instead of only on real hardware —
`test/test_domain/test_domain.cpp` is a single Unity test binary covering the domain layer
(schedule validation, duration-adjustment math, current-fault thresholds, DST rules, and so on).

Run the suite:
```
pio test -e native
```
Or target just the tests relevant to what you're changing:
```
pio test -e native -f test_domain
```

`src/drivers/` code (real I2C/GPIO register access) generally can't be meaningfully unit-tested
without hardware mocks — changes there are verified by flashing to a real unit instead (see
**Building and flashing**, above), not by adding a native test.

### One-click test button in VS Code

The repo's `.vscode/tasks.json` already wires up a status-bar button for this, the same way it
does for OTA (see [ota.md](ota.md)) — using the `actboy168.tasks` extension (recommended in
`.vscode/extensions.json`) to render a task's `options.statusbar` as a clickable status-bar item:

```json
{
    "type": "shell",
    "command": "${userHome}/.platformio/penv/Scripts/platformio.exe",
    "args": ["test", "-e", "native"],
    "problemMatcher": ["$platformio"],
    "label": "PlatformIO: Test (native)",
    "options": {
        "statusbar": {
            "label": "$(beaker) Test (native)",
            "tooltip": "Run native unit tests (src/domain/) via PlatformIO"
        }
    }
}
```

This ships out of the box — reload the VS Code window after cloning the repo and a **⚗ Test
(native)** button appears in the status bar, running the whole native suite with one click, no
terminal needed. Add your own task the same way (copy the block, change the label/args) if you
want a one-click button scoped to a single test file instead of the whole suite.

## Known limitations

- Current-sensor calibration constants ship as placeholders — readings are meaningless until
  calibrated per **Current-sensor calibration** under **MQTT**, above.
- The SSD1309 U8g2 driver variant (`NONAME0` vs `NONAME2`) hasn't been verified against a real
  panel; if the display shows garbage or inverted colors on first boot, swap the constructor in
  `src/ui/IrrigatorUi.h`.
- **The pump does not actually pre-start before a valve opens.** `PUMP_PRESTART_MS` (500ms) exists
  in `board_config.h` and is referenced by some `test_domain.cpp` timing offsets, but nothing in
  `IrrigationStateMachine` or `main.ino` ever reads it — `start()` moves straight into the
  `Watering`/`PumpOnly` state in one step, and `updateOutputsFromDomain()` sets the pump and valve
  relays from that same snapshot in the same call, with no gap between them. If a real pre-start
  gap matters for your pump/valve hardware, treat this as unimplemented, not just untested.
