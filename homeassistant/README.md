# Irrigator - Home Assistant setup

This directory is a standard HA "package" plus a Lovelace tab, built against
the actual MQTT contract implemented in `src/main.ino` / `src/network/MqttService.*`.
It implements the four sections requested in `ha.md` using only built-in
Home Assistant integrations and card types - no HACS, no custom cards.

## 1. Find your device's MQTT prefix

The firmware's topic prefix is `irrigator` + the last 2 hex chars of its MAC
(see `makeDevicePrefix()` in `src/main.ino`), e.g. `irrigatorF4`. Every file
in this directory hardcodes `irrigatorF4` as a placeholder - **before doing
anything else, find-and-replace `irrigatorF4` with your real prefix** across:

- `packages/irrigator.yaml`
- `packages/irrigator_automations.yaml`
- `dashboards/garden_tab.yaml`

Confirm the prefix by watching `+/online` on your broker, or checking the
device's serial log / display at boot.

## 2. Install the package

1. Copy `packages/irrigator.yaml` and `packages/irrigator_automations.yaml`
   into your HA config's `packages/` directory (create it if it doesn't
   exist yet - it lives next to `configuration.yaml`). **Keep these exact
   filenames** - HA derives each package's name from its filename, and
   package names must be lowercase/digits/underscores only; renaming a file
   to e.g. `irrigator_F4.yaml` (embedding your device prefix, uppercase
   letter and all) fails to load with "invalid slug". The device prefix
   only needs to be substituted *inside* the files (step 1), never in the
   filename.
2. Merge `configuration_snippet.yaml` into your `configuration.yaml`. Most
   instances already have a `homeassistant:` key - add `packages:` under it
   rather than duplicating the key.
3. This assumes the MQTT integration is already set up and pointed at the
   same broker as the device. If it isn't yet: Settings > Devices & Services
   > Add Integration > MQTT. Nothing in this package needs new `secrets.yaml`
   entries - broker credentials live in that integration's own config entry,
   not in YAML.
4. Restart Home Assistant (packages, template entities, input helpers, and
   automations all need a full restart to pick up for the first time).

## 3. Add the Lovelace tab

Paste the contents of `dashboards/garden_tab.yaml` in place of the existing
empty "Garden" tab (`sections: - type: grid cards: []`) in your dashboard.

## What each section does

1. **Status & Manual Control** - one card per pump/zone (pump first): a
   toggle (its own name IS the live zone name), a "Remaining" row, and a
   1-120 min duration slider. Toggling on calls `script.irrigator_start`
   (publishes `cmd/run` with the slider's current value); toggling off calls
   `script.irrigator_stop`. Cards for disabled zones hide themselves via a
   `conditional` card keyed on that zone's `enabled` flag from `settings.json`.
   The toggle (`switch.irrigator_zone_N`) and "Remaining" (`sensor.irrigator_zone_N_remaining`)
   are two separate entities, both fed by `sensor.irrigator_watering_state`
   (the device's `/state` topic) - template `switch` entities can't carry a
   custom `remaining_min` attribute (see the header comment above the
   `template: switch:` block in `irrigator.yaml`), so it's its own sensor.
   Since the device only ever runs one zone/pump at a time, `/state` reports
   that single active one, and every switch/sensor pair is gated on "is this
   the zone /state currently says is active" - real minutes for whichever
   one is actually running, `0` for all others (shown rather than hidden,
   since entities-card rows can't be individually conditional). See
   "Troubleshooting" below if toggles/remaining time never reflect real
   state.
2. **History** - a single `history-graph` card, 7 days (`hours_to_show: 168`),
   referencing the switches directly - their name is live-templated (see
   #1), and history-graph shows an entity's current registered name
   automatically, so no dashboard-side templating is needed here.
3. **Schedule** - a read-only "Device Schedule" table plus a small editor
   panel (pick a record, edit its fields, Save or Delete) - see "How the
   schedule editor works" below for why this replaced an earlier
   Local-Calendar-based design.
4. **Settings** - schedule-active toggle and watering-percentage number are
   live MQTT entities (direct topic round-trip). Zone name/enabled and
   default-manual-duration republish the *entire* `settings.json` via
   `script.irrigator_publish_settings` - see the comment on that script for
   why a partial payload would be dangerous (the firmware fills any missing
   field with its factory default, not with the value already on flash).
   This is guarded against the same virgin-deploy race as the schedule tab,
   but automatically rather than with a manual toggle - see "How the
   download/sync model works" below. Current-sensor calibration is separate
   from all of this - see #5 below.
5. **Current-sensor calibration** - `valve_current_base_level`/
   `pump_current_base_level` are device-owned (only `cmd/calibrate` ever sets
   them, same as `latitude`/`longitude`/`city`) and read-only in HA: the
   "Irrigator Calibrate" button (`button.irrigator_calibrate`) publishes
   `cmd/calibrate`, and the two base levels update via the next
   `sensor.irrigator_settings` download plus the live
   `sensor.irrigator_calibration_result` (the `cmd/calibrate` response
   itself - fires once per sensor). There's no local `input_number` for
   either value; editing them any other way isn't possible by design. See
   `docs/technical.md`'s "cmd/calibrate" section for the full protocol.

Zone names, everywhere they appear (Status, History, Schedule, Settings),
come from exactly one place: `sensor.irrigator_settings`' `zone_names`
attribute, sourced from the device's `settings.json` - never a hardcoded
name. Until that sensor has real data, most zone-name fields show `#N (no
name)` instead - a deliberate placeholder that doesn't look like a real
name, so it's obvious at a glance when the device data hasn't loaded rather
than silently passing off a fallback as if it were configured. See
"Troubleshooting" if you see that placeholder persist.

**One deliberate exception**: `switch.irrigator_pump`/`switch.irrigator_zone_N`
use `"Irrigator Zone N"` as their fallback instead of `#N (no name)`. Reason:
template `switch` entities don't support `default_entity_id` (see
`irrigator.yaml`'s `template:` header comment), so nothing pins their
`entity_id` directly - but `entity_id` is only ever auto-generated *once*,
at first-ever creation, from whatever `name:` happens to render to at that
exact moment. On a fresh deploy `sensor.irrigator_settings` has no data yet
at that moment, so the fallback text is what actually gets slugified into
the permanent `entity_id` - it has to slug to exactly `switch.irrigator_zone_N`
(what everything else in this package references), which `#N (no name)`
does not. This is a one-time, first-boot-only concern: every later
re-render (once real names arrive, moments later) only updates the
*displayed* name, never the already-frozen `entity_id` - so in practice you
should only ever see "Irrigator Zone N" flash briefly right after the very
first deploy, then never again.

## How the schedule editor works

This section was originally built on Home Assistant's Local Calendar
integration (`calendar.irrigator_zone_1`..`_11`, one event per schedule
block, edited via the calendar's own dialog). That design turned out to be
fundamentally blocked: `calendar.create_event` accepts `summary`,
`description`, `start_date_time`/`end_date_time` (or `start_date`/`end_date`/
`in`), and `location` - **no `rrule` field at all**. True weekly recurrence
is a capability of the calendar dialog's own frontend code, not something
exposed through any callable service, so no automation could ever have
created a genuinely-recurring event, and a schedule made of only
non-recurring single events would have silently gone stale the next time
`calendar.get_events` scanned "this calendar week" and found nothing. If you
still have the 11 Local Calendar integration entries from an earlier version
of this setup, they're no longer referenced anywhere in this package and can
be deleted (Settings > Devices & Services > Local Calendar).

The replacement edits `schedule.json`'s records directly - no calendar
entities involved:

- **`sensor.irrigator_schedule`** mirrors whatever schedule the device
  currently has loaded (its `records` attribute is a plain list of
  `{zone, day_of_week, start_time, duration_min, active}` objects, same
  shape as `ha.md`'s Schedule JSON example) - shown as the read-only
  "Device Schedule" table at the top of the Schedule section, sortable via
  the "Sort Schedule Table By" dropdown (see below for why that's a dropdown
  and not a clickable column header).
- **The editor panel** (`input_select.irrigator_schedule_picker` down to
  `input_boolean.irrigator_edit_active`) lets you pick an existing record or
  "+ Add new", edit zone/day/start time/duration/active, then press **"Save
  Schedule Record"** or **"Delete Schedule Record"**. Both scripts
  (`script.irrigator_schedule_save`/`_delete` in `irrigator.yaml`) read the
  *entire* current record list, apply one change (replace/append/remove),
  and republish the whole array to `cmd/upload/schedule.json` - same
  whole-file-replace contract every other write to this device uses. The
  picker's `[<index>]` prefix and the table's `#` column both refer to the
  same position in `sensor.irrigator_schedule`'s `records` list, so you can
  match a table row to its editor entry at a glance; two automations in
  `irrigator_automations.yaml` ("...rebuild schedule editor dropdown
  options" / "...load selected schedule record...") keep the picker's
  options and the panel's fields in sync with real device data.
- **Per-record "active" pause/resume is preserved directly** - unlike the
  calendar design, `active` is just a checkbox in the editor panel, matching
  the field's real meaning in `schedule.json`.
- **32-record cap.** `script.irrigator_schedule_save` checks the resulting
  record count against the firmware's `SCHEDULE_MAX_RECORDS` (32) before
  publishing, and raises a persistent notification instead of sending an
  oversized array that the firmware would whole-file-reject anyway.
- **No true click-a-column-header-to-sort.** A stock `markdown` card can't
  run JavaScript (HA sanitizes it out), so a real clickable sort isn't
  buildable without a custom/HACS card. `input_select.irrigator_schedule_sort_by`
  is the native equivalent - it only reorders the table's Jinja rendering,
  never touches `sensor.irrigator_schedule` or the device.
- **Dropdown options are built via Jinja's native-type list rendering**
  (a `namespace()` accumulator ending in a single bare `{{ ns.items }}`
  expression, which HA's template engine returns as an actual list rather
  than a stringified one) - this is a standard, common HA automation
  pattern, but if either `input_select.irrigator_schedule_picker` or
  `input_select.irrigator_edit_zone` ever shows one garbled option instead
  of several clean ones, check "Irrigator - rebuild schedule editor dropdown
  options" in `irrigator_automations.yaml` first.

## How the download/sync model works

Per ha.md's preface, settings.json and schedule.json are requested from the
device via MQTT - but only when HA doesn't already have a representation,
not on a recurring poll:

- **`sensor.irrigator_settings`** subscribes to `.../download/settings.json`
  (the response topic for `cmd/download/settings.json`) and simply reflects
  whatever last landed there - from an explicit request, or anyone else
  asking the device for it. There's no topic the device pushes settings.json
  to on its own, but there doesn't need to be: settings.json only ever
  changes via HA in practice (the local rotary/display menu has no
  settings-editing screens - see `irrigator.md`'s menu spec), so once HA has
  a copy, HA's own edits (via `script.irrigator_publish_settings`) are the
  only thing that can make it stale, and HA already knows about those.
  "Irrigator - republish settings on local helper change" (the automation
  that calls that script) is itself guarded on this sensor already holding
  real data, which is what closes the settings-side virgin-deploy race
  automatically: the 15 Settings-tab helpers are created fresh at their
  YAML `initial:` placeholder values on a virgin deploy, and that creation
  fires a state-changed event just like a real edit would. This same sync
  automation also "self-sets" `switch.irrigator_schedule_active` (a
  `template:` entity - see "Offline command queue" below - whose `state:`
  reads the read-only `binary_sensor.irrigator_scheduled_state` MQTT echo
  sensor bound to `/scheduled`, which only ever gets a value when someone
  actively changes it, never as part of a passive settings download, so
  without this it'd sit blank on every fresh boot until manually touched)
  and `input_number.irrigator_watering_percentage` (a plain local helper,
  same as every other Settings-tab field - no echo dance needed there).
- **`sensor.irrigator_schedule`** is a trigger-based template sensor
  listening on *two* topics: `.../schedule` (which the device pushes on
  every change, whether from an HA upload or the device's own rotary/
  display menu) and `.../download/schedule.json` (the request/response
  topic). Since the device already pushes on every change, this sensor
  stays current on its own after the first value arrives - no polling needed.
- **"Irrigator - request initial data on virgin deploy"** (in
  `packages/irrigator_automations.yaml`) fires both on HA startup AND
  whenever `binary_sensor.irrigator_online` turns "on" (covers the device
  reconnecting to the broker after an HA restart, which a startup-only
  trigger would miss entirely if the device isn't back online yet at that
  exact moment), and checks each of the two sensors independently: if
  either is still `unknown` (never received a value), it publishes an empty
  payload to that file's `cmd/download/...` topic to prime it. Once primed,
  it isn't re-requested automatically again.
- The **"Refresh From Device"** button (`button.irrigator_refresh_from_device`,
  on both the Schedule and Settings sections) calls
  `script.irrigator_request_downloads` to force-request `/status` plus both
  files on demand, regardless of virgin status - useful if you suspect drift
  or want to double-check after a manual device-side change.

## Offline command queue

The garage's Wi-Fi is flaky enough that the device drops off `.../online`
often, and the pre-queue behavior was every manual-control switch greying
out via `availability:` whenever that happened - so all the Run switches and
Schedule Active carry **no** `availability:` binding at all now; they're
always clickable. Writing to any of them while the device is offline doesn't
get lost - it's queued and retried until it actually reaches the device.
Watering Percentage is a plain `input_number` helper (not gated by
`availability:` either, since it isn't an MQTT entity at all) - editing it
goes out via the same full-settings republish as every other Settings-tab
field, which is itself queued the same way when offline:

- **`script.irrigator_enqueue_or_publish`** (`topic`, `payload`) is the one
  chokepoint every device-bound write goes through - `irrigator_start`,
  `irrigator_stop`, `irrigator_stop_all`, `irrigator_test_all`,
  `irrigator_publish_settings`, `irrigator_schedule_save`,
  `irrigator_schedule_delete`, and the `turn_on`/`turn_off` of the Schedule
  Active template switch above. If `binary_sensor.irrigator_online` is `on`,
  it publishes immediately - same latency as before this existed. If not, it
  fires an `irrigator_queue_push` event instead of publishing.
- **`sensor.irrigator_command_queue`** is a trigger-based template sensor
  that accumulates `{topic, payload}` entries in its `items` attribute,
  self-referencing its own previous value via `this.attributes.items` on
  every `irrigator_queue_push` (append one) or `irrigator_queue_pop` (drop
  the leading `count` entries). This lives as an *attribute*, not an entity
  `state` string, specifically so it isn't subject to HA's 255-character
  state-value limit - a queued `cmd/upload/settings.json` or
  `cmd/upload/schedule.json` payload can be several hundred bytes to a few
  KB, and a plain `input_text` helper couldn't hold that.
- **"Irrigator - flush pending command queue"** (in
  `irrigator_automations.yaml`) drains the queue whenever the device is
  reachable: triggered both by the online transition (fastest path) and a
  30-second `time_pattern` safety net, so it keeps retrying for as long as
  the device stays online and anything remains queued - not just a single
  attempt on reconnect. It pops exactly one item at a time, immediately
  after publishing it, and re-checks `binary_sensor.irrigator_online` before
  every iteration - so a disconnect mid-batch stops the loop right there
  instead of firing the rest of a stale batch at a device that already
  stopped listening, and only items that actually got published are ever
  removed from the queue.
- There's no per-command delivery acknowledgement from the device for
  `cmd/run` / `cmd/stop` / `cmd/scheduled` / `cmd/upload/settings.json` (see
  `docs/technical.md`'s MQTT section) - `cmd/#` is delivered at QoS 0 in
  practice, and the broker can't hold QoS-0 messages for a disconnected
  client even with a persistent session. "Published to the broker while
  `binary_sensor.irrigator_online` said on" is the best success signal
  available; it's not a true device-side confirmation.
- **`irrigator_schedule_save`/`_delete`** build their new record list on top
  of the *last still-queued* `cmd/upload/schedule.json` payload if one
  exists (found by filtering `sensor.irrigator_command_queue`'s `items` for
  that topic), falling back to `sensor.irrigator_schedule.records` only when
  nothing's queued. This matters because `sensor.irrigator_schedule` only
  updates from the device's own echo on `.../schedule` - which won't arrive
  until it's back online - so without this, a second offline edit would
  silently overwrite the first instead of building on it.
- The dashboard shows `sensor.irrigator_command_queue` (its `state` is the
  pending count) both as a conditional tile at the top of "Status & Manual
  Control" (only visible when non-zero) and in the Diagnostics list, so it's
  obvious when a command hasn't reached the device yet.

## Troubleshooting

**Many/most entities from this package show "not found"/"unavailable" in
Lovelace at once (e.g. `sensor.irrigator_status`, `button.irrigator_stop_all_button`,
even ones with no data dependency of their own):** this means the *whole
package file* failed to load, not that those specific entities are
individually broken. Check **Settings > System > Logs** for a line like
`Setup of package 'X' failed ... Package will not be initialized` or any
`DisallowedExtraPropWarning`/`Invalid config for ...` referencing
`irrigator.yaml` - either one can take out every entity in the file at once.
Common causes: the package filename isn't lowercase (see step 2.1 above), or
a leftover schema issue from hand-editing the YAML. Fix whatever the log
names and reload/restart - don't chase individual "missing" entities until
the log is clean.

**Zone names show `#N (no name)` everywhere instead of real names, or
toggles never reflect the device's actual on/off/watering state:** (once the
above is ruled out) both point to the same root cause -
`sensor.irrigator_settings` and/or `sensor.irrigator_watering_state` never
received real MQTT data. Check, in order:

1. **Developer Tools > States** - look up `sensor.irrigator_settings` and
   `sensor.irrigator_watering_state`. Do they show attributes at all (e.g.
   `zone_names`, `zone`, `on`, `pump`), or none/blank? No attributes means
   no MQTT message has ever arrived on that entity's topic.
2. **Is `irrigatorF4` actually your device's real prefix everywhere?** A
   mismatched prefix in even one of the three files (see step 1 above) means
   that file's entities silently subscribe to a topic nothing publishes to.
3. **Developer Tools > MQTT > Listen to a topic** - subscribe to
   `irrigatorF4/#` (your real prefix) and trigger a manual run from the
   device or its local UI. You should see messages on `.../state` the
   instant a valve/pump starts or stops. If nothing appears there at all,
   the problem is upstream of HA entirely (device not connected to this
   broker, wrong broker, etc.) - if messages DO appear there but the
   entities still don't update, the entity's `state_topic` doesn't match
   what you just saw.
4. Press **"Refresh From Device"** (`script.irrigator_request_downloads` -
   pulls `/status`, `settings.json`, and `schedule.json` in one go) and
   re-check `sensor.irrigator_settings` - if `zone_names` still doesn't
   populate after confirming step 3 shows a real reply on
   `.../download/settings.json`, compare that raw payload against the
   expected shape in `ha.md`'s "Settings" JSON example.

**Entities exist and hold real data (confirmed via Developer Tools > States),
but the entity_id has an unexpected extra prefix** (e.g.
`sensor.ha_irrigator_irrigator_status` instead of `sensor.irrigator_status`),
**and this persists even after restarting HA:** when an MQTT entity is
linked to a `device:`, HA generates its entity_id as `slugify(device_name) +
"_" + slugify(entity_name)`. The `mqtt:` block in `irrigator.yaml` is
written so the device name (`"Irrigator"`) and each linked entity's own name
(`"Status"`, `"Settings"`, etc., deliberately *without* a redundant leading
"Irrigator ") combine into exactly the entity_ids referenced everywhere
else. **But entity_id is only ever auto-generated once, at an entity's
first-ever registration** - if these 8 entities were already created under
an earlier/different device or entity name (e.g. while this package was
still being iterated on), simply fixing the YAML and restarting does NOT
retroactively rename them; HA just reloads the same already-registered
entities and refreshes their state, not their entity_id. Fix it once, by
hand:

- **Rename each one** - Settings > Devices & Services > Entities, search
  `ha_irrigator` (or whatever the wrong prefix is), and for each of these 8,
  open it > gear icon > Entity ID field > change to the target below > Update:

  | Wrong (example) | Rename to |
  |---|---|
  | `binary_sensor.ha_irrigator_irrigator_online` | `binary_sensor.irrigator_online` |
  | `sensor.ha_irrigator_irrigator_status` | `sensor.irrigator_status` |
  | `sensor.ha_irrigator_irrigator_watering_state` | `sensor.irrigator_watering_state` |
  | `sensor.ha_irrigator_irrigator_last_alert` | `sensor.irrigator_last_alert` |
  | `sensor.ha_irrigator_irrigator_queue` | `sensor.irrigator_queue` |
  | `sensor.ha_irrigator_irrigator_settings` | `sensor.irrigator_settings` |
  | `switch.ha_irrigator_irrigator_schedule_active` | `switch.irrigator_schedule_active` |

  (Watering Percentage used to be a `number.` entity affected by this same
  device-linked-naming issue; it's since been replaced by the plain
  `input_number.irrigator_watering_percentage` helper below, which was never
  linked to the `device:` block and so was never subject to this rename.)

- **Or delete the "Irrigator" device** (Settings > Devices & Services >
  Devices > Irrigator > ⋮ menu > Delete) and restart - since it's an
  MQTT-config-defined device with no separate config entry tying it down,
  it should recreate cleanly with correct entity_ids. Fall back to the
  manual table above if it doesn't fully clear.

This one matters more than it looks: `sensor.irrigator_settings` existing
under its *expected* name is a prerequisite for "Irrigator - sync device
settings to local helpers" (its trigger watches that exact entity_id) and
for every `state_attr('sensor.irrigator_settings', ...)` lookup throughout
this package - so a wrong entity_id here is very likely also the reason
zone names, calibration, and other settings-derived fields silently stay at
their defaults, not a separate bug.

**Your editor/IDE flags things like `Device '*irrigator_device' does not
exist` or `Device 'variables:' does not exist`:** these are not from Home
Assistant - check **Settings > System > Logs**; if it's clean, the package
loaded and parsed successfully, which wouldn't be possible if these were
real errors. This is almost certainly a VS Code HA extension (or similar
YAML-linting tool) misreading constructs it doesn't fully support: the
`device: *irrigator_device` YAML *anchor alias* (resolved by the YAML parser
before HA ever sees it, which some lightweight linters don't do), and nested
`variables:` steps inside a `repeat:` block. Safe to ignore as long as the
real HA log is clean.

**A schedule dropdown (`input_select.irrigator_schedule_picker` or
`input_select.irrigator_edit_zone`) shows one long garbled option instead of
several clean ones:** the automation that builds its option list relies on
HA's Jinja native-type rendering returning an actual list from a
`namespace()` accumulator (see "How the schedule editor works" above) -
check "Irrigator - rebuild schedule editor dropdown options" in
`irrigator_automations.yaml` and confirm via Developer Tools > Actions >
"Input select: Set options" that a plain list, not a string, is what's being
sent.

## Entity naming reference

- `switch.irrigator_pump`, `switch.irrigator_zone_1`..`_11` - manual on/off
  control, name tracks the zone's configured name live (see the "deliberate
  exception" note above for why its fallback text differs from everywhere
  else).
- `sensor.irrigator_pump_remaining`, `sensor.irrigator_zone_1_remaining`..`_11_remaining`
  - remaining minutes of the current run, `0` unless this zone/pump is the
    one currently active.
- `input_number.irrigator_pump_duration`, `..._zone_N_duration` - manual run
  duration, 1-120 min.
- `switch.irrigator_schedule_active` - `template:` entity, state read from a
  read-only MQTT echo sensor, writes routed through the offline command
  queue (see "Offline command queue" below) rather than a direct
  `command_topic`.
- `switch.irrigator_auto_adjustment` - master on/off for weather-based
  auto-adjustment, same `template:`-entity-over-echo-sensor pattern as
  `switch.irrigator_schedule_active` immediately above (backed by
  `binary_sensor.irrigator_auto_adjustment_state`, writes via
  `cmd/auto_adjustment`).
- `input_text.irrigator_zone_N_name`, `input_boolean.irrigator_zone_N_enabled`,
  `input_number.irrigator_default_manual_duration`,
  `input_number.irrigator_watering_percentage` - settings that require a full
  `settings.json` republish (see above).
- `button.irrigator_calibrate` - publishes `cmd/calibrate`; requires an active
  watering run (valve open) or the device rejects it via `/alert`.
- `sensor.irrigator_calibration_result` - live `cmd/calibrate` response
  (fires once per sensor: `sensor`, `base_level`, `sample_count`,
  `burst_samples_mv` as attributes). Read-only, not part of the
  `settings.json` republish - `valve_current_base_level`/
  `pump_current_base_level` are device-owned, same as
  `latitude`/`longitude`/`city`.
- `input_select.irrigator_schedule_picker`, `input_select.irrigator_edit_zone`,
  `input_select.irrigator_edit_day`, `input_datetime.irrigator_edit_start_time`,
  `input_number.irrigator_edit_duration_min`, `input_boolean.irrigator_edit_active`
  - the schedule editor panel's fields - see "How the schedule editor works"
  above.
- `input_select.irrigator_schedule_sort_by` - display-only sort order for
  the "Device Schedule" table.
- `button.irrigator_refresh_from_device`, `button.irrigator_save_schedule_record`,
  `button.irrigator_delete_schedule_record`, `button.irrigator_stop_all` -
  manual triggers for the corresponding scripts.
- `sensor.irrigator_status`, `sensor.irrigator_watering_state`,
  `sensor.irrigator_last_alert`, `sensor.irrigator_queue`,
  `sensor.irrigator_schedule`, `binary_sensor.irrigator_online` - read-only
  device telemetry.
