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
| 24VAC current sense       | GPIO 32 (ADC)           | Valve current — no fixed mA range; see **True-RMS current sampling** under **MQTT**, below |
| 230VAC current sense      | GPIO 33 (ADC)           | Pump current — no fixed mA range; see **True-RMS current sampling** under **MQTT**, below |
| Flow meter (Modbus RTU/RS-485) | TX=GPIO2, RX=GPIO36 | To a Waveshare TTL-to-RS485 (C) isolated converter (automatic DE/RE, no extra GPIO) — see [hardware/readme.md](../hardware/readme.md)'s **Water flow sensing** section for wiring, and **Water flow sensing (TUF-2000M)** under **MQTT**, below |

All of the above lives exclusively in `include/board_config.h` — it's the **only** file in the
project allowed to hardcode a GPIO number, I2C address, or similar board-specific constant. If
this firmware is ever ported to a different board revision, that file (and only that file) should
need to change. It also holds the operating constants worth knowing about: current-fault
thresholds (as multipliers of a calibrated base, not fixed mA — see **True-RMS current sampling**
below), JSON file size caps, and schedule/duration limits.

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
  - **The rotary UI can lean on it too.** `UiStateMachine::handleNormal` no longer gates rotation/
    click on `Context::wateringActive` — turning the knob while a run is already active picks a
    zone/duration exactly like the idle case, and `UiIntent::boolValue` flags that the resulting
    `UiIntentType::StartWatering` was made while watering, so `main.ino`'s `handleUiIntent` knows to
    report the push outcome back via `IrrigatorUi::showQueueAddResult()` (`UiScreen::QueueAddResult`
    — "Added to queue"/`QUEUE FULL`) instead of assuming an immediate start. This is only possible
    because every request already goes through the queue regardless of whether it can start right
    away — the UI change didn't need any new backend plumbing.
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

The standard-time UTC offset (`config.json`'s `timezone` field, a plain `-13`..`+13`) and daylight
saving are both applied in software on top of that UTC baseline, via `domain::isEuDstActive()`:

- DST is the fixed **EU rule**: clocks go forward on the last Sunday of March at 01:00 UTC, and
  back on the last Sunday of October at 01:00 UTC.
- It's deliberately evaluated in UTC (not "02:00 local time", which is how the rule is often
  phrased) — the UTC framing is what actually keeps the transition simultaneous across every EU
  country regardless of their individual standard-time offset, and it's a pure function of the
  current UTC timestamp with no extra state to track.
- This is hardcoded to the EU rule specifically — a non-EU deployment observing a different DST
  rule (or none at all) would need a different `isEuDstActive()`-equivalent function; there's no
  region setting that switches this.

### Where the offset comes from

The captive portal does **not** ask for a timezone. The offset is derived from the same ip-api.com
geolocation lookup that gates the weather auto-adjustment (`tryGeolocateIfDue()` in
`src/main.ino`), which now requests the provider's `offset` field alongside `lat`/`lon`/`city`.

`offset` is the region's *current* UTC offset in seconds with DST already folded in, so it can't be
stored as-is — `Clock` would then add the DST hour a second time every summer.
`domain::standardUtcOffsetHoursFromCurrent()` takes that hour back out by asking `isEuDstActive()`
about the moment of the lookup, yielding the plain standard offset. Because both directions use the
same EU rule, a device that resolves in July and one that resolves in January store the same value.

Consequences worth knowing:

- `tryGeolocateIfDue()` now waits for a set clock (NTP or the DS3231 fallback) before spending the
  lookup — the DST subtraction needs a trustworthy "now", and geolocation resolves only once per
  device (`Settings::location_known`), so getting it wrong would bake in a permanent one-hour
  error. It retries on the usual 5-minute cadence until the clock is up.
- Between the captive-portal save and that first successful lookup, `timezone` is `0` (UTC).
- The offset is applied to the live clock immediately and written back to `config.json`; unlike a
  `cmd/upload/config.json` this does not restart the device, since no Wi-Fi/MQTT credential
  changed.
- Existing devices are untouched: they already have `location_known: true`, so geolocation never
  re-runs and whatever offset they were configured with stays.
- The manual override, for the cases the EU-rule conversion can't express — a non-EU DST rule, or a
  sub-hour zone like `+05:30`, which truncates toward zero — is uploading a `config.json` with an
  explicit `timezone`. `mergeConfig()` deliberately still honours that field, and it sticks for the
  same reason: geolocation won't run again.

## MQTT

Every topic is prefixed `irrigator<xx>`, where `<xx>` is the last two hex digits of the device's
MAC address (e.g. `irrigatorF4`, from `makeDevicePrefix()` in `src/main.ino`). Substitute your
device's actual prefix in every example below.

### Command/response

|  Command topic | Command msg | Response topic | Response msg | Comment |
|--|--|--|--|--|
| `/cmd/state` | "" | `/state` | `{"datetime":"2026-07-12T06:00:00",`<br>`"zone":3,"name":"Back Lawn",`<br>`"on":true,"pump":true,`<br>`"reason":"manual","scheduled":true,"remaining_min":12,"volume":0.00}` | On every start/stop of a valve or pump, and on demand via `cmd/state` (reason is one of manual/schedule/mqtt/abort; zone:0 means pump-only; scheduled reflects whether the schedule engine is currently active/paused; remaining_min is minutes left in the current run, 0 when off; volume is the just-completed run's total liters, but only in the specific message where `on` transitions to `false` for that run — 0 in every other message, see `FlowRunVolumeTracker` under **Water flow sensing (TUF-2000M)** below). Also fires the instant the valve closes even while the pump is still in its graceful-shutdown tail (`on:false,pump:true`), so `remaining_min`/`on` track the requested duration, not the pump's extra runtime. |
| `/cmd/queue` | "" | `/queue` | list of waiting waterings from the queue | On every push and pull from watering queue or by `cmd/queue` request. |
| `/cmd/run` | `{"zone":3,"duration":15}` |  |  | **No special response** Response will be either adding to the queue or alert. Starts zone 3 for 15 min (or queues it if something's already running). `zone:0` = pump only. A **disabled** zone (`zone_names.<n>.enabled: false` in `settings.json`) is rejected outright with an `/alert` — `{"message":"Zone 3 (Back Lawn) is disabled. Enable it in settings first."}` — it's never silently ignored, and it never reaches the watering queue at all (see `enqueueOrStart()` in `src/main.ino`). |
| `/cmd/stop` | empty, or `all`  |  |  | **No special response** Response will be either adding to the queue or alert. Stops the current run — closes the valve immediately, but a zone run's pump still gets its normal graceful-shutdown tail rather than cutting immediately (a pump-only run has no such tail). Plain/empty payload also pops and starts the next queued item, if any; `all` also empties the queue. |
| `/cmd/scheduled` | bool | `/scheduled` | bool | Turn the schedule on/off, write the schedule state in settings.json if changed, report back true/false. |
| `/cmd/auto_adjustment` | bool | `/auto_adjustment` | bool | Master on/off for weather-based auto-adjustment (both the daily percentage recalculation and the rain-skip check) — persisted to `settings.json`'s `auto_adjustment_enabled`, reported back true/false. |
| `/cmd/watering_percentage` | integer | `/watering_percentage` | integer | Sets the schedule-wide watering multiplier (0–300%; `0` from the auto-adjustment pipeline means "skip today's cycle entirely", not "water at 0%" — see **Weather-based irrigation auto-adjustment** below). **Retained message** — see **Wi-Fi/MQTT connection resilience** below for why. |
| `/cmd/skip_rain_until` | `"2026-07-12T06:00:00"` |  `/skip_rain_until` | `"2026-07-12T06:00:00"` | Suppresses **scheduled** runs until this UTC timestamp (manual/MQTT runs are never affected — this only gates the schedule engine). |
|  |  | `/online` |  boolean (LWT on ungraceful disconnect, or explicitly before a deliberate restart). | **Retained message**. Matches Home Assistant's default MQTT `availability_topic` convention. |
| `/cmd/status` | "" | `/status`  | JSON: `datetime`, `firmware_version`, `boot_reason`, `ip`, `uptime_s`, `rssi_db`, `wifi_quality` (verbal, `miserable`/`poor`/`fair`/`good`/`excellent`), `free_heap_bytes`, `free_storage_bytes`, `mqtt_buffer_bytes`, `cpu_temp_c`, `coredump_available` (read together with `boot_reason` — see [coredump.md](coredump.md)), `publish_failures`, `publish_retried`, `publish_offline`, `mqtt_forced_reconnects` (cumulative since boot — see **Wi-Fi/MQTT connection resilience** below), and the weather auto-adjustment block: `auto_adjustment_enabled`, `location_known`, `city` (only once known), `weather_adjust_status` (`disabled`/`no_location`/`no_clock`/`done_today`/`waiting_for_window`/`no_schedule_waiting_recheck`/`fetch_in_progress`/`waiting_retry` — why the daily cycle has or hasn't run), `watering_percentage` (what the schedule is running at right now; `0` = today's cycle is skipped) and `last_weather_update` (omitted until this session's first successful fetch). No `outputs`/current section — which zone/pump is on is `/state`'s job (fired on every start/stop, and on `cmd/state`), and current readings only have a meaningful number outside `cmd/calibrate`'s response as a comparison against a calibrated base; see **True-RMS current sampling** below. | ~15s after boot and on request.  |
| `/cmd/weather` | "" | `/status`, `/watering_percentage`, `/skip_rain_until`, `/alert` | Manual test trigger for the whole weather auto-adjustment pipeline: runs the fetch + rain-skip check + percentage recalculation right now, bypassing the schedule-today / lead-time / retry-pacing gates. When the fetch finishes it always reports back with a `/status` (success or failure), and on success always republishes `/watering_percentage` — even when the value is unchanged, unlike the automatic daily cycle, which only publishes on change — or publishes `/skip_rain_until` if the forecast triggered a rain skip. On failure it also sends the `{"weather":"failed",...}` `/alert` regardless of the once-a-day cap. Rejected with an `/alert` if a fetch is already running, location isn't known yet or the clock isn't synced. | Test-only; the daily cycle needs no command. |
|  |  | `/alert` | see **Error reporting** below | Fire-and-forget notices: current out of expected range, watering queue full, OTA result, and any command/topic error. Not retained, not queued for delivery guarantees. Every alert carries a `datetime` field — JSON alerts get it spliced in alongside their other keys, plain-text alerts are wrapped as `{"datetime":"...","message":"..."}`. |
|  |  | `/flow` | JSON: `flow_rate_lpm`, `total_liters`, `zone` (whichever zone/pump is currently running, 0 = idle). | Raw TUF-2000M flow-meter telemetry — see **Water flow sensing (TUF-2000M)** below for the publish-cadence rule (not every Modbus poll goes out). Not retained. |
|  |  | `/flow_totals` | JSON: one `zone_<n>` field (liters) per **enabled** zone, plus `other`. | Lifetime (since-boot) water consumption per zone, plus "other" (flow while every valve is closed). Roughly once a minute — see **Water flow sensing (TUF-2000M)** below. Not retained. |
| `/cmd/restart` | "" |  |  | **No special response** Publishes `false` to `online`, then restarts. |
| `/cmd/ota` | FW `.bin` URL | `/ota_started` then `/ota_completed` | `"true"`, then `"2026-07-12T06:00:00"` |  `/ota_started` fires the instant the command is accepted (before the HTTP GET), separately from success/failure - `cmd/#` is only delivered at QoS 0, so a caller can't tell "broker accepted my publish" from "device actually received it" without a distinct receipt signal (see [ota.md](ota.md)'s retry logic, built on exactly this). Downloads and flashes the URL, restarts on success. Blocking by design — see [agents.md](../agents.md) for why. See [ota.md](ota.md) for the higher-level tooling built on top of this. |
| `/cmd/calibrate` | "" | `/calibrate` (one message per sensor) | JSON: `sensor` (`"valve"`/`"pump"`), `base_level`, `sample_count`, `burst_samples_mv` (the most recent burst's raw ADC readings, for spotting saturation). | Requires an active watering run (valve open — see **True-RMS current sampling** below); rejected with `/alert` otherwise. |
| `/cmd/meter` | "" | `/meter` | JSON: `flow_rate_lpm`, `total_m3`, `signal_strength_up`/`signal_strength_down` (regs 93/94), `quality` (reg 92 low byte, 0–100), `autogain_step` (reg 92 high byte), `sound_speed_ms` (regs 7/8), `reading_age_s`, `last_modbus_error` (numeric `Modbus::Error`, `0` = none since boot) and `last_modbus_error_text`. | Flow-meter diagnostics, answered from the latest valid poll (no extra Modbus transaction). Before any valid poll exists the reading fields are replaced by `"error":"no valid reading yet"`; the two error fields are always present. The error is sticky — a later good poll doesn't clear it. See **Water flow sensing (TUF-2000M)** below. |
| `/cmd/download/<filename>` | "" | `/download/<filename>` | String | Reads that file from flash and publishes it. |
| `/cmd/coredump` | "" | `/coredump` | JSON: `coredump` (`"present"`/`"none"`), `size_bytes`, `task`, `panic_reason`, `pc`, `exc_cause`, `fault_addr`, `elf_sha256`, `bt_corrupted`, `backtrace` (array of `"0x...."` strings). | Summary of the crash dump the panic handler stored in flash, without transferring the image. Also published unprompted ~15s after boot when a dump is present, alongside `/status`. Decode the addresses against the exact `firmware.elf` named by `elf_sha256` — see [coredump.md](coredump.md). |
| `/cmd/download/coredump` | "" | `/download/coredump` (a header, then N chunks, then a trailer) | `{"coredump":"begin","size":…,"chunks":N,"chunk_bytes":3072,"encoding":"base64","summary":{…}}`, then `{"seq":i,"data":"<base64>"}` for i=1..N, then `{"coredump":"end","chunks":N,"size":…,"crc32":"0x…"}` | Not a file on SPIFFS — the raw ELF image out of the `coredump` partition, up to 64KB, so it streams as base64 chunks one per `loop()` iteration rather than in one message. All QoS 0: a dropped chunk shows as a gap in `seq`, a corrupted one fails the trailer's CRC32; either way just re-request it. `scripts/coredump.py` does the whole round-trip and decode. |
| `/cmd/coredump_clear` | `true` |  |  | **No special response, alert either way.** Erases the stored dump. Payload must be the literal string `true`, same confirmation gate as `cmd/delete`. Never happens automatically after a download — see [coredump.md](coredump.md) for why. |
| `/cmd/upload/<filename>` | new file content |  |  | Writes that file to flash. |
| `/cmd/delete/<filename>` | `true` |  |  | **No special response, alert either way.** Deletes that file (and its `.bak`, if any) from flash. Payload must be the literal string `true` — anything else is rejected outright as a missing confirmation, not treated as a no-op. See **JSON file formats** below for why you'd want this. |

#### Error reporting

Every command above rejects malformed or out-of-range input rather than guessing at intent
(e.g. clamping a duration, defaulting a missing field) — invalid input is reported, not
silently dropped or coerced. A malformed payload, an out-of-range value, an unrecognized
`cmd/<name>`, or a `download` for a file that doesn't exist all publish to `irrigatorF4/alert`
(each also carries a `datetime` field, omitted below for brevity):

```json
{"command":"run","error":"duration out of range (1-120)"}
{"command":"calibrate","error":"Make sure the valve is open - better if the pump is running too."}
{"command":"calibrate","sensor":"valve","error":"reading too low (12); check wiring or that the relay is actually energizing"}
{"command":"nonexistent_command","error":"unknown command"}
{"download":"missing.json","error":"not found"}
{"download":"settings.json","error":"failed to read"}
{"upload":"schedule.json","error":"invalid schedule.json content"}
{"upload":"schedule.json","error":"failed to write"}
{"delete":"missing.json","error":"not found"}
{"delete":"settings.json","error":"payload must be 'true' to confirm deletion"}
{"download":"coredump","error":"no valid core dump stored"}
{"download":"coredump","error":"a transfer is already in progress"}
{"command":"coredump_clear","error":"payload must be 'true' to confirm erase"}
```

A `settings.json`/`config.json` upload that otherwise succeeds but had one or more fields
rejected (see **JSON file formats** below) raises a softer `alert` instead of an `error`, since the
rest of the upload still went through:
```json
{"upload":"settings.json","alert":"invalid value(s) ignored, kept previous: watering_percentage"}
```

A successful `delete` raises an informational `alert` too, since there's no dedicated response
topic to confirm it otherwise:
```json
{"delete":"settings.json","info":"file deleted"}
```

### Wi-Fi/MQTT connection resilience

Before 2026-08-29 the device would occasionally go dark for minutes at a time — `/state` on a
watering start silently missing, or `/status`/`/watering_percentage` failing to publish. (A
separate, harder fault — `boot_reason: "other_watchdog"`, a hardware watchdog reset around a relay
transition that bypassed the panic handler and left no core dump — had already gone quiet after an
earlier round of Wi-Fi power-save/TX-power tuning in `connectWifiOnce()`, and is not what this
section is about; see [coredump.md](coredump.md) for how `boot_reason`/`coredump_available` work in
general.) Since the cluster of fixes below landed together on 2026-08-29, the device has held one
continuous MQTT session for days at a stretch with zero silent drops. What actually changed:

- **`wifiClient_.setNoDelay(true)`** (`MqttService::onConnected()`), disabling Nagle's algorithm on
  the MQTT `WiFiClient`. Nagle holds a small outgoing segment while a previous one is still
  unacknowledged, which — paired with the broker's own delayed-ACK behavior — could park a
  ~150-byte `/state` in the send buffer for up to ~200ms. `startIrrigation()` publishes `/state` and
  energizes the relay a few milliseconds later in the same `loop()` iteration, squarely inside that
  window: the publish would report success (the bytes were handed to the socket) while the segment
  itself was still sitting unsent when the moment it described had already passed. Disabling Nagle
  pushes each publish out on its own TCP segment immediately.

- **Retry + forced reconnect on a failed publish** (`MqttService::publishRaw()` /
  `forceReconnect()`). `PubSubClient::publish()` returns `false` for exactly three reasons: not
  connected, payload larger than the buffer, or the underlying socket write coming up short. On this
  device it was always the third — a momentary radio/TX-queue condition, not a dead link — so a
  failed publish is now retried up to `kPublishAttempts` (3) times, `kPublishRetryDelayMs` (5ms)
  apart, before being treated as a real failure.
  <br>The more important half: PubSubClient does **not** notice a dead socket on its own. A failed
  `write()` leaves `_state` at `MQTT_CONNECTED` and the file descriptor open, so `connected()` keeps
  answering `true` while every subsequent publish — and every inbound `cmd/*` the broker tries to
  deliver — goes into a socket that leads nowhere. The only thing that used to notice was the
  keepalive: the client sends its first `PINGREQ` only after `kMqttKeepAliveSec` (200s) of silence
  and gives up after twice that, while the broker's own `1.5×keepalive` rule (300s) fires the LWT
  first. Measured on this device: a failed publish was the last thing to reach the broker, `/online`
  flipped to `false` exactly 300s later, and the device only reconnected after that — a 5-minute
  outage from one dropped write. Once every retry is exhausted, `forceReconnect()` now calls
  `mqttClient_.disconnect()` and resets the reconnect timer so `MqttService::loop()` rebuilds the
  session on its very next pass — collapsing that ~300-400s recovery to about one second.

- **Paced connect-time seed publishes** (`MqttService::kConnectSeedSpacingMs`, 20ms). On every
  (re)connect the device fires `/online`, then (from `main.ino`'s `ConnectedHandler`) `/scheduled`,
  `/auto_adjustment`, and `/watering_percentage` — four small writes back-to-back into a TCP socket
  that is only milliseconds old. Every publish failure this device ever reported was the *last*
  message of that burst. A 20ms gap between each publish (~80ms total, once per connection) moves
  the burst out of the socket's just-opened, still-settling window.

- **`/watering_percentage` is now retained.** It carries the auto-adjustment pipeline's most
  consequential number — `0` means "skip today's watering entirely," not "water at 0%" — and it was
  repeatedly the exact message lost to the burst above. Retaining it means a dropped publish no
  longer erases that information from the broker's point of view; the next subscriber (or an HA
  restart) still sees the last value the device actually sent, and it self-corrects on the next
  successful publish. (The original reason this was *not* retained — a stale value surviving past a
  device restart — no longer applies: every one of these entities is gated in Home Assistant on the
  retained `/online`, which the LWT drives to `false` whenever the device is actually down, so a
  retained value can't be shown as live while the device is offline.)

- **Bounded HTTP/TLS timeouts** on all three outbound HTTPS calls (`WeatherService.cpp`: Open-Meteo,
  WeatherAPI, and `ip-api.com` geolocation). `HTTPClient`'s own connect timeout does **not** cover
  the mbedTLS handshake — a stalled handshake was observed blocking the weather task for 70 seconds,
  holding its ~60KB of TLS buffers the entire time. `client.setHandshakeTimeout(TLS_HANDSHAKE_TIMEOUT_S)`
  (10s) plus `http.setConnectTimeout()`/`setTimeout()` (`HTTP_CONNECT_TIMEOUT_MS`/
  `HTTP_RESPONSE_TIMEOUT_MS`, 8s each — see `board_config.h`) cap the worst case per provider at
  ~10s. This doesn't touch MQTT directly, but a background task blocked for over a minute delays
  `loop()`'s own MQTT servicing indirectly and was worth closing at the same time.

- **Four new health counters**, cumulative since boot and surfaced in `/status` (see the
  Command/response table above): `publish_failures` (every retry exhausted — each one also forced a
  reconnect), `publish_retried` (failed at least once, then succeeded — the near-misses, and the
  earliest sign the link is degrading, worth watching *before* `publish_failures` climbs),
  `publish_offline` (a publish attempted while `!connected()` — previously silent), and
  `mqtt_forced_reconnects` (sessions torn down by the mechanism above rather than by the keepalive).
  Read these against `rssi_db` — a climbing count on a device with poor signal is the RF link
  telling you something; the same count on a device with good signal points elsewhere.

None of this is a claim that the underlying link itself got better — RSSI at the device's location
is unchanged (-68 to -72dBm, "fair"). What changed is that a **transient** write failure — which the
data shows self-heals within milliseconds — no longer gets escalated into a **multi-minute** outage
by machinery (Nagle's buffering, the keepalive's slow death-detection) that was built for a
different failure shape than the one this device actually has.

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
above) are always read back as a whole file via `download`. `upload` behaves differently per file:

- **`settings.json`/`config.json`** are merged field by field onto the device's current values
  (see `mergeSettings()`/`mergeConfig()` in `src/domain/Settings.cpp`/`ConfigStore.cpp`): a field
  present in the upload with a non-null, non-empty value is validated on its own and applied only
  if it individually makes sense; a field that's missing, `null`, an empty string, or that fails
  its own check (wrong type, e.g. `-1` for a boolean, or out of range, e.g. `300` for a setting
  clamped at 200) instead keeps whatever was already there — the latter case also raises an
  `/alert` naming the rejected field(s) by its dotted group path (e.g. `Watering.watering_percentage`,
  `Current_sensor.alert_pump_uc`), so the rest of the upload isn't silently dropped without a
  trace. `Zones` is merged per zone index, so naming only zone 3 doesn't disturb the others. This
  means you can push just the field(s) you want changed instead of round-tripping the whole file —
  though each field must still be nested under its group (see the schema below), since
  `mergeSettings()` only reads the new nested paths, not the pre-restructuring flat ones.
- **`schedule.json`** keeps whole-file replace semantics: it's a JSON array of records, not a flat
  object of named fields, so any single invalid record, more than `SCHEDULE_MAX_RECORDS` (255)
  records, or a payload that isn't a JSON array at all rejects the entire upload with an `/alert`,
  same as loading it from flash at boot. The number of records can still grow or shrink freely
  between uploads, up to that cap.
- Any other filename (JSON or not) is written to flash exactly as received — there's no schema to
  validate it against.

Because `settings.json`/`config.json` uploads only ever add or update a field — never remove
one — a stale key from a since-abandoned JSON shape (e.g. the flat-to-nested `Zones` restructuring)
or an oversized `Zones`/`schedule.json` list can't be trimmed down through `upload` alone. Use
`/cmd/delete/<filename>` (payload `true`) to remove the file outright first, then either upload a
clean replacement or let it regenerate with firmware defaults on the next boot (`settings.json`
always regenerates; `config.json`'s absence instead re-enters captive portal mode; a missing
`schedule.json` is simply treated as empty). Deleting takes effect only on the next boot/upload —
the in-RAM `deviceConfig`/`settings`/`scheduleList` stay live until then, so deleting `config.json`
remotely won't drop your current MQTT session, but the *next* reboot will need the captive portal
again since there's no Wi-Fi config left to load.

**`settings.json`** (see `src/domain/Settings.h`/`.cpp`) is nested into four top-level groups —
`Zones`, `Location`, `Current_sensor`, `Watering`:
```json
{
  "Zones": {
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
  },
  "Location": {
    "location_known": false,
    "latitude": 0,
    "longitude": 0,
    "city": ""
  },
  "Current_sensor": {
    "valve_current_base_level": 0,
    "pump_current_base_level": 0,
    "alert_pump_uc": true,
    "alert_pump_oc": true,
    "alert_valve_uc": true,
    "alert_valve_oc": true
  },
  "Watering": {
    "watering_percentage": 100,
    "schedule_active": false,
    "default_manual_duration": 5,
    "auto_adjustment_enabled": true,
    "upper_limit": 200,
    "lower_limit": 40,
    "rain_skip_probability_percent": 70,
    "rain_skip_amount_mm": 4,
    "weatherapi_key": ""
  }
}
```
`Location.*` and `Current_sensor.valve_current_base_level`/`pump_current_base_level` are
device-owned (see below); the 4 `Current_sensor.alert_*` toggles are not — they gate **Current-fault
alerting** below and are editable both from HA and from the device's own Settings menu (`Alert
pump/valve UC/OC`, directly above `Calibrate CS`). `parseSettings()` (the boot-time flash load)
additionally falls back to each field's pre-restructuring flat top-level key when its new nested
key is absent, so a device upgrading from an older firmware keeps its real settings instead of
resetting to factory defaults — `mergeSettings()` (MQTT uploads) does not have this fallback, it
reads only the nested paths shown above.

**`schedule.json`** — a flat array of records, up to `SCHEDULE_MAX_RECORDS` (255), `day_of_week`
0-indexed Monday=0..Sunday=6 (see `src/domain/Schedule.h`/`.cpp`):
```json
[
  {"zone": 1, "day_of_week": 2, "start_time": "06:00", "duration_min": 30, "active": true},
  {"zone": 8, "day_of_week": 2, "start_time": "06:30", "duration_min": 30, "active": true},
  {"zone": 2, "day_of_week": 3, "start_time": "06:00", "duration_min": 9, "active": true},
  {"zone": 3, "day_of_week": 3, "start_time": "06:09", "duration_min": 9, "active": true}
]
```

**`config.json`** (Wi-Fi/MQTT, written by the captive portal; `timezone` is filled in later by
geolocation rather than by the form — see `src/domain/ConfigStore.h`/`.cpp` and "Clock sync and
daylight saving" above):
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

### True-RMS current sampling

A raw AC current-sensor signal read once is close to meaningless without either analog
rectification/smoothing on the sensor board itself or proper RMS math in firmware — a lone ADC
sample just catches a random point on the 50/60Hz waveform. There is also no mA conversion
anywhere in this design — readings are never turned into a real current value, only compared
against a per-sensor reference level captured under real load (`cmd/calibrate`, below). A
dedicated background FreeRTOS task (`currentSamplingTask` in `src/main.ino`) does the sampling:

1. **Burst**: `CurrentSensor::sampleRmsMv()` (`src/drivers/CurrentSensor.cpp`) takes
   `CURRENT_RMS_BURST_DURATION_MS` (100ms) worth of raw ADC samples spaced
   `CURRENT_RMS_SAMPLE_INTERVAL_MS` (1ms, ~1kHz) apart — several full mains cycles. The zero-line
   isn't a fixed board constant: it's `(min+max)/2` of *that same burst's* own samples — for a
   periodic, roughly-symmetric AC waveform this midpoint approximates the true DC center, so it
   self-adjusts for bias drift between sensor units or with temperature instead of relying on a
   calibrated offset. The burst's result is `sqrt(mean((raw_mV - center_mV)^2))` — a virtual
   reference level in raw millivolts, not amps. Per-sample deviations are *not* clamped to zero — a
   bipolar AC signal needs both polarities to square into a valid RMS.
2. **Cadence**: one burst per sensor every `CURRENT_RMS_BURST_PERIOD_MS` (5s), for as long as a
   valve or pump is on, plus a `CURRENT_MEASURE_POST_OFF_MS` (10s) tail after it turns off — not
   just while nominally "on" — which also catches a valve that fails to fully close.
3. **Handoff**: each burst (RMS values for both sensors, the zone the valve reading should be
   attributed to, and both sensors' raw samples) is pushed onto `currentBurstResultQueue` for
   `loop()` to drain (`drainCurrentBurstResults()`) — same producer/consumer split as
   `weatherAutoAdjustTask`/`weatherResultQueue`. Settings, MQTT, and persisted state are only ever
   touched from that drain, never from the task itself.

The task is idle — blocked on a FreeRTOS task notification, not polling — whenever nothing is
running, and is woken exactly once per on-window transition. It has to live in its own task rather
than `loop()` or a `Ticker` callback specifically because both burst calls block for real (~100ms
each): [agents.md](../agents.md)'s Ticker-callback-discipline and no-blocking-in-loop rules both
rule that out anywhere but a dedicated task.

`/status` doesn't report current at all — see its row in **Command/response** above. The only way
to see a reading is `cmd/calibrate`, next.

### cmd/calibrate

Also reachable from the device itself, with no MQTT client needed: the rotary UI's Settings menu
has a **Calibrate CS** item (`UiStateMachine::kSettingsIndexCalibrate`, right before Restart) that
asks for the usual YES/NO confirmation, then runs the exact same `handleCalibrateCommand()` logic below
— including the same rejections — and shows the outcome on the SSD1309 (each sensor's new base
level, or its short rejection reason) with an OK button to dismiss back to the normal screen. See
`src/ui/IrrigatorUi.cpp`'s `renderCalibrateConfirm()`/`renderCalibrateResult()`.

`/cmd/calibrate` captures each sensor's **base** — the reference level everything else in this
section compares against — as the average of its last `CURRENT_RMS_HISTORY_LEN` (12) bursts, a
trailing ~1 minute at the 5s burst cadence (`CurrentRmsHistory` in `main.ino`, fed by
`drainCurrentBurstResults()`). It requires an actual watering run — `IrrigationState::Watering`,
not just the pump alone — so the captured base reflects a real loaded valve+pump, not a dry/idle
pump-only reading; otherwise it's rejected with `/alert`:
```json
{"command":"calibrate","error":"Make sure the valve is open - better if the pump is running too."}
```

Per sensor, the captured average is also rejected (nothing is saved) if:
- **No bursts yet** — the run just started and the history is still empty.
- **Reading below `CURRENT_CALIBRATION_MIN_BASE_LEVEL`** (30, placeholder — needs tuning against
  real hardware, see `board_config.h`) — a real valve/pump under load should read well above ADC
  noise, so a near-zero average usually means the output wasn't actually drawing current (wiring
  fault, relay not energizing), not a genuinely low-draw device. Silently accepting it would mean
  every future burst reads as "over 2.9x this near-zero base" — this is agents.md's fail-loud rule,
  not a guess dressed up as a calibration.

Either rejection publishes `/alert` with the reason instead of saving:
```json
{"command":"calibrate","sensor":"valve","error":"reading too low (12); check wiring or that the relay is actually energizing"}
```

On success, the base is saved to `settings.json` (`valve_current_base_level`/
`pump_current_base_level` — device-owned, like `location_known`/`latitude`/`longitude`/`city`: only
`cmd/calibrate` ever writes them, never an uploaded `settings.json`, so HA's full-settings-replace
flow can't clobber a calibration) and a response is published **per sensor** (two separate
messages, not one combined payload — keeps each comfortably under the MQTT buffer size, since the
raw-samples array alone can be ~100 entries):
```
mosquitto_pub -t irrigatorF4/cmd/calibrate -m ""
```
```json
{"sensor":"valve","base_level":734,"sample_count":12,"burst_samples_mv":[1652,1889,2041,...]}
{"sensor":"pump","base_level":612,"sample_count":12,"burst_samples_mv":[1648,1701,1810,...]}
```
`burst_samples_mv` is the most recent burst's raw ADC readings (not deviations) — inspect it to
tell a genuinely low reading apart from one where the signal is clipping against the ADC's rail
(saturation), which `base_level` alone can't distinguish.

Re-running `cmd/calibrate` overwrites the previous base — there's no separate "reset calibration"
command; recapture is how you update it. The valve sensor is shared across all 11 zones (one
physical sensor), but because only one zone is ever open at a time, every burst is unambiguously
attributable to whichever zone was active — see **Current-fault alerting** below for how that
attribution is used.

### Current-fault alerting

Once a sensor has a calibrated base (0 = never calibrated = no check), `drainCurrentBurstResults()`
compares *every burst* — not a windowed average — against it:
- **Overcurrent** (valve and pump): reading > `base * CURRENT_OVERCURRENT_MULTIPLIER` (2.9).
- **Undercurrent** (valve and pump): reading < `base * CURRENT_VALVE_UNDERCURRENT_MULTIPLIER` /
  `CURRENT_PUMP_UNDERCURRENT_MULTIPLIER` (both 0.5).

Each of these four checks is additionally gated by its own `settings.json` toggle
(`Current_sensor.alert_pump_uc`/`alert_pump_oc`/`alert_valve_uc`/`alert_valve_oc` — see
`Settings::alertPumpUc`/etc., default `true`) on top of the base-calibrated (`!= 0`) gate — turning
a toggle off silences that specific check even while the sensor stays calibrated. Editable from HA
(the "Current-Sensor Calibration" card) or the device's own Settings menu (`Alert pump/valve
UC/OC`, directly above `Calibrate CS`).

A trip publishes `/alert`:
```json
{"valve":3,"fault":"overcurrent","level":950,"base":700}
{"pump":0,"fault":"overcurrent","level":1400,"base":1000}
{"pump":0,"fault":"undercurrent","level":320,"base":1000}
```

Each fault is gated to **one `/alert` per calendar day**, not one per bad burst — a valve stuck
out-of-range for an entire 15-minute run raises exactly one alert, not one every 5 seconds
(`consumeDailyAlertGate()`, same idiom as `lastWeatherFailureAlertDate`/`lastGeoFailureAlertDate`
elsewhere in `main.ino`). Don't mistake "no repeated alerts" for "the fault stopped happening." The
valve's dedup is **per-zone** (`lastValveOvercurrentAlertDate`/`lastValveUndercurrentAlertDate`,
indexed by zone), not global for the sensor — since every burst is attributable to a specific zone
(see **cmd/calibrate** above), a stuck zone 3 today doesn't silence a separately failing zone 5
later the same day. The pump has no zone dimension, so its dedup (both directions) is a single
shared date per direction.

This is MQTT-only on the device side — unlike the earlier per-output fault monitor this design
replaces, it doesn't drive the status LED's alert pattern. The HA package (see
`homeassistant/packages/irrigator_automations.yaml`'s "Irrigator - forward current-fault alerts to
WhatsApp + logbook" automation) forwards every fault alert (identified by the `fault` key) to
`notify.greenapi` and the logbook.

### Water flow sensing (TUF-2000M)

An ultrasonic clamp-on flow meter (TUF-2000M) on Modbus RTU/RS-485 — see
[hardware/readme.md](../hardware/readme.md)'s **Water flow sensing** section for the wiring/converter
and the failsafe reasoning (the meter's own onboard relay, not this firmware, is what actually closes
a shut-off valve; polling here is for HA visibility/logging only). `src/drivers/Tuf2000mFlowMeter.*`
is the driver, built on the [eModbus](https://github.com/eModbus/eModbus) library rather than a
blocking one — its `ModbusClientRTU` runs its own FreeRTOS worker task internally and delivers
results via callback, so unlike **True-RMS current sampling** above, no extra task needed to be
created for this feature.

**Register map** (function code 03, holding registers — per the register map documented by
[github.com/tczerwonka/tuf2000-modbus-reader](https://github.com/tczerwonka/tuf2000-modbus-reader),
cross-checked during bench bring-up): register 1 (2 words) is flow rate in m³/h, register 9 (2 words)
is the positive totalizer in m³ — both REAL4/IEEE-754 floats, word-swapped before being reinterpreted
as a float (`irrigator::domain::decodeSwappedFloat()`, `src/domain/Tuf2000mProtocol.*` — pulled out of
the driver itself so the decode and the poll-sequencing state machine are unit-testable under
`pio test -e native` without any Modbus hardware or mocking) — register 7 (2 words, same REAL4
encoding) is the fluid sound speed in m/s, and registers 92–94 (3 words, one request) are signal
quality (low byte quality, high byte autogain step) followed by upstream/downstream signal strength.
Sound speed and the signal block only feed `cmd/meter`. Flow rate and totalizer are multiplied by
`METER_CALIBRATION_MULTIPLIER` (`board_config.h`, currently 1.4 — this meter's calibration is off and
can't be fixed on the unit) inside the driver, so every published/derived figure is already corrected;
sound speed and signal values are raw. All four requests are polled together every
`FLOW_METER_POLL_INTERVAL_MS` as separate Modbus requests per poll cycle (never overlapped with the next poll's — see the driver's class comment for
why: the reference project above found "the modbus stuff just didn't give back consistent data for
two sequential queries").

**MQTT publish cadence** is independent of the Modbus poll cadence above — every poll's result is a
candidate for `/flow`, but not every one is published (`updateFlowPublishCadence()` in `main.ino`,
backed by `irrigator::domain::FlowPublishGate`):
- While a zone or pump is active: once a minute.
- While idle: threshold-triggered rather than timer-driven — once accumulated idle-time volume has
  grown past 1 liter since the last idle publish, capped to never fire more than once a minute even
  if that threshold keeps getting crossed faster. A real leak surfaces promptly; a quiet pipe
  generates no idle MQTT traffic at all.

**A failed poll** (Modbus error/timeout on any of the four requests — `Reading::valid == false`)
publishes `/alert` with `{"flow_meter":"failed","reason":"modbus_error_or_timeout"}`, gated to once
per calendar day the same way the current-sensor faults are (`consumeDailyAlertGate()` in
`main.ino`). Deliberately not a `{"fault":...}` payload — there's no zone/level/base to report for a
communication failure, so it doesn't match (and isn't forwarded to WhatsApp by)
`irrigator_automations.yaml`'s fault-forwarding automation; it's the same category as
`reportWeatherFailure()`'s `{"geolocation"/"weather":"failed",...}` alerts. This alerts on the link
itself going quiet, which is a different, lower-level thing than the anomaly detection (flow rate
out of an expected band while watering) planned as a later stage — not implemented yet.

**Per-run and lifetime volume** are tracked by `irrigator::domain::FlowConsumptionTracker.h/.cpp`'s
two pure classes (pulled out of `main.ino` for the same `pio test -e native` reason as
`Tuf2000mPollTracker`/`FlowPublishGate`):
- `FlowRunVolumeTracker` captures the totalizer reading at the moment a run starts
  (`startIrrigation()`) and computes+clears it exactly once — the completed run's own liters — at
  the moment that run's watering actually ends (`reportValveClosed()` for a zone+pump run,
  `reportStop()` for a pump-only one). This is `/state`'s `volume` field: `0` while a zone/pump is
  running or fully idle, the real number only in the one `/state` message where `on` transitions to
  `false` for that run.
- `FlowLifetimeCounters` maintains a never-reset (RAM-only) liters counter per zone plus one for
  "other" (flow while every valve is closed — a hose bib spliced into the line, a leak — not a
  second whole-house meter; a pump-only run or the post-valve-close pump-graceful-shutdown tail
  falls into neither bucket and is discarded, see the class comment), fed by the totalizer delta on
  every valid poll and attributed by `irrigation.snapshot(nowMs)` at that moment. Published on
  `/flow_totals` (one field per **enabled** zone plus `other`) roughly once a minute — feeds the HA
  "Water Consumption" dashboard group's `total_increasing` sensors, whose long-term statistics
  tolerate this counter resetting to 0 on a device reboot without miscounting it as negative
  consumption.

`settings.json`'s `FlowMeter` group (`Settings::flowMeterEnabled`/`expectedFlowRateLpm`) holds the
anomaly check's on/off switch and its expected-flow-rate threshold (0 = not configured, no check) —
see [usage_homeassistant.md](usage_homeassistant.md)'s **Water Flow Sensing** section for how it's
edited. **The anomaly check itself (comparing live flow rate against this threshold while watering
and publishing `flow_high`/`flow_low` faults) is not implemented in firmware yet** — the settings
surface above is built ahead of it deliberately, so wiring the check up later is a `main.ino`-only
change with no further settings/HA/doc rework. The end-user docs already describe that alert as if
it exists (matching the settings/threshold that already do) — keep that in mind if you're
implementing it: it needs to actually match what's already documented there, not the other way
around.

## Weather-based irrigation auto-adjustment

Pure calculation lives in `src/domain/AutoAdjustment.h`/`.cpp` and `src/domain/WeatherSummary.h`/
`.cpp` (both unit-tested, see **Testing** below); the HTTP/JSON fetching lives in
`src/network/WeatherService.h`/`.cpp` (not unit-testable — it's a live HTTP client, verified by
flashing); orchestration (retry pacing, the daily trigger window, rain-skip vs. percentage
decision) lives in `src/main.ino`'s `tryGeolocateIfDue()`/`runAutoAdjustmentIfDue()`.

The feature has exactly one forward-looking input (the rain-skip forecast, Step 1) and two
backward-looking inputs (the adjustment formula's temperature and rainfall, Step 2) — it's easy to
assume both steps use the forecast, so `network::WeatherFetchResult`'s field names spell out the
direction explicitly: `avgPastTempC`/`rainPastDaysMm` (historical) vs.
`maxRainProbabilityPercent`/`rainAmountNextWindowMm` (forecast).

### Data sources

1. **Location** (`GET http://ip-api.com/json/?fields=status,lat,lon,city,offset`) — fetched once,
   only while `settings.json`'s `location_known` is `false` *and* the clock has been set, retried
   every 5 minutes until it succeeds. Scheduled watering uses the manual `watering_percentage`
   unmodified until this resolves; there's no way to enter a location manually (see **Known
   limitations**). `offset` is what sets `config.json`'s `timezone` — see "Clock sync and daylight
   saving".
2. **Weather**, primary — [Open-Meteo](https://open-meteo.com) (no API key), as **two sequential
   HTTPS requests**, one per direction, to two different endpoints:
   - **Forward (rain-skip, Step 1)** — `/v1/forecast?hourly=precipitation_probability,precipitation
     &past_days=5&forecast_days=2&timezone=UTC`. An ArduinoJson `DeserializationOption::Filter` keeps
     only those two hourly arrays (the raw payload also carries `hourly_units`, `elevation`,
     `generationtime_ms`, etc., which would otherwise inflate the parsed `JsonDocument`). Passing
     `timezone=UTC` makes the arrays start at 00:00 UTC of `today - 5 days`, one entry per hour, so
     "now"'s index is `domain::openMeteoNowIndex()`'s `5*24 + current_UTC_hour` — plain arithmetic, no
     timestamp parsing. `domain::summarizeForecastRainWindow()` maxes/sums the 12 entries from there.
     `past_days=5` is kept only so that index arithmetic stays valid; the past hours it returns are
     not used.
   - **Historical (formula inputs, Step 2)** — the **ERA5 archive**,
     `archive-api.open-meteo.com/v1/archive?start_date=<today-5>&end_date=<yesterday>
     &daily=precipitation_sum,temperature_2m_max&timezone=UTC`. Date-bounded because that endpoint has
     no `past_days` and rejects any `end_date` after today, so yesterday is the newest day it accepts;
     asking for exactly 5 days means the first 5 array entries *are* the window.
     `domain::summarizePastAverageDailyMaxTemperature()` averages `temperature_2m_max` and
     `domain::summarizePastRainfall()` sums `precipitation_sum`. Both **reject a null inside the
     window** (the archive's "not available yet"): ArduinoJson reads null as 0, which would be a
     silent "0mm fell" (raising the percentage) or a freezing day (lowering it ~5°C per missing day).
     Daily *max*, not a 24h mean — see **Calculation** below for why that distinction is load-bearing.

   **Why the history comes from the archive and not the forecast endpoint's `past_days`.** For past
   dates `/v1/forecast` serves the weather *model's own output* for those hours, stitched from recent
   runs and never revised against what actually happened. ERA5 is also a model, but one that
   assimilates real observations (stations, radar, satellite) after the fact — coarser (~25 km grid,
   so it smooths localized convective storms) but anchored to reality. They are not close: for the
   window 2026-09-14…18 the forecast endpoint's series read **24.34°C / 16.7mm**, ERA5 read
   **25.32°C / 10.5mm** — about 25 percentage points of watering at the calibrated coefficients.
   Every constant in `board_config.h` is fitted to the archive series, so the two must not be mixed.
   The two requests are separate calls (`fetchOpenMeteoForecastWindow` then
   `fetchOpenMeteoArchivePast`) so each one's `WiFiClientSecure` (~40–60KB of mbedTLS state) is
   destroyed before the next TLS handshake starts. **Either failing fails the whole Open-Meteo
   attempt**, which falls through to WeatherAPI below — deliberately not "use the forecast series as
   a stand-in", for the mixing reason above.
3. **Weather, fallback** — [WeatherAPI.com](https://www.weatherapi.com) (`forecast.json` for the
   forward rain-skip window, + 5×`history.json` — one call per past day, since that endpoint only
   ever returns a single day — for the historical temperature/rainfall), used only when
   Open-Meteo's request fails outright (bad HTTP status or JSON parse error). Each `history.json`
   response's `day.maxtemp_c`/`day.totalprecip_mm` are averaged/summed across the 5 calls —
   `maxtemp_c`, not `avgtemp_c`, so the fallback feeds the formula the same daily-peak quantity
   Open-Meteo's `temperature_2m_max` does. **It is a third source with its own bias, and is not
   calibrated:** for 2026-09-14…18 it reported 23.64°C / 10.5mm against ERA5's 25.32°C / 10.5mm,
   which is ~30 percentage points lower at the current coefficients (~50% vs 80%). Acceptable for a
   fallback that runs only when Open-Meteo is down, but a day watered from it should be read as
   approximate. WeatherAPI.com exposes no evapotranspiration field, so a
   future ET₀-based formula would have to derive it here (Hargreaves-Samani from
   `maxtemp_c`/`mintemp_c` + latitude + day-of-year) rather than read it.
   Requires `settings.json`'s `weatherapi_key`; `fetchForecastWeatherApi()` returns failure
   immediately if that's empty rather than attempting a call that can't succeed.

### When it runs

`settings.json`'s `auto_adjustment_enabled` (default `true`) is the master on/off switch for the
whole feature, checked first thing in both `runAutoAdjustmentIfDue()` and
`weatherAutoAdjustStatus()` — when `false`, the daily cycle never runs and scheduled watering keeps
using the manual `watering_percentage` unmodified, exactly as if the feature didn't exist. It's
editable from the device's own rotary UI (Settings menu, second item — right after Schedule, since
auto-adjustment only makes sense while the schedule itself is active) or Home Assistant's
`switch.irrigator_auto_adjustment`; either path writes through `/cmd/auto_adjustment` semantics and
persists immediately (see **MQTT**, above). Geolocation resolution (`tryGeolocateIfDue()`) is
deliberately *not* gated by this — it's cheap, one-time, and its result (city/coordinates) is still
worth having even with adjustment disabled.

`runAutoAdjustmentIfDue()` (called every `loop()` iteration, cheap to no-op) picks its gate from `autoAdjustScheduleGateReason()` depending on whether *today* has an active schedule entry:
- **Watering scheduled today**: opens a window `AUTOADJUSTMENT_LEAD_TIME_MINUTES` (5) before the day's *earliest* start — or immediately, if the device boots after that window has already opened. Once open, it retries every `AUTOADJUSTMENT_WEATHER_RETRY_INTERVAL_MS` (1 minute), so a failed/slow fetch still gets a few attempts before that first watering fires.
- **Nothing scheduled today**: instead waits for `AUTOADJUSTMENT_OFFDAY_CHECK_INTERVAL_S` (24h) to pass since the last successful check (or checks immediately if there's never been one). This keeps the weather pipeline — and its failure alerts — exercised daily even through a stretch with an empty schedule, and lets a rain-skip decision land ahead of tomorrow's first watering instead of waiting for that day's own 5-minute window.

Either path stops retrying once a forecast is successfully applied for that calendar date, tracked by `main.ino`'s `lastWeatherUpdateDate`. Retries are otherwise **deliberately uncapped** — a forecast that only arrives after the first watering has already run at the last-known percentage still helps later same-day waterings, or the rain-skip decision, so retrying past that point is worth it rather than giving up.

`lastWeatherUpdateDate` is a plain in-RAM global, **not** a `settings.json` field — a power cycle resets it to empty, so the device re-attempts today's cycle on the next boot (immediately, or once the lead-time window opens) rather than remembering it already ran before the reboot. This is deliberate: the auto-adjustment window is designed to tolerate re-running, so there's no reason to persist "already done today" across a restart, and doing so would mean writing to flash daily for no benefit.

### Calculation

`schedule.json`'s per-zone `duration_min` is already the base runtime at the `AUTOADJUSTMENT_BASE_TEMP_C` reference temperature (23.0°C), so there's no separate mm-of-water/flow-rate conversion — the whole calculation collapses to one percentage, applied through the exact same `watering_percentage` → `ScheduleEngine::tick()` path the manual **Adjust** setting already uses.
Both inputs below are **historical** (trailing `AUTOADJUSTMENT_PAST_WEATHER_DAYS`, 5 days) — never the forecast:

The main formula is:
```
Percentage = 100 + (avgPastDailyMax5Days - base_temp_c) * temp_modifier_pct_per_degree
                  - rainPast5Days * rain_modifier_pct_per_mm
```

**What the temperature input is matters more than it looks.** `avgPastDailyMax5Days` is the mean of
the five daily **maxima**, not a 24-hour mean, so `base_temp_c` has to be a daily-max reading too —
the average daily peak of a day that should water at exactly 100%. The two only mean anything as a
pair. Between commits `c3af5c9` and this one they were mismatched: `c3af5c9` calibrated
`base_temp_c = 20` against an hourly-mean input, `af3497b` switched the input to `temperature_2m_max`
and left the base at 20. At this site the season-mean gap between daily max and 24h mean is 5.2°C,
so the formula read every day as ~5°C hotter than it was and added a near-constant **+52 percentage
points**. The recalibration below fixes the pairing.

Let's review an example:

**Data received**

Daily (past 5 days feed Step 2; today/tomorrow are ignored by the formula):

| Date	| rain_sum (mm)	| max temp (°C) |
|-|-|-|
|2026-08-17	| 0.0	| 27.7|
|2026-08-18	| 0.0	| 29.5|
|2026-08-19	| 14.2 |	30.3|
|2026-08-20	| 19.4 |	26.5|
|2026-08-21	| 35.3 |	21.3|
|2026-08-22 (today)	| 0.0	| 25.3|
|2026-08-23 (tomorrow)	| 0.0	| 25.2|

Next-12h hourly window (06:00→17:00 UTC today, nowIndex=126), feeds Step 1: every hour came back precipitation_probability=0%, precipitation=0mm. So max prob = 0%, summed rain = 0mm.

**Step 1 — Rain-skip check**

`shouldSkipForRain`(prob>=70%, amount>4mm) (board_config.h:121-122, or from `settings.json`): 0% >= 70% → false. No skip.

**Step 2 — Correction formula**

```
percentage = 100 + (avgPastTempC − 23.0) × 12.5 − rainPastDaysMm × 3.1
avgPastTempC = (27.7+29.5+30.3+26.5+21.3)/5 = 27.06 °C
rainPastDaysMm = 0+0+14.2+19.4+35.3 = 68.9 mm

percentage = 100 + (27.06 − 23)×12.5 − 68.9×3.1
           = 100 + 50.75 − 213.59
           = −62.84  →  below lower_limit=40, so the cycle is skipped: 0%
```

(The table above was captured under the old forecast-endpoint history; today the same five days
would come from the archive and differ by a few tenths of a degree and a few mm — the arithmetic is
what this example is for.)

Under the pre-recalibration constants (base 20°C, 10 %/°C) the same day produced 95%. 68.9mm of
rain in five days emptying the cycle is the intended behaviour; what the old constants were doing
was letting a +52pp temperature bias cancel most of that rain credit.

then clamped: below `lower_limit` → **0%** (skip the cycle entirely, not clamped up to the limit);
above `upper_limit` → capped at `upper_limit`.

### Calibration and how it was chosen

The three coefficients are the `board_config.h` constants (`AUTOADJUSTMENT_BASE_TEMP_C`,
`AUTOADJUSTMENT_DEFAULT_TEMP_MODIFIER_PCT_PER_DEGREE`, `AUTOADJUSTMENT_DEFAULT_RAIN_MODIFIER_PCT_PER_MM`),
read directly by `computeAutoCorrectedPercentage()`. A per-device override through `settings.json`'s
`Watering` group (`base_temp_c`, `temp_modifier_pct_per_degree`, `rain_modifier_pct_per_mm`) was
prototyped and is currently disabled (commented out in `Settings.cpp`), so changing them means
editing `board_config.h` and flashing. They are deliberately **not** exposed in the device menu or
Home Assistant: they are calibration constants for a site, not controls.

The current defaults (23.0°C / 12.5 / 3.1) were fitted against Open-Meteo **ERA5 archive** history
for this site (Biella, 45.563N 8.058E) over April–September 2026, against two anchors the owner
supplied plus one sanity point:

| anchor | inputs (ERA5) | target | result |
|---|---|---|---|
| 2026-09-19 (a shoulder-season day judged over-watered) | avg daily max 25.32°C, 10.5mm | ~96% | 96% (was ~80% under the second fit) |
| peak July (2026-06-27–07-01) | avg daily max 33.62°C, 7.3mm | ≥ 200%, i.e. still capped | 210% raw → 200% |
| ordinary July week | avg daily max 31.5°C, 2.0mm | 200% (in the cap, not just at the extreme) | 200.05% raw → 200% |
| cool spell, no rain | avg daily max 18.2°C, 0mm | still waters at the 40% floor | 40% (18.1°C → skip) |

**Two calibrations, and why the second one exists.** The first fit (24.0°C / 16.0 / 1.6) was made
against the forecast endpoint's series, where 2026-09-19 read 24.34°C / 16.7mm. When the history
moved to the ERA5 archive (see Data sources), the same day read 25.32°C / 10.5mm — hotter *and*
drier — and the first fit returned **104%**, i.e. the original overshoot back again. So the base moved
24.0 → 25.5 to restore the ~80% anchor, and the slope 16 → 18 to keep an *ordinary* July week in the
cap (16 with the new base left it at 191%: passing the peak-day anchor while quietly under-watering
the typical summer day). Rain stayed at 1.6. Over the Apr–Sep watering days (Wed/Sat) the refit uses
~10% less water than the first fit, with 14 of 47 days at the 200% cap.

**Third calibration (2026-09-21): 23.0°C / 12.5 / 3.1.** The second fit skipped watering below
22.2°C, which is too warm for a cool spell that still needs the 40% floor. The line was re-pivoted
so the floor sits at 18.2°C and the cap at 31.0°C (both at 0mm): base 25.5 → 23.0, slope 18 → 12.5.
Flattening the slope alone would have moved the shoulder-season anchor from 80% to ~112%, so the
rain term was raised 1.6 → 3.1 (rain now takes ~0.25°C off the cutoff per mm instead of ~0.09°C).
The price is a thin summer margin: the ordinary-July anchor now clears the cap by only 0.05 points
at 2mm (196.95% at 3mm), so a further nudge to slope or rain will start under-watering ordinary
July weeks.

**The third row is the one that constrains the slope.** `upper_limit` is not a safety rail here,
it is a deliberate water *budget*: peak-summer demand genuinely exceeds it, and saturating there is
the intended behaviour. That rules out simply re-centring the base and leaving the slope at 10 —
that combination hits the first anchor but drops summer below the cap, quietly cutting its water.
Steepening the slope pivots the line instead of shifting it, so all anchors hold: the raw
percentage reaches the 200% cap at 31.0°C and crosses below the 40% skip floor at 18.2°C (both at
0mm rain; see `docs/usage_irrigator.md`'s Step 2 table for the full grid).

**A single calibration day is thin.** The ~80% anchor is one owner judgment ("that day looked
over-watered"), and the model behind it is uncertain to the tune of ~25 points depending on which
source is asked — which is exactly what the source switch above demonstrated. Treat these as a
working calibration, not a validated one, and expect to adjust `base_temp_c` as more days accumulate.

`rain_modifier_pct_per_mm` is 3.1 (raised from 1.6 in the third calibration), and it is **entangled** with the temperature terms
and should not be read as independently validated: while the base temperature was biased high, the
rain term was partly acting as a bias trim (it was raised 1.1 → 1.6 in `a598b5b` for exactly that
reason). Expect it to need re-tuning now that the bias is gone. There is also a structural caveat —
`%/mm` is a property of the *schedule*, not of the climate: it is only dimensionally correct if every
zone's 100% dose is the same depth in mm (true here, where the tree zones run long on low-rate drip
while the lawn zones run short on high-rate sprays), and rescaling every `duration_min` would require
rescaling this coefficient inversely.

**Known limitations of the linear form** (documented so the next round of tuning doesn't re-litigate
them): temperature is only a proxy for evaporative demand, and it carries no seasonal information.
At this site, five-day windows averaging 29–31°C daily max have a real ET₀ of 5.9 mm/day in May but
4.1 mm/day in September — a 30% swing in actual water need that no choice of `base_temp_c` and
`temp_modifier_pct_per_degree` can represent, because the formula sees both windows as the same
temperature. Open-Meteo returns `et0_fao_evapotranspiration` in the *same daily block already
requested*, so replacing the temperature term with `Kc × ET₀` is cheap in bytes; it is not done here
because calibrating it needs more owner-supplied anchors than the one shoulder-season day available
so far. See `src/domain/AutoAdjustment.cpp` and its unit tests for the exact behavior at each
threshold.

Separately — and using the *forecast*, not the history above — if the next 12 hours show rain
probability **at or over** `rain_skip_probability_percent` (default 70) **and** rain amount **over**
`rain_skip_amount_mm` (default 4), the day's percentage calculation is skipped entirely and
`rainSkip.setSkipUntil()` is called for 24 hours instead — reusing the same `RainSkip` gate
`cmd/skip_rain_until` already writes to (see **MQTT**, above), so manual/MQTT `cmd/run` remain
unaffected either way. The two thresholds are intentionally asymmetric: the probability check is
`>=`, the amount check is `>` (see `domain::shouldSkipForRain()`, which takes both thresholds as
parameters rather than reading fixed constants — they're user-configurable settings, like
`upper_limit`/`lower_limit`, not compiled-in defaults).

### Known limitations

- `latitude`/`longitude`/`city` have no manual-entry path, and — unlike every other field —
  `mergeSettings()` deliberately never reads them from an uploaded `settings.json` at all (they're
  device-owned, populated only by the device's own geolocation resolution), so there is currently
  no fix from outside the device if ip-api.com's IP-based geolocation is inaccurate for your
  network (common on VPNs, CGNAT, or some rural ISPs).
- The same inaccuracy now also lands the clock in the wrong timezone, since `config.json`'s
  `timezone` is derived from that lookup. That one *is* fixable from outside: upload a
  `config.json` with an explicit `timezone` — `mergeConfig()` still honours the field, and
  geolocation won't overwrite it because it only ever resolves once.

## Building and flashing over USB (first time)

1. Install [VS Code](https://code.visualstudio.com/) and open this repository as a folder. Accept    the recommended-extensions prompt (or install everything under the Extensions panel's    `@recommended` filter) — this pulls in the PlatformIO IDE extension, which provides the build/    upload/monitor tooling and its own status-bar icons (checkmark to build, right-arrow to upload,    plug icon to open the serial monitor).

   > Note: `platformio.ini` points the `esp32dev` environment at the community `pioarduino` fork of  the espressif32 platform (not the stock PlatformIO one) — this is deliberate, not a typo, and the IDE extension handles it transparently.
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

- Current-fault alerting is inert until `cmd/calibrate` has been run for each sensor at least once
  (base defaults to 0 = uncalibrated = no check) — see **cmd/calibrate** under **MQTT**, above.
  `CURRENT_CALIBRATION_MIN_BASE_LEVEL` (`board_config.h`) is also an untuned placeholder pending
  real hardware verification.
- The SSD1309 U8g2 driver variant (`NONAME0` vs `NONAME2`) hasn't been verified against a real
  panel; if the display shows garbage or inverted colors on first boot, swap the constructor in
  `src/ui/IrrigatorUi.h`.
