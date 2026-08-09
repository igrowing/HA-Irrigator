# AGENTS.md - Project Context & Operational Rules for AI Agents

This file provides non-negotiable architectural context, programming standards, and verification workflows for autonomous agents working on this ESP32 (Arduino framework, C++) garage irrigation controller firmware. Read and internalize this file entirely before executing any tasks. This device controls live 24VAC valves and a 230VAC pump — treat correctness and fail-safe behavior as higher priority than feature velocity.

---

## 1. Project Overview & Architecture Blueprint

### Directory Layer Convention

Every feature must follow a strict separation of concerns. Do not mix hardware register access, business logic, and network/UI code in the same function or file:

* `src/drivers/` : Thin wrappers around individual physical peripherals — PCF8574 relay expander, DS3231 RTC, SSD1309 display, rotary encoder + button input, ADC current sensors. Each driver exposes a small interface (e.g. `IRelayBank`, `IClock`, `ICurrentSensor`) and contains the *only* code in the project allowed to call `Wire.h`, `digitalWrite`, `analogRead`, or similar direct hardware calls.
* `src/domain/` : Pure C++ business logic — the irrigation state machine, the UI/menu state machine, the schedule queue, the duration-adjustment formula, current-fault threshold checks. This layer depends only on the driver *interfaces* defined above, never on concrete Arduino types or `#include <Arduino.h>` directly. This is what makes Step 1.B.3 below possible.
* `src/network/` : WiFi connection handling, captive portal, MQTT client wrapper, OTA, NTP sync, Home Assistant discovery/state publishing.
* `src/ui/` : Display rendering (screen 1 / menu screens) and encoder input dispatch. Reads from `domain/` state, never contains irrigation logic itself.
* `include/board_config.h` : The **only** file containing pin numbers, I2C addresses, and board-specific `#define`s. No other file may hardcode a GPIO number.
* `test/native/` : Host-run unit tests (see Step 1.B.3) for everything in `src/domain/`.

### Core Design Patterns

* **Composition Root, Not Scattered Globals:** All long-lived objects (relay bank, RTC, display, MQTT client, WiFi manager, the irrigation state machine) are instantiated exactly once, in `main.cpp`, and passed by reference or pointer into whatever needs them. Never declare `extern` globals in arbitrary files, and never lazily construct a driver instance inside a function the first time it's called. This mirrors a service-locator's discipline without pulling in a DI framework a microcontroller can't afford.
* **Hardware Abstraction via Interfaces:** Domain code must never call `Wire.h`, `digitalWrite`, or any Arduino core function directly — it calls through the small interfaces defined in `drivers/`. The concrete Arduino-backed implementation is wired in at the composition root; a mock implementation is swapped in for native unit tests. This is the same discipline as "conditional imports over scattered `Platform.isAndroid` checks" — one seam, defined once, not `#ifdef ESP32` scattered through business logic.
* **Board/Pin Isolation:** All pin assignments live in `board_config.h` (see above). If this project is ever ported to a different ESP32 board revision, that file — and only that file — should need to change.
* **Event-Driven Flow Over Polling Loops:** Default to callback- and event-driven patterns — MQTT library callbacks, `Ticker`-scheduled callbacks, an ISR that sets a flag polled once per `loop()` iteration — instead of structuring logic as a tight loop that repeatedly re-checks a condition (`while (!ready) { delay(10); }` and similar). This keeps `loop()` responsive to WiFi/MQTT/UI events instead of being stuck spinning on one wait. `Ticker` is the default tool for "do X every N seconds" or "do X once, N seconds from now." Before using it, read the ISR/Ticker safety rule in Section 2 below — `Ticker` is for scheduling *when* a callback fires, not a place to put blocking or slow work.

---

## 2. Non-Negotiable Coding Standards & Modern Syntax

### Anti-Hallucination & Embedded-Safe API Rules

Agents must use patterns that are safe on a resource-constrained, long-uptime microcontroller. Do not copy generic Arduino tutorial code that ignores these constraints:

* **ISR Safety:** Interrupt service routines (rotary encoder pins, any future GPIO interrupt) must do the absolute minimum: set a `volatile` flag or push to a lock-free ring buffer, then return. **Never** call `delay()`, `Serial.print()`, perform I2C/SPI transactions, allocate heap memory, or run any non-trivial logic inside an ISR. Mark ISR functions `IRAM_ATTR` on ESP32. Do the real work in `loop()` by polling the flag/buffer the ISR set. (This project has already hit a real freeze caused by a quadrature-decode state machine running inside a GPIO interrupt handler — do not reintroduce that class of bug.)
* **No Blocking Delays in the Main Loop:** `delay()` anywhere in `loop()` or in code reachable from it stalls WiFi/MQTT servicing and UI responsiveness. Use `millis()`-based non-blocking timing or the `Ticker` class for anything periodic.
* **Ticker Callback Discipline (ESP32-specific):** On ESP32, `Ticker` runs on the shared `esp_timer` service task — not a true hardware ISR, but still a single task shared by every `Ticker` instance and other `esp_timer`-based subsystems in the firmware (including parts of the WiFi stack). A slow or blocking callback in *one* `Ticker` delays *every* `Ticker` and timer-driven subsystem in the whole firmware, not just its own schedule. Treat `Ticker` callbacks with near-ISR discipline: set a flag or increment a counter and return immediately. Never perform an I2C/SPI transaction, call `delay()`, publish MQTT, or do file I/O directly inside a `Ticker` callback — do that work in `loop()` after checking the flag the callback set.
* **Avoid Heap Fragmentation:** Do not build up `String` objects via repeated concatenation in `loop()` or any frequently-called path (this is one of the most common causes of multi-day-uptime ESP32 crashes). Prefer fixed-size `char` buffers, `snprintf`, or a `String` with `reserve()` called once. Avoid repeated small `malloc`/`new` of variable-sized buffers on hot paths.
* **ArduinoJson Discipline:** Size `JsonDocument`s appropriately for the payload (config.json, schedule.json, settings.json, MQTT command payloads) — don't over-allocate, and don't guess a size that silently truncates a valid payload. See Section 3 for what to do when parsing fails.
* **Prefer `constexpr`/`static const` over `#define`** for typed constants (thresholds, buffer sizes, timeouts) — catches type errors at compile time that a macro would hide.
* **Don't repeat yourself** restriction - when any functionality is needed to implement, review existing classes and function for similar functionality. Often you will find that what's needed is already implemented. Then just call and reuse it. If slight modification to the existing functionality is needed and that function has only 1-2 invocations, enhance that function and adapt the existing invokations. If signigicant modification needed or too many calls exist to the implemented function then add a function which has larger functional span and then the older, narrower span function, can use the new one to avoid code duplication. Add new functions only if there is no similar fucntionality is already implemented.


### Code Style & Formatting Constraints

* **Static Analysis:** Code must pass `pio check` (cppcheck/clang-tidy via PlatformIO) with no new warnings introduced.
* **Formatting:** Run `clang-format` (project `.clang-format`, LLVM-based style) on any modified files. Target ~100 characters per line — embedded C++ conventionally runs wider than Dart's 80, but keep it consistent.
* **Generated/Build Artifacts:** Never manually edit anything under `.pio/build/`. If a code-generation tool is introduced later (e.g. nanopb, should MQTT payloads move off raw JSON for size reasons), its output files follow the same never-hand-edit rule.
* **Versioning:** Bump the firmware's minor version (`FIRMWARE_VERSION` in `include/board_config.h`, mirrored in the `platformio.ini` build flag) on every commit. Bump major or middle version only when explicitly requested by the developer. The current version must be included in the MQTT system-status payload (`irrigator/<hostname>/status`) so a deployed unit's firmware version is always visible without a serial connection.

---

## 3. Strict Error Handling & Failure Protocols

* **No Silent Swallowing:** Never ignore a return code from a hardware or I/O call — `Wire.endTransmission()`, file open/read/write on the filesystem, an MQTT `publish()` call, an ADC read. Every failure must be logged via the project's logger wrapper (never a bare `Serial.print`, see Step 3 below) and, for anything safety- or availability-relevant, published to `irrigator/<hostname>/alert` and reflected in the status/fault LED's blink pattern (define distinct patterns per fault class — e.g. I2C bus failure vs. WiFi loss vs. current-sensor fault — so a fault is diagnosable by eye in the garage without a laptop).
* **Explicit Failure Over Default Data:** When parsing `config.json`, `schedule.json`, or `settings.json`, or an incoming MQTT command payload, **never** silently substitute a placeholder or clamp an out-of-range value into something plausible (e.g. a zone number outside 1–11, a malformed time string, a missing field defaulting to `0`). Reject the record or the whole file, log it, alert over MQTT, and fall back to a defined safe state — e.g. missing/invalid `config.json` re-enters captive portal mode; an invalid `schedule.json` marks the schedule inactive rather than guessing at intent. It is always better for this device to fail loudly and stay idle than to silently water the wrong zone, or the right zone for the wrong duration, from corrupted or hallucinated data.

---

## 4. Operational Steps & Verification Loop

When assigned a development ticket or bug fix, execute the task according to this chronological checklist. Do not skip steps.

### Step 1: Establish Environment Baseline

Before changing a single line of production code:

A. Pull the changes from the current remote repo. In team coding, account for peer changes already in flight.
B. Verify the codebase is in a functional state to isolate future errors:
  1. Run `pio run -e esp32dev` (or the project's active environment) to resolve dependencies and confirm the firmware compiles cleanly — in C++, compilation itself is the static-correctness check.
  2. Run `pio check -e esp32dev` to confirm the starting code is clear of static-analysis warnings.
  3. Run `pio test -e native` to ensure existing unit tests for `src/domain/` currently pass on the host machine. This step only works because domain logic is hardware-abstracted per Section 1 — if a change makes something in `domain/` depend on a concrete Arduino type, that's a Section 1 violation to fix first, not a reason to skip this step.

### Step 2: Implement Changes

1. Execute your structural modifications localized to the corresponding layer (`drivers/`, `domain/`, `network/`, or `ui/`).
2. If a code-generation tool is in use for a given change (e.g. nanopb schemas), regenerate via its documented command rather than hand-editing output. Most changes in this project will not need this step.
3. On changes to production code:
  * Add a native unit test for any new function or class added to `src/domain/`.
  * On removal of obsolete or merged functions, remove their unit tests too.
  * On changes to existing functions, update the corresponding tests to match.
  * Changes confined to `src/drivers/` (real I2C/GPIO register access) generally cannot be meaningfully unit-tested without hardware mocks — document the manual hardware verification you performed instead (see Step 5).

### Step 3: Self-Correction & Automated Cleaning

Before presenting your changes for human review, clean your workspace of transient tracking code:

* Verify the diff contains **no** leftover bare `Serial.print()`/`Serial.println()` calls outside the project's logger wrapper. Anything worth logging on a device sitting in a garage with no attached terminal should go through the logger, and anything critical should also reach `irrigator/<hostname>/alert` — nobody is watching the serial monitor on a deployed unit.
* Remove unused `#include` statements and any leftover commented-out code blocks (e.g. a disabled `pump_relay.turn_on()` call left in from earlier debugging) rather than leaving them for the next person to puzzle over.

### Step 4: Verification Testing Pipeline

Confirm your changes have not created regressions elsewhere:

1. Run `pio check -e esp32dev` again to confirm the new code introduces no static-analysis warnings.
2. Run the native test suite targeting the module you modified: `pio test -e native -f test_<target_module>`.
3. If errors are flagged, read the compiler or test output, apply a corrective fix, and re-run until the console reports a clean pass.

### Step 5: Hardware-in-the-Loop Smoke Test

Unlike a mobile app, a clean compile and a passing native test suite do **not** guarantee correct behavior on the real device — I2C bus timing, GPIO wiring, and interrupt behavior only manifest on actual hardware. Before considering any change complete:

1. Flash the firmware to a real (bench or production) unit.
2. Confirm boot completes without an I2C scan failure or crash-loop in the serial log.
3. Confirm the MQTT `irrigator/<hostname>/status` payload publishes successfully within 15 seconds of boot, and that `irrigator/<hostname>/availability` reads `online`.
4. For any change touching WiFi connection handling, captive portal, or OTA specifically: apply extra scrutiny and test the OTA path itself before merging. A broken OTA update path on a deployed unit means recovery requires a physical USB connection — treat changes in this area as higher-risk than any other part of the codebase.