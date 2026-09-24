# Sump Master 2000

An ESP32-C3 sump pump monitor. It detects the pump running from its vibration,
measures the water level and the air temperature/humidity, sends everything to
InfluxDB (via Telegraf on the LAN) for a Grafana timeline, and pushes pump
events, alarms and a daily report to your phone with [ntfy.sh](https://ntfy.sh).

It's a fork of the vibration-monitor project (the ESP32-C3 + ADXL345 appliance
monitor), made sump specific. There's no cloud service in the data path: Adafruit
IO is gone. [PLAN.md](PLAN.md) has the design and the remaining open questions.

The sketch is `src/sm2k.ino`, with small drivers for the level sensor
(`src/jsn_sr04t.*`) and the AHT20 (`src/aht20.*`).

## What it does

- **Pump on/off:** a GY-346 (ADXL346) accelerometer clamped to the pump or the
  discharge pipe. Samples at 400 Hz, high-passes each axis (~13 Hz cutoff) to
  reject gravity, footsteps and house rumble, and computes RMS over 250 ms
  windows. `RUN_CONFIRM_MS` of continuous vibration above `ON_RMS_THRESHOLD`
  starts a run; `STOP_CONFIRM_MS` of quiet below `OFF_RMS_THRESHOLD` ends it
  (3 s each).
- **Water level:** a JSN-SR04T waterproof ultrasonic sensor under the pit lid.
  Every 5 s it takes the median of a burst of 5 pings (one per 250 ms window, so
  vibration sampling never waits), corrected for the speed of sound at the
  measured air temperature. Level is reported as water depth above the pit
  floor.
- **Temperature/humidity:** an AHT20 on the same I2C bus, read every 10 s and
  CRC-checked.
- **Data to InfluxDB:** pump start/stop as it happens, and every 10 minutes the
  level, temperature, humidity and pump activity. See [Data](#data).
- **ntfy pushes:** see [Notifications](#notifications).
- **OTA updates** pushed from the Mac over Wi-Fi, with automatic rollback if the
  new firmware isn't healthy. See [Updating](#updating-firmware).

The onboard LED is off when quiet, solid while any vibration is detected (e.g.
picking the board up), and blinks while the pump is considered running.

Serial output (115200) prints a status line once a second:
`rms=0.012 state=idle level=12.3cm air=17.9C/71% wifi=up tgq=0` (`tgq` is the
number of points waiting to be sent to Telegraf).

## Hardware and wiring

| Part | Connection |
|------|------------|
| GY-346 (ADXL346) SDA / SCL | GPIO 5 / GPIO 6 |
| AHT20 SDA / SCL | GPIO 5 / GPIO 6 (same bus) |
| JSN-SR04T Trig | GPIO 3 (direct) |
| JSN-SR04T Echo | GPIO 4 **through a voltage divider** (Echo is 5 V) |
| JSN-SR04T 5V / GND | SuperMini 5V (USB) / GND |
| LED | GPIO 8 (onboard, active low) |

GPIO 2, 8 and 9 are boot strapping pins on the C3, so nothing else uses them.

**GY-346 note:** the ADXL346 is register-compatible with the ADXL345 and uses
the same Adafruit library, but reports device ID `0xE6` rather than `0xE5`, which
the library's `begin()` rejects. The firmware accepts either chip and enables
measurement itself.

**Echo divider:** Echo → 1 kΩ → GPIO 4, and GPIO 4 → 2 kΩ → GND (≈3.3 V; 2.2 kΩ /
3.3 kΩ also works). Some board versions output only 3.3 V on Echo: measure it
first, and if so wire it straight to GPIO 4. [PLAN.md](PLAN.md#jsn-sr04t-wiring)
has the diagram and details.

**Mounting the level sensor:** the JSN-SR04T is blind closer than about 20-25 cm
and can then report a double echo (about twice the real distance), which would
read as "low" just as the pit overflows. Mount the transducer **at least 30 cm
above the highest level the water can reach** (the pit rim), on a bracket or
riser. Readings closer than 25 cm are treated as "at or above the limit", never
as a real distance.

## Setup

1. **Secrets:**
   ```
   cp src/secrets.h.example src/secrets.h
   cp secrets.ini.example secrets.ini
   ```
   `src/secrets.h` holds Wi-Fi, the Telegraf URL and the ntfy topic;
   `secrets.ini` holds the OTA password (letters and digits), used both to build
   the firmware and to authenticate uploads. Both are git-ignored.
2. **Pit settings** in `platformio.ini` (`[env:sump]`): `SENSOR_TO_FLOOR_CM`,
   `HIGH_WATER_CM`, `HIGH_WATER_HYST_CM`, `MIN_DROP_CM`. The values there are
   placeholders until the sensor is mounted; see [Calibration](#calibration).
3. **Telegraf:** copy `telegraf/sump.conf` to `/etc/telegraf/telegraf.d/` on the
   InfluxDB host, add the variables from `telegraf/sump.env.example` to
   Telegraf's environment file (use a write-only token) and restart Telegraf.
   It listens on port 8186 for the board's POSTs.
4. **ntfy:** subscribe to your `NTFY_TOPIC` in the ntfy app.
5. **First flash over USB** (it installs the OTA partition table):
   `pio run -t upload`.

## Updating firmware

After the first USB flash, update over Wi-Fi from the Mac:

```
pio run -e sump-ota -t upload
```

- The board is found as `sm2k.local` (change `upload_port` in `platformio.ini` to
  an IP address if mDNS doesn't resolve).
- Updates are only accepted while the pump is idle (an upload pauses sampling
  for ~15 s). If the pump is running the upload fails with no response; run it
  again once the pump stops.
- **Rollback:** a new image runs as a trial. After 3 minutes with Wi-Fi up and
  every sensor that was working before the update working again, it's marked
  good and ntfy says `sump updated A -> B`. If it isn't healthy within 10
  minutes, crashes, or reboots during the trial (including failing to join
  Wi-Fi), the bootloader switches back to the previous firmware and ntfy says
  `update rolled back` with the reason.
- Bump `FIRMWARE_VERSION` in `src/sm2k.ino` for each release.
- Recovery if all else fails: USB flash (`pio run -t upload`).

## Data

The board POSTs InfluxDB line protocol to Telegraf's `http_listener_v2`
(`TELEGRAF_URL`), timestamped with its own NTP-synced clock, so late or buffered
points land at the right time. All points are tagged `device=sump`.

| Measurement | When | Fields |
|---|---|---|
| `sump` | every 10 min, on the clock | `level_cm`, `distance_cm`, `level_status` (`ok`/`near_limit`/`missing`), `temp_c`, `humidity`, `running`, `pump_sensor` (1 = accelerometer answering), `cycles` and `run_s` (for the 10 minutes), `rssi`, `heap` |
| `sump_event` | pump start/stop | `event`, `running`, `seconds` (stop), `level_cm` (start); `note="restart"` on a post-reboot correction, `note="sensor_lost"` if the accelerometer dropped out mid-run |
| `sump_cycle` | after each run | `seconds`, `level_before_cm`, `level_after_cm`, `drop_cm`, `result` (`ok`/`no_drop`/`unverified`) |
| `sump_alarm` | high water on/off | `type`, `active`, `level_cm` |
| `sump_boot` | each boot | `version`, `reason` |

If Telegraf can't be reached, points queue on the board (150 lines, about a day
of readings plus events) and are sent in order when it's back; if the queue
fills, the oldest are dropped. The queue is in RAM, so a reboot loses it.

## Grafana

**Dashboard** (`grafana/sump-dashboard.json`, InfluxDB 2.x with Flux): import it
(Dashboards > New > Import), pick your InfluxDB (Flux) data source, then set the
`bucket` variable. Keep the `high_water_cm` and `min_drop_cm` variables equal to
the firmware's `HIGH_WATER_CM` and `MIN_DROP_CM`. All panels share one time
axis:

- Current values: pump on/off, water depth, air temperature, humidity, pump
  cycles in the range, and time since the last data.
- Pump and high-water state timeline.
- Water depth, with the level before and after each pump run and a dashed
  high-water line.
- Run time per cycle and level drop per cycle (a slow rise in run time, or a
  shrinking drop, is an early warning).
- Pump cycles and run time per day (US Eastern days).
- Temperature and humidity.
- Tables of recent pump cycles and of alarms and reboots, plus high-water and
  reboot annotations on every graph.
- Board health: Wi-Fi RSSI and free heap.

The JSON is generated by `tools/make_dashboard.py`: edit that and re-run it
rather than editing the JSON by hand.

**"Sump monitor silent" alert** (`grafana/sump-alerting.yaml`): fires to ntfy
when no sump data has arrived for 30 minutes, which covers what the board can't
report itself (no power, no Wi-Fi, Telegraf down, a hang). It sends a
"resolved" push when data returns.

- **Provisioning:** fill in the three `CHANGE_ME` values (ntfy topic, data
  source UID, bucket), copy the file to Grafana's `provisioning/alerting/`
  directory and restart Grafana. The rule routes to its own contact point, so
  your notification policies are untouched.
- **Or in the UI:** create a Webhook contact point with the URL
  `https://ntfy.sh/<topic>?tpl=yes&t={{.title}}&m={{.message}}&p=4&ta=warning`
  (ntfy's template mode turns Grafana's webhook JSON into a normal push). Then
  create an alert rule on the Flux query in the YAML file, with the condition
  "A is below 1", evaluated every 5 minutes, "Alert state if no data" set to
  **Alerting**, and the contact point selected under notifications.

## Notifications

All pushes go to `NTFY_TOPIC` (leave it out of `secrets.h` to disable them; they
are then only logged). They're queued and retried if the internet is down.

| Push | Priority |
|---|---|
| `sump started` / `sump stopped` in real time; the stop includes the run time, e.g. `Stopped at 2026-09-24 09:12:30 EDT (ran 28.8 s)` | default |
| **High water**: as soon as the level passes `HIGH_WATER_CM` (or reaches the sensor's limit), then every 30 minutes until it falls `HIGH_WATER_HYST_CM` below, then one "all clear" | urgent |
| **Pump ran but the level didn't drop** by `MIN_DROP_CM`: before/after levels and run time (clogged discharge, stuck check valve, pump running dry). If there was no level reading around the run: "can't verify" | high |
| **Daily report** just before the 3 AM reboot: pump state, water level, temperature and humidity, cycles/run time/longest run/highest water since the last report, firmware version | low (silent) |
| Sensor not responding (accelerometer, level sensor, AHT20), and recovered | high |
| Telegraf unreachable for 30 minutes, and reachable again | default |
| Exception reboots with the reason, e.g. `sump rebooted: WiFi down watchdog (reboot #2)` | default |
| OTA updated / rolled back | default / high |

ntfy.sh has per-IP rate and daily message limits for anonymous use, shared with
anything else on your public IP. Every pump cycle is two pushes, so check the
limits against a heavy wet-season day.

## Reliability

Borrows the self-healing approach from the
[RF ceiling fan remote](https://github.com/bradrblack/rf-ceiling-fan-remote)
firmware: recover on its own where possible, and never fail silently.

| Mechanism | Behavior |
|-----------|----------|
| **Bounded Wi-Fi connect at boot** | If Wi-Fi isn't up within 30 s, reboot and retry instead of hanging. |
| **Wi-Fi watchdog** | If Wi-Fi drops it resets the radio and retries every 30 s; if it stays down for 2 minutes, reboot. |
| **Telegraf outage** | Doesn't reboot (that can't fix the server): data queues and one push says so. |
| **Accelerometer optional** | Every 30 s the accelerometer's device ID is read (without this a dead sensor reads as zeros and looks like an idle pump). If it's missing at boot, or fails three checks in a row, the board **keeps running without it**: pump on/off detection pauses, while water level, alarms, reporting and OTA carry on. One push says so, it's retried every 30 s with I2C bus recovery (clocking SCL to release a bus a sensor is holding low), and a push says when it's back. A run in progress when it drops out is closed with `note="sensor_lost"` and no run time. |
| **Level and AHT20 watchdogs** | Push an alert when there's been no reliable echo for 2 minutes, or no AHT20 reading for 5 minutes, and again when they recover. |
| **Suspect level readings** | A jump of more than 15 cm between readings is held until the next reading confirms it. |
| **Task watchdog** | A 30 s hardware watchdog resets a hung main loop; the reset is reported on the next boot. |
| **Nightly reboot** | 3 AM local time (`REBOOT_HOUR`, US Eastern via `TZ_STRING`, DST-aware) to guard against slow heap fragmentation, after sending the daily report. Held off while the pump runs, high water is active, a push is waiting or an OTA trial is running. The "already rebooted today" day is stored in flash so it can't reboot-loop within the hour. |
| **Unexpected reset detection** | A crash, watchdog or brownout reset is detected on the next boot and pushed. Power cycles, the reset button, flashing and deliberate restarts are not. |
| **Stuck "running" correction** | If the board reboots mid-run and the pump is idle 15 s after boot, a `stop` (with `note="restart"`, no run length) is recorded and pushed so nothing implies it's still running. |

## Calibration

- **Vibration thresholds:** watch the `rms=` values over serial with the pump
  idle and running, and set `ON_RMS_THRESHOLD` / `OFF_RMS_THRESHOLD`.
- **Pit:** set `SENSOR_TO_FLOOR_CM` to the measured distance from the sensor to
  the empty pit floor. Every pump run is logged over serial (and in
  `sump_cycle`) as `Pump cycle: on at X cm, off at Y cm, dropped Z cm`, so after
  a few real cycles set `HIGH_WATER_CM` a few cm above the normal "on" level and
  `MIN_DROP_CM` well below the normal drop.

## Notes and gotchas

- Wi-Fi TX power is capped (`WIFI_TX_POWER`, 15 dBm). At full power this
  ESP32-C3 mini board joined Wi-Fi on only ~1 in 3 boots. If a board is flaky
  at 15 dBm, drop to `WIFI_POWER_8_5dBm`.
- Log lines are timestamped (wall-clock time once NTP syncs, uptime before
  that) and the boot banner prints `FIRMWARE_VERSION`.
- The platform is pinned (`55.03.37`) because the unpinned "stable" release's
  esptool crashed mid-upload; don't unpin it.
- The USB port name changes each time the board re-enumerates; close any
  serial monitor before flashing, and restart the monitor after the board
  reboots.
