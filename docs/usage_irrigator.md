# Using the Irrigator

## What it does

The Irrigator controls up to 11 sprinkler zones (valves) plus a shared pump. You can run any zone
on demand, or set up a weekly watering schedule and let it run itself. It has a small screen and a
turn-and-click knob (rotary encoder) built in, so it works standalone even before you connect it
to anything else.

Integration (pairing) of the Irrigator with Home Assistant benefilts you with:
- Easy configuration and settings update: Zone naming, Scheduling, defaults.
- Ability of remote control of the Irrigator.
- Ability of status observation and historical watering review.
- Sophisticated weather-awareness for automatic adjustment of watering - *not implemented yet*.

If you want to hook it up to Home Assistant afterwards, see
[usage_homeassistant.md](usage_homeassistant.md).

## Connections and wiring

> **Safety first:** everything below the transformer's 24VAC output is low-voltage and safe to
> handle. Everything upstream of it — the mains feed and the pump wiring — is 230VAC. Always
> switch off the breaker feeding the unit before touching any of that, and if you're not
> comfortable working with mains wiring, have an electrician make those connections for you.

Two reference layouts cover most installs:

- **Simple wiring** — one enclosure holds the transformer, power supply, current sensors, and
  the control board together. Easiest to follow if everything fits in one box.
- **Separated eco wiring** — the mains-facing parts (transformer, power supply, current sensors)
  live in their own small enclosure, connected to the control board by a short low-voltage cable.
  Useful if you'd rather keep the ESP32 and relay board away from the mains wiring, or need to
  split things across a tighter space.

### Simple wiring
![Simple wiring diagram](../hardware/irrigator%20wiring-simple.png)

 ### Separated eco wiring
 This wiring method runs the 24VAC transformer only when pump is running. When valve is open, the pump is active. This saves the energy waste (and the electricity bill) of running the transformer without any load.

 ![Separated eco wiring diagram](../hardware/irrigator%20wiring-eco%20separated.png)

Either way, the same four things need wiring up: the transformer, the current sensors, the pump,
and the valves.

### Placing the 24VAC transformer

The transformer converts your 230VAC mains supply down to the 24VAC that actually drives the valve solenoids.

- It is recommended to user DIN-rail mount transformer of 15VA at least.
- Mount it in a dry, ventilated spot with the rest of the electronics — a garage or utility
  closet, or inside the same weatherproof enclosure as the control board. Never mount it inside an
  outdoor valve box or anywhere it could get wet.
- Keep it reasonably close to the control board — the run from the transformer's 24VAC output to
  the board should be short, since long thin cable runs droop the voltage the valves see.
- **Primary side (230VAC, mains)**: Live and Neutral from a breaker-protected circuit, wired with
  the breaker off. If the transformer has an earth terminal, connect it to your home's protective
  earth.
- **Secondary side (24VAC)**: two wires, no polarity to worry about since it's AC — these feed the
  board's 24VAC input, which powers the pump relay and all 11 zone relays.

### Wiring the current sensors

- **24VAC current sensor**: pull the 24VAC wire that carries current to whichever valve
  is open — this is the wire between the transformer's 24VAC output and the valves' common
  terminal (see below). It reads the combined draw of whatever zone (or the pump) is currently
  running.
- **230VAC current sensor**: pull the mains Live wire feeding the pump, after the
  pump's relay — so it only reads current while the pump relay has actually switched the pump on.

Two things matter for either sensor to read correctly:
- Thread only **one** of the two wires (just the Live/hot side) through the clamp — if both wires
  of a pair pass through together, their currents cancel out and the sensor reads close to zero.
- Pass the wire through the clamp once (a single loop), not several times, and note the direction
  arrow printed on the sensor if it has one.

Fresh current sensors read placeholder values until calibrated against your actual hardware setup
(see **Current-sensor calibration** in [technical.md](technical.md)) — don't worry if the raw
readings look off before that step.

### Connecting the zone valves

For wiring of valves it is recommended to use the wries at least 20 AWG or thicker. This ensures minimal voltage drop for distant valves. If your valves are very close to the irrigator, using thinner wires is OK too.

Each sprinkler valve has two wires, and it doesn't matter which is which — it's an AC solenoid,
not polarity-sensitive:

1. Take **one wire from every valve** and join them all together (a wire nut or a terminal block
   works well), then run that single joined wire back to the board's **Valves common** terminal.
   This is the shared return path every zone uses.
2. Take the **other wire from each valve** and run it back individually to that valve's own
   numbered **Zone** terminal on the board (Zone 1 through Zone 11). Only that valve's relay
   switches 24VAC onto its own zone wire, so only it opens when its zone runs.
3. Keep the cable gauge and run length reasonable for the distance involved — solenoids are
   sensitive to undervoltage, and a long thin run to a distant valve can drop the voltage enough
   that it won't reliably open.

### Connecting the pump

The pump runs on 220-240VAC mains, switched through its own relay on the board (unlike the valves,
which run on the low-voltage 24VAC side). Wire the pump's supply through the board's **To Pump**
terminals, with the breaker off while you work, and make sure the circuit feeding it has its own
appropriate mains protection.

## First-time Wi-Fi setup

A brand-new (or freshly reset) unit doesn't know your Wi-Fi yet, so it starts up in **setup mode**:

1. Power it on. The status LED blinks slowly (on/off about every second) and the screen shows a
   network name like `irrigator42_SETUP` along with an address, `192.168.4.1`.
2. On your phone or computer, open Wi-Fi settings and connect to that network
   (`irrigator42_SETUP`). It has no password.
3. Open a web browser and go to `http://192.168.4.1/`. A setup page appears — fill in:
   - Your home Wi-Fi network name and password
   - Your MQTT server details, *only if* you're integrating with Home Assistant — leave the
     defaults if you're not sure, you can always come back and redo setup later
   - Your timezone, as a plain hour offset from UTC (e.g. `+2`). Daylight saving is handled for you
     automatically — you don't need to adjust anything for it.
4. Submit the form. The Irrigator tries connecting with what you entered. If it works, it saves
   the settings and restarts normally, connected to your home Wi-Fi. If something's wrong (wrong
   password, unreachable server, etc.), it forgets the attempt and goes back to setup mode so you
   can try again.

Once connected, the screen shows the last part of its IP address and the current date/time instead
of the setup network name — that's your sign it's ready to use.

## Reading the screen

During normal operation, the screen looks like this:

```
IP.42    2026-07-11 14:32:07
----
           ZONE 3
      WATERING 4:32
     LONG CLICK = MENU
```

- **Top line**: last part of the device's IP address, plus the current date and time.
- **Dashes row**: a quick Wi-Fi signal strength meter — 0 dashes means little to no signal, 4
  dashes means excellent. (You can check the exact signal strength in **Settings**, below.)
- **Middle line**: what's happening right now — a zone name, `PUMP`, or nothing if idle.
- **Status line**: one of:
  - `IDLE-READY` — nothing running, nothing scheduled soon
  - `SCHEDULED` — nothing running right now, but the middle line shows which zone will run next
  - `WATERING 4:32` / `REMAINING 11:05` — actively running; alternates between elapsed and
    remaining time every few seconds
- **Bottom line**: a reminder that holding the button down opens the menu.

The screen dims after 20 seconds of no activity and turns off shortly after. Turning the knob or
pressing the button wakes it back up — the first tap or turn after it's asleep just wakes the
screen, it won't accidentally start or change anything.

## Using the knob

The knob does two things:
- **Turn** it clockwise or counter-clockwise to move through options or adjust a number.
- **Click** it (short press) to confirm/select.
- **Hold it down** for about a second (long click) to open the menu, or to cancel whatever you're
  currently doing and back out to the normal screen.

## How watering requests queue

A manual zone you start from the knob, the weekly schedule firing, and a run triggered remotely
from Home Assistant/MQTT all land in the same waiting line — up to 15 requests can be waiting at
once. You don't manage this directly, but it explains a few things you might otherwise find
confusing:

- Starting a manual zone right as the schedule (or a remote command) fires doesn't lose either
  one — whichever arrived first runs, and the other waits its turn, starting the moment the first
  finishes.
- **Test all zones** (below) queues every enabled zone in one go, so a full test run doesn't need
  you to sit and restart it after each zone finishes.
- Stopping the current run (below) immediately lets the next waiting item start, rather than
  leaving it stranded until you go find it.

## Starting a zone manually

You can only start a manual watering while the system is idle (not already running something):

1. **Turn the knob** — the screen switches into zone-picking mode, cycling through `PUMP`, then
   `Zone 1`, `Zone 2`, and so on by name. (Any zone you've disabled in settings is skipped
   automatically.)
2. **Click** to pick the zone you want.
3. **Turn the knob** to set how many minutes it should run — small adjustments move by a couple of
   minutes per click, larger durations jump by more. You can set anywhere from 1 to 120 minutes.
4. **Click** to start watering, or **hold the button down** at any point to cancel and return to
   the normal screen without starting anything.

## The menu

Hold the button down from the normal screen to open the menu:

```
MENU
> Stop ZONE 3        (only shown while watering)
  Stop all           (only shown while watering)
  Schedule
  Test all zones
  Settings
  BACK
```

Turn the knob to move the `>` selector, click to choose. Any menu times out back to the normal
screen after 20 seconds of no input, and turning the knob past the last item wraps back around to
the first.

### Stop `<zone>` / Stop all

Only shown while a zone or the pump is actively running.

- **Stop `<zone>`** asks you to confirm (`STOP?  [NO]  YES` — turn to move onto `YES`, click to
  confirm) before stopping just the current run. If you had other waterings queued up behind it,
  the next one starts right away.
- **Stop all** works the same way, but also clears out anything else waiting in the queue — use
  this if you want everything to stop, not just hand off to the next one in line.

Either way, choosing `NO` (or backing out) just returns you to the normal screen without stopping
anything.

### Schedule

A scrollable list of watering programs. You can add up to 32 in total, spread across any zones,
days, and times you like — enough for several programs per zone across the whole week. That cap
exists to keep the saved schedule comfortably within the device's storage budget, not as an
arbitrary restriction; you're unlikely to run into it in normal use.

Each row shows one program's zone, day, start time, duration, and whether it's active or paused.
Click any field to edit it:

- **Zone** / **day**: click, then turn the knob to step through zones (by name) or days of the
  week, click again to confirm.
- **Start time**: click to edit the hour, click again to confirm and move to minutes, click once
  more to confirm the whole time.
- **Duration**: click, then turn the knob to adjust (1–60 minutes), click to confirm.
- **Active/paused**: click once to flip it — no extra confirmation needed.

While editing any field, a long click cancels the edit and discards your change.

### Test all zones

A quick way to confirm every zone works, without setting up a full schedule:

```
TEST ALL ZONES
> Run
  Dur./zone: 5 min
  BACK
```

- **Run** queues every enabled zone to run, one after another, for however long **Dur./zone** is
  set to. The screen returns to normal right away, and zones run through the queue in the
  background.
- **Dur./zone** lets you change how long each zone runs during the test (1–120 minutes, defaults
  to 5 and resets back to 5 after a restart).

### Settings

- **Adjust**: a watering percentage (10–300%) that scales every scheduled run's duration up or
  down — handy for quickly cutting back watering in cooler weather without editing every program.
  Once the device has a location (see **Loc**, below), this value is also recalculated
  automatically once a day by weather-based auto-adjustment — see below.
- **Upper limit** / **Lower limit**: the range auto-adjustment is allowed to move **Adjust**
  within (defaults 200% / 40%). If the computed percentage would fall below **Lower limit**, the
  day's watering is skipped entirely (0%) rather than clamped up to it; above **Upper limit**, it's
  capped at that value.
- **Skip rain prob** / **Skip rain amt**: the two thresholds that decide whether today's watering
  is skipped outright because of rain in the forecast (defaults 70% / 4mm) — see **Step 1** below
  for exactly how they're combined.
- **City** / **Loc**: the city name and latitude/longitude auto-adjustment resolved from the
  device's IP address (via ip-api.com) — display only, refreshed once, the first time the device is
  online with no location saved yet.
- **Last update**: the date (UTC) auto-adjustment last successfully fetched a forecast and applied
  it — display only. Stays blank until the first successful cycle, and resets after every restart
  (this isn't saved — a restart simply re-runs today's cycle rather than remembering it already
  happened).
- **Schedule**: turns the whole weekly schedule on or off.
- **WiFi**: signal quality in plain words (e.g. `good`) plus the exact signal reading.
- **IP** / **MQTT**: connection details, useful mainly if you're troubleshooting network issues.
- **Topic**: the device's unique ID — only relevant if you're integrating with Home Assistant.
- **FW**: the firmware version currently installed (display only).
- **Restart**: reboots the device. Asks you to confirm first (`RESTART?  [NO]  YES` — turn to
  move onto `YES`, click to confirm), the same way **Stop**/**Stop all** do. Choosing `NO` (or
  backing out) returns you to the normal screen without restarting. No saved settings or schedule
  are affected — this is just a reboot, not a reset.

## Resetting to factory defaults

If you need to start over — for example, to reconnect it to a different Wi-Fi network — you can
wipe all saved configuration:

> **Warning:** this deletes all saved settings, including your Wi-Fi credentials and your schedule. Only do this if you mean to reconfigure the device from scratch.

Hold down the **Boot** button on the device itself (not the rotary knob) for about 10 seconds. The
screen will ask you to confirm before anything is actually deleted. Confirming erases the
configuration and restarts the device straight into setup mode (see **First-time Wi-Fi setup**
above), ready to be configured again.

This requires physical access to the device, on purpose — it can't be triggered remotely.


## Autoadjustment of irrigation explained

Once the device has resolved its location (see **City** / **Loc** in Settings, above), it checks
the weather once a day — a few hours before your first scheduled watering — and either skips the
day's watering entirely, or nudges the **Adjust** percentage up or down for that day. Two checks
happen, always in this order, and the first one decides whether the second one runs at all.

### Step 1 — Is today's watering skipped because of rain?

The device looks at the rain **forecast for the next 12 hours** — the only forward-looking check
in the whole feature; everything in Step 2 below looks backward instead. Watering is skipped for
the next 24 hours only if **both** are true at once, against the **Skip rain prob** / **Skip rain
amt** thresholds from Settings (above; defaults shown here):
- rain probability is **at or above Skip rain prob** (default 70%), **and**
- the forecast rain amount is **strictly above Skip rain amt** (default 4mm — exactly 4mm doesn't count)

If either condition isn't met, nothing is skipped — Step 2 (below) runs as normal. Note the two
thresholds work differently on purpose: probability counts *at* the threshold, amount only counts
*above* it — the table below uses the factory-default values (70% / 4mm) to illustrate.

| Rain probability (next 12h) | Rain amount (next 12h) | Watering skipped today? |
|---|---|---|
| 50% | 10mm | No — probability under 70% |
| 70% | 4mm | No — probability qualifies, but exactly 4mm doesn't count |
| 70% | 4.1mm | **Yes** |
| 71% | 4mm | No — probability well over, but amount still exactly 4mm |
| 80% | 5mm | **Yes** |
| 100% | 0mm | No — certain but no actual rain amount forecast |
| 30% | 20mm | No — probability too low despite the heavy rain amount |

Lowering either threshold from Settings makes the device skip more readily (e.g. dropping **Skip
rain amt** to 2mm would make the 70%/4mm row above trigger a skip too); raising them makes it more
tolerant of rain in the forecast.

### Step 2 — If not skipped: how much to adjust

If Step 1 didn't skip the day, the device recalculates **Adjust** (the watering percentage)
automatically, based on the **last 5 days**' actual weather — never the forecast:
- the **average temperature over the past 5 days** — hotter than 20°C increases the percentage,
  colder decreases it;
- the **total rainfall actually measured over the past 5 days** — more recent rain decreases the
  percentage.

The result is then kept between **Lower limit** and **Upper limit** (see Settings, above): if it
would fall below **Lower limit**, that day's watering is skipped entirely (0%) instead of being
raised back up to the limit; if it would go above **Upper limit**, it's capped there instead.

The table below shows the result for a zone whose scheduled duration is **20 minutes**, using the
factory-default settings (**Lower limit** 40%, **Upper limit** 200%, temperature sensitivity 10%
per °C):

| Avg temperature (past 5 days) | Past 5 days rain: 0mm | 2mm | 4mm | 6mm | 8mm |
|---|---|---|---|---|---|
| 10°C | 0min (skip) | 0min (skip) | 0min (skip) | 0min (skip) | 0min (skip) |
| 14°C | 8min (40%) | 0min (skip) | 0min (skip) | 0min (skip) | 0min (skip) |
| 16°C | 12min (60%) | 10min (50%) | 8min (40%) | 0min (skip) | 0min (skip) |
| 18°C | 16min (80%) | 14min (70%) | 12min (60%) | 10min (50%) | 8min (40%) |
| 20°C (base) | 20min (100%) | 18min (90%) | 16min (80%) | 14min (70%) | 12min (60%) |
| 22°C | 24min (120%) | 22min (110%) | 20min (100%) | 18min (90%) | 16min (80%) |
| 24°C | 28min (140%) | 26min (130%) | 24min (120%) | 22min (110%) | 20min (100%) |
| 26°C | 32min (160%) | 30min (150%) | 28min (140%) | 26min (130%) | 24min (120%) |
| 28°C | 36min (180%) | 34min (170%) | 32min (160%) | 30min (150%) | 28min (140%) |
| 30°C | 40min (200%) | 38min (190%) | 36min (180%) | 34min (170%) | 32min (160%) |
| 32°C | 40min (capped) | 38min (190%) | 36min (180%) | 34min (170%) | 32min (160%) |


A couple of things worth knowing about these numbers:
- With the factory-default 10%/°C sensitivity, the 200% **Upper limit** is reached at exactly
  **30°C** (holding rain at 0mm) — a realistic hot-weather scenario, unlike the much steeper
  temperature that would've been needed at a gentler sensitivity.
- The 14°C/0mm and 16°C/4mm and 18°C/8mm cells all land exactly on the 40% **Lower limit**
  boundary and are *not* skipped — only a result strictly below the limit skips the day.
  Anything colder or rainier than those crosses into skip territory.


