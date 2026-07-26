# HA-Irrigator

## What is it?

An intuitive and human-friendly irrigation controller: 11 sprinkler zones + a shared pump, driven by a
rotary-encoder + OLED local UI and a full MQTT API for Home Assistant integration. 

## Why is it?

HA-Irrigator fulfills traditional industry lacks of watering control systems.

| Trouble | Solution |
|---|---|
| 1.5V or 9V Battery - the settings, the schedule and the clock is backed up with regular batteries. You never know when the battery is dead and leaks into the device. And when the battery is dead and the mains fails, you lose all the schedule. This forces you to stand again in from og your irrigator, recall what the program was and configure everything from scratch. | We keep all the settings and schedule in non-volatile memory. It does not require battery. The clock is working on batter which persists for 10 years and NEVER leaks. Your device is safe from investment perspective and your precious time is saved: no more re-configurations. |
| You cannot really manage any irrigator without reading manuals and standing in frnt of the device for long time.  | HA-Irrigator has 0 (zero, nothing) configuration in from of the device. And when you approach it for manual commands, it helps you with on-screen hints.  |
| You never know if industrial irrigator followed you program. You only hope until your garden dries and dies.  | HA-Irrigator provides you precise and exact logging of what and when happened.  |
| You must adjust the watering by season time-to-time. If you forget, your garden suffers from lack of water or your pocket suffers from excessive expense.  | HA-Irrigator has weather awareness. You just set the base of watering per zone and from here the system takes control: when it is too dry the watering is increased propoertionally. When it is rain you save the water (and the cost).  |
| Industrial irrigator forces you to approach it and stand in front of it to see its status, configure its programs.  | We provide you a power to see and change everything from your computer or mobile phone. More than that, it can be integrated with your Smart home. So your home can configure the irrrigator if you wish. |
| Traditionally, irrigator have at least 4 buttons and/or also a dial with weird labels. Confusing and not intuitive.  | This device has 1 (only one) knob. You rotate and pushit like the volume knob in your car. And it helps you with navigation on the screen if you get lost.  |
| Manual control of normal irrigator is timed out. It takes your command after 10-30 seconds.  | HA-Irrigator does not waste your time. Your command is executed immediately. If you don't appreciate half minute of your life wasted, then this feature is meaningless :)  |
| Ususally, if you set different manual command to normal irrigator, the current watering is being aboretd. | HA-Irrigator assumes that every command is important. It puts all your command in line. When the command turn arrives it is being executed. Just fire and forget. |
| Industrial irrigator has limit of programs with limit of entries per program. Why? | We hate limits. Feel free to build weirdest schedule ever, we accomodate that! |
| Even if industrial irrigator provides your "smart" access and features it only works either you're clode to it (over Bluetooth) or over the vendore remote server (forget your privacy!) | HA-Irrigator is your private system. You run it, you access it as you want and prefer, you configure it, you're under control! |

## Feature overview

- **11 independently-named sprinkler zones** plus pump-only runs, with the pump always
  pre-started ~500ms before a valve opens and kept on for a 60s graceful-shutdown window after a
  valve closes (protects the pump from rapid-cycle wear) — if another watering is queued up, it
  hands off to the still-running pump immediately instead of waiting out that window. Each zone
  can also be individually disabled (`settings.json`'s `zone_names.<n>.enabled`) to exclude it
  from scheduled, manual, and MQTT-triggered watering alike — an MQTT `cmd/run` against a disabled
  zone is rejected with a `/alert` instead of running, and the local UI's manual zone picker (see
  below) skips disabled zones entirely so they can't be dialed up in the first place.
- **Weekly schedule** (`schedule.json`, almost endless count of records) with a per-zone watering-percentage
  multiplier, a pause/rain-skip mechanism, and a 15-deep queue so overlapping or MQTT-triggered
  runs during an active watering don't get lost — they queue and run in order, back-to-back on
  the same pump cycle whenever the pump is still spinning down from the previous one.
- **Local UI**: 2.4 inch OLED Display and knob, auto-dimming after 20s of inactivity, full menu for stopping, viewing/adjusting the schedule, and settings.
- **MQTT**: King feature, allowing to connect HA-irrigator to practically ANY Snamrt home system. Provides status/availability reporting (Home-Assistant-compatible LWT), remote run/stop/schedule control, current-fault alerting, OTA updates, calibration, and file backup.
- **Captive-portal setup**: first boot (or after a factory-reset-style config wipe) serves a
  Wi-Fi AP with a setup page; it tests both Wi-Fi *and* MQTT connectivity before committing the new config, and rolls back to AP mode if either fails.
- **DS3231-backed clock** with NTP sync and automatic EU DST switching (last Sunday of March/October, 01:00 UTC).
- Give the names to your zones. Numbers are nice but words play better for humans.
- Configurable time for testing of all zones.


## Device


## Screenshots


## Useful (further reading)
- User guide of the HA-Irrigator, incuding installation instructions.
- User-guide for HA integration.
- User guide for HA.
- Technical explanations (for geeks only).
- OTA firmware upgrade (for makers only).