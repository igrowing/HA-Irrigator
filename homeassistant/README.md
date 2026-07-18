# Irrigator - Home Assistant setup

This directory is a standard HA "package" plus a Lovelace tab, built against
the actual MQTT contract implemented in `src/main.ino` / `src/network/MqttService.*`
(not the older `irrigator.md`/`dev.md` notes, which predate the move off ESPHome).
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

## 3. Create 11 Local Calendar integration entries (one-time, manual, UI only)

Local Calendar is a config-entry integration - it has no YAML schema at all
(confirmed against the current integration docs), so these can't be
provisioned by this package the way everything else is. This is a real,
unavoidable one-time manual step, once per zone:

For each zone 1-11: **Settings > Devices & Services > Add Integration >
Local Calendar**. Name it so you'll recognize it (e.g. "Irrigator Zone 1 -
Front Lawn") - the calendar file name it asks for doesn't matter, but the
resulting **entity ID** does; open the new entry's entity and set it
explicitly to:

```
calendar.irrigator_zone_1
calendar.irrigator_zone_2
...
calendar.irrigator_zone_11
```

These exact entity IDs are what `packages/irrigator.yaml`,
`packages/irrigator_automations.yaml`, and `dashboards/garden_tab.yaml`
reference - if they don't match, the schedule tab and the publish-to-device
automation silently do nothing.

## 4. Add the Lovelace tab

Paste the contents of `dashboards/garden_tab.yaml` in place of the existing
empty "Garden" tab (`sections: - type: grid cards: []`) in your dashboard.

## 5. Before you touch the Schedule tab: populate or check, then arm it

The 11 calendars you just created in step 3 are empty. Auto-publish to the
device is deliberately **off by default** (`input_boolean.irrigator_schedule_sync_enabled`,
the "Schedule Auto-Publish Armed" tile at the top of the Schedule section) -
arming it while the calendars are still empty would publish an empty
`schedule.json` and erase whatever schedule is already running on the
device. Before turning it on, do ONE of:

- Press **"Populate Calendars From Device"** once - it seeds all 11
  calendars from the device's current `schedule.json` automatically (see
  `script.irrigator_populate_calendars_from_device` in `irrigator.yaml` for
  what it assumes about the `calendar.create_event` service - worth a quick
  glance at Developer Tools > Actions to confirm those field names match
  your HA version before relying on it), **or**
- Manually re-create each block from the "Device Schedule (last known)"
  table at the bottom of the Schedule section into the matching zone's
  calendar.

Only then, turn on "Schedule Auto-Publish Armed". If the device has no
schedule yet, there's nothing to lose - arm it immediately. The "Publish
Schedule Now" button always works regardless of this toggle, since pressing
it is itself a deliberate, explicit action.

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
3. **Schedule** - a `type: calendar` card showing all 11
   `calendar.irrigator_zone_N` entities together (each auto-colored) - see
   "How the schedule sync works" below.
4. **Settings** - schedule-active toggle and watering-percentage number are
   live MQTT entities (direct topic round-trip). Zone name/enabled,
   default-manual-duration, and calibration all republish the *entire*
   `settings.json` via `script.irrigator_publish_settings` - see the comment
   on that script for why a partial payload would be dangerous (the
   firmware fills any missing field with its factory default, not with the
   value already on flash) and why calibration goes through the same script
   rather than the firmware's narrower `cmd/set_calibration` topic. This is
   guarded against the same virgin-deploy race as the schedule tab, but
   automatically rather than with a manual toggle - see "How the download/
   sync model works" below.

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

## How the schedule sync works, and its limitations

- **HA -> device**: every 2 minutes (`time_pattern` automation, not a
  reactive trigger - `calendar` entities don't fire a state-changed signal
  for every event add/edit/delete the way the old `schedule.*` helper did),
  `script.irrigator_publish_schedule_from_calendars` queries all 11
  calendars for the current week via `calendar.get_events`, rebuilds the
  full `schedule.json` array, and publishes it to `cmd/upload/schedule.json`
  - but only while `input_boolean.irrigator_schedule_sync_enabled` is on
  (see step 5 above). A "Publish Schedule Now" button forces this
  immediately regardless of that toggle.
- **Device -> HA**: `sensor.irrigator_schedule` mirrors whatever the device
  currently has loaded (shown as a read-only table at the bottom of the
  Schedule section), fed automatically. **"Populate Calendars From Device"**
  (step 5) is a *one-time* convenience that reads that same data and calls
  `calendar.create_event` on each of the 11 calendars to seed them - unlike
  the earlier Schedule-helper design, this direction is *not* fundamentally
  blocked, since `calendar.create_event`/`delete_event` are real callable
  services. Re-running it after calendars already have events will create
  duplicates (there's no "only if it doesn't already exist" check), so treat
  it as a one-shot action right after creating the 11 calendars, not
  something to run repeatedly.
- **Per-record "active" pause/resume isn't preserved 1:1.** The original
  `schedule.json` record shape has a per-entry `active` boolean for pausing
  one row without deleting it. This design has no equivalent - an event
  either exists or doesn't. Records built from HA always publish with
  `active: true`; to pause an entire zone, use its "Enabled" checkbox in
  Settings (which the firmware already gates all activity on, independent
  of the schedule).
- **32-record cap.** The firmware whole-file-rejects `cmd/upload/schedule.json`
  if the combined array (all 11 zones, this week's occurrences) exceeds
  `SCHEDULE_MAX_RECORDS` (32). `script.irrigator_publish_schedule_from_calendars`
  checks this before publishing and raises a persistent notification instead
  of sending an oversized array that would otherwise wipe the device's
  schedule with a rejected upload.
- **Per-event duration is clamped to 1-60 min** to match
  `SCHEDULE_DURATION_MIN/MAX_MINUTES` - an event outside that range gets
  clamped rather than rejecting the whole publish.
- **Field names need a one-time check.** I could not confirm the exact
  `calendar.get_events` response shape or `calendar.create_event` field
  names (`summary`/`start_date_time`/`end_date_time`/`rrule`) against
  current HA documentation from outside a running instance - both scripts
  in `irrigator.yaml` say exactly what to check in Developer Tools >
  Actions and what happens if the assumption is wrong (publish fails toward
  "does nothing", not toward corrupting data, but do check once).

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
  automatically, with no manual arm toggle needed (unlike the schedule tab -
  see step 5 above): the 15 Settings-tab helpers are created fresh at their
  YAML `initial:` placeholder values on a virgin deploy, and that creation
  fires a state-changed event just like a real edit would. This same sync
  automation also "self-sets" `switch.irrigator_schedule_active` and
  `number.irrigator_watering_percentage` to whatever settings.json says -
  those two are direct MQTT entities whose own topics (`/scheduled`,
  `/watering_percentage`) only ever get a value when someone actively
  changes them, never as part of a passive settings download, so without
  this they'd sit blank on every fresh boot until manually touched.
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
  | `number.ha_irrigator_irrigator_watering_percentage` | `number.irrigator_watering_percentage` |

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
exist`, `Device 'variables:' does not exist`, or `Device 'action:
calendar.create_event' does not exist`:** these are not from Home Assistant
- check **Settings > System > Logs**; if it's clean, the package loaded and
parsed successfully, which wouldn't be possible if these were real errors.
This is almost certainly a VS Code HA extension (or similar YAML-linting
tool) misreading constructs it doesn't fully support: the `device: *irrigator_device`
YAML *anchor alias* (resolved by the YAML parser before HA ever sees it,
which some lightweight linters don't do), and the nested `variables:`/
`action: calendar.create_event` steps inside a `repeat:` block. Safe to
ignore as long as the real HA log is clean.

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
- `switch.irrigator_schedule_active`, `number.irrigator_watering_percentage`
  - direct MQTT round-trip.
- `input_text.irrigator_zone_N_name`, `input_boolean.irrigator_zone_N_enabled`,
  `input_number.irrigator_default_manual_duration` - settings that require a
  full `settings.json` republish (see above).
- `input_number.irrigator_valve_zero_offset_mv` / `_valve_mv_per_ma` /
  `_pump_zero_offset_mv` / `_pump_mv_per_ma` - current-sensor calibration,
  also folded into the full `settings.json` republish (see above).
- `calendar.irrigator_zone_1`..`_11` - created manually per step 3 above.
- `input_boolean.irrigator_schedule_sync_enabled` - "Schedule Auto-Publish
  Armed" toggle, off by default - see step 5 above.
- `button.irrigator_refresh_from_device`, `button.irrigator_publish_schedule_now`,
  `button.irrigator_populate_calendars_from_device_button`,
  `button.irrigator_stop_all_button` - manual triggers for the corresponding
  scripts.
- `sensor.irrigator_status`, `sensor.irrigator_watering_state`,
  `sensor.irrigator_last_alert`, `sensor.irrigator_queue`,
  `sensor.irrigator_schedule`, `binary_sensor.irrigator_online` - read-only
  device telemetry.
