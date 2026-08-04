# Integrating with Home Assistant

Pairing the Irrigator with Home Assistant gets you:

- Easy configuration from a proper UI instead of the device's tiny screen — zone naming, scheduling, defaults.
- Remote control and status from anywhere, not just standing at the unit.
- Watering history you can look back over.

Everything below uses only Home Assistant's built-in integrations and stock Lovelace cards — you don't need any custom/community cards for any of it.

## What you'll need

- Home Assistant already running, with its **MQTT integration** connected to the same broker your
  Irrigator is configured to use (the same broker address you entered during the device's Wi-Fi setup — see [usage_irrigator.md](usage_irrigator.md)). If you haven't set that up yet: **Settings  > Devices & Services > Add Integration > MQTT**, and point it at your broker.
- Your device's **MQTT topic prefix**, e.g. `irrigatorF4` (`irrigator` + the last 2 hex chars of its MAC — see `makeDevicePrefix()` in `../src/main.ino`). Find it on the device: long-click the knob, go to **Settings**, and read the **Topic** line — or confirm it by watching `+/online` on your broker, or checking the device's serial log at boot. You'll need this in a moment.
- This repository, downloaded or cloned somewhere you can copy files from (the ready-made configuration lives in its `homeassistant/` folder).

## Installing HACS (optional)

You don't need this for anything described in this guide — the dashboard below is built entirely from cards that ship with Home Assistant. Only install HACS if you think you'll want to add community/custom Lovelace cards later (nicer-looking button grids, graphs, etc.):

1. Go to [hacs.xyz/docs/use/download/download](https://hacs.xyz/docs/use/download/download/) and follow the installer instructions for your type of Home Assistant install (OS, Container, Core, or Supervised).
2. Restart Home Assistant when prompted.
3. Go to **Settings > Devices & Services > Add Integration**, search for **HACS**, and follow the prompts — this includes signing in with a GitHub account, which HACS uses to pull in community add-ons.

That's it — HACS itself is just the installer/updater for custom content; it doesn't add anything to your dashboard on its own.

## Step 1: install the Irrigator package

The repo's `homeassistant/` folder is a ready-to-use Home Assistant "package" (a self-contained bundle of entities, automations, and scripts) plus a dashboard tab — no need to hand-write any entity YAML yourself.

1. **Find and replace the placeholder device prefix.** Every file below hardcodes `irrigatorF4` as a stand-in — before copying anything, open each of these three files and replace every `irrigatorF4` with your real prefix from **What you'll need** above:
   - `homeassistant/packages/irrigator.yaml`
   - `homeassistant/packages/irrigator_automations.yaml`
   - `homeassistant/dashboards/garden_tab.yaml`
2. **Copy the package files.** Copy `irrigator.yaml` and `irrigator_automations.yaml` into your Home Assistant config's `packages/` folder (it lives right next to `configuration.yaml` — create the folder if it doesn't already exist). Keep these exact filenames; Home Assistant derives each package's internal name from its filename, and names must be lowercase letters/digits/underscores only.
3. **Merge the configuration snippet.** Open `homeassistant/configuration_snippet.yaml` and merge its contents into your real `configuration.yaml`. Most existing configs already have a `homeassistant:` key — add the `packages:` line underneath it rather than creating a second `homeassistant:` block.
4. **Restart Home Assistant** (Settings > System > Restart). Packages need a full restart to load for the first time, not just a config reload.

## Step 2: add the dashboard tab

Open `homeassistant/dashboards/garden_tab.yaml` and paste its contents in place of an empty dashboard tab (in the dashboard editor, add a new view/tab, switch it to YAML mode, and replace the empty `sections: - type: grid cards: []` with the file's contents).

## What you get

- **Status & manual control** — one card per zone (and the pump), each with an on/off toggle showing the zone's real configured name, a remaining-time readout, and a duration slider (1–120 min). Toggling a zone on/off talks to the device the same way the local rotary menu does. Only one zone (or the pump) ever actually runs at a time — toggling a second one on while another is already running queues it rather than starting it immediately, same as it would from the local UI or the weekly schedule; it starts automatically the moment the first one finishes.
- **History** — a 7-day graph of every zone/pump's on/off history.
- **Schedule** — a read-only table of the device's current weekly schedule, plus an editor panel to add, change, or remove individual schedule records. The device holds up to 255 records in total — far more than a real weekly schedule needs — a limit set by how the device counts and indexes programs internally, not by storage space.
- **Settings** — zone names, per-zone enable/disable, default manual duration, the schedule on/off switch, the watering-percentage multiplier, and current-sensor calibration (including the 4 over/under-current alert toggles, which also forward to WhatsApp and the logbook when tripped — see **Current-fault alerting** in [technical.md](technical.md)), all synced live with the device.

Zone names shown everywhere come directly from the device's own configuration — until it's finished loading (a few seconds after Home Assistant starts), you may briefly see a placeholder like `#3 (no name)` instead of the real name.

## Setting the WeatherAPI.com fallback key manually

The dashboard's **WeatherAPI.com Key** field (Settings section) is the normal way to set this — click directly on that row (not the gear/settings icon, which only edits entity registry metadata and will tell you the entity "cannot be edited from the UI"; that's expected for every package-defined helper here, not an error) and Home Assistant pushes it to the device automatically. This section is for setting it **without** Home Assistant, or for confirming what HA is doing under the hood.

The key lives in `settings.json`'s `Watering` group, which the device merges field by field on upload (see **JSON file formats** in [technical.md](technical.md)) — so setting just this one field is a single publish, no read-edit-write round-trip needed:

```
mosquitto_pub -t irrigatorF4/cmd/upload/settings.json -m '{"Watering":{"weatherapi_key":"YOUR_KEY_HERE"}}'
```

Every other field (zone names, `schedule_active`, calibration, etc.) is left exactly as it was on flash — only fields you actually include, with a non-null non-empty value, get changed.

Get your key from [weatherapi.com](https://www.weatherapi.com) (free tier is enough) — it's only used as a fallback when [Open-Meteo](https://open-meteo.com)'s keyless request fails (see **Weather auto-adjustment** in [technical.md](technical.md)), so a wrong or empty key doesn't break normal operation, just the fallback path.

If you're running the HA package too: HA caches its own copy of the whole file (`sensor.irrigator_settings`) and re-uploads it wholesale whenever any *other* Settings-tab field changes (zone name, calibration, etc.) — so a manual edit made outside HA can get silently overwritten the next time you touch anything in HA, until HA's cache catches up. Press **Refresh From Device** on the dashboard right after a manual edit to pull the new key into HA's cache and avoid that.

## Troubleshooting

If most or all Irrigator entities show as **unavailable** right after setup, the package likely failed to load entirely (rather than each entity being individually broken) — check **Settings > System > Logs** for a line mentioning `irrigator.yaml` or "Package will not be initialized". The most common cause is a leftover `irrigatorF4` that didn't get replaced with your real device prefix in one of the three files from **Step 1**.

For anything past that — zone names stuck on placeholders, entities registered under an unexpected ID, or issues with the schedule editor — see the much more detailed troubleshooting section in [usage_homeassistant.md](usage_homeassistant.md), which walks through each of those scenarios step by step.
