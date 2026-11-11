# Changelog

Notable changes to the Sump Master 2000 firmware, newest first. Versions refer
to `FIRMWARE_VERSION` in `src/sm2k.ino`.

Sump Master 2000 is a fork of the vibration-monitor project, taken at
`2026-09-22r1`. Entries below that point are the original project's history.

## 2026-11-11r2

- **Web page on the board** (`http://sm2k.local/`, port 80): live pump state,
  vibration (with a chart against the thresholds), water depth, temperature,
  humidity and Wi-Fi; calibration readings (idle average and peak since boot,
  the last run's average and lowest, suggested thresholds); the last 120 log
  lines; and a settings form. `GET /api/status` (JSON), `GET /api/log?since=N`.
  Requests are served once per 250 ms window, between vibration samples.
- **Calibration settings changeable without reflashing:** both vibration
  thresholds, both confirm times, sensor-to-floor distance, high-water depth
  and its clear distance, and the expected drop. Saved in NVS (`c_*` keys);
  the `platformio.ini` flags are now the defaults. Saving needs HTTP digest
  authentication (`admin` / the OTA password). Values are range-checked; a
  change is logged and recorded as a `sump_config` point. Changing the
  sensor-to-floor distance shifts the current depth instead of making it jump.
- **Vibration reported to InfluxDB:** the 10-minute `sump` point adds
  `rms_idle_avg`, `rms_idle_max`, `rms_run_avg`, `rms_run_min` (each only if
  there were such windows) and the thresholds in force, `rms_on` / `rms_off`.
  A `stop` event adds that run's `rms_avg` and `rms_min`. Windows seen while a
  start or stop is being confirmed are counted once the outcome is known.
  `TG_LINE_LEN` 224 -> 320 for the longer line.
- Dashboard: new panel "Vibration: idle and running, against the thresholds"
  and a "Settings changed" annotation.
- The once-a-second status line stays on serial only (not in the web log).
- (r2) The page shows the pump state and HIGH WATER as two badges; in r1 the
  alarm replaced the pump state.

## 2026-11-10r3

- Vibration thresholds raised to ON 0.50 / OFF 0.30 m/s^2 RMS (were 0.40 /
  0.20), in `platformio.ini` (its `-D` flags override the sketch defaults; r1
  changed only the sketch, so it had no effect). The ADXL346's idle noise floor
  on the bench is ~0.21 (louder windows up to ~0.37 with typing on the same
  desk), right on the old OFF threshold, so after a bump started a "run" it
  never saw 3 s of continuous quiet and stayed RUNNING indefinitely.
  Provisional until calibrated against the real pump.
- Status line adds `max=` (loudest 250 ms window in that second) and `loud=`
  (windows at or above OFF_RMS_THRESHOLD), since `rms=` shows only one window
  in four.

## 2026-11-08r5

- Accelerometer: a failed 400 Hz read now counts as "no signal" (never as
  vibration), and 200 in a row (0.5 s) marks the accelerometer lost, instead of
  waiting for the 30 s watchdog. Samples are read in one 6-byte burst.
- Accelerometer watchdog also checks the measure-mode bit, and an exactly
  constant signal for 10 s marks it lost (it answered its ID but had stopped
  measuring, so the pump looked idle forever).
- AHT20: after a failed read it retries every 2 s (five times) before backing
  off to once a minute, so `air=--` no longer lasts a minute after a cold boot.
- Level: the first reading after boot must be confirmed by a second burst, so
  one bad echo (e.g. a loose contact) can't raise the high-water alarm.

## 2026-11-08r4

- NeoPixel status LED on GPIO 8 (replaces the on/off LED): green = all clear,
  blue = pump running, flashing red = high water with the pump off, flashing
  purple = high water with the pump running.
- I2C runs at 100 kHz (400 kHz was unreliable on the bench). The accelerometer
  is looked for up to three times at boot, restarting the bus between tries.
- The JSN-SR04T Echo pin has a weak pull-down so a disconnected Echo can't fire
  the interrupt from radio noise. The level sensor runs from 3.3 V with no
  Echo divider.
- Hardware note: 470 uF across 3V3/GND at each I2C sensor fixed the intermittent
  bus failures and boot crashes seen on the bench (5 of 5 cold power-ups clean).

## 2026-09-24r3

- Quieter logs while sensors are missing: the AHT20 is retried once a minute
  (not every 10 s) while it isn't answering, and "no reliable echo" is logged
  at most once a minute.
- Telegraf send failures log the error name (e.g. "connection refused").

## 2026-09-24r2

- Run without the accelerometer instead of waiting for it at boot (and
  rebooting every 30 minutes). If it's missing at boot or stops answering,
  pump detection pauses, everything else carries on, one push says so, and it's
  retried every 30 s with I2C bus recovery. It no longer reboots the board.
- New `pump_sensor` field in the 10-minute reading.

## 2026-09-24r1

First Sump Master 2000 firmware (not yet tested on hardware).

- Fork from vibration-monitor as a separate project. Rename the sketch
  `src/vibration-monitor.ino` to `src/sm2k.ino`.
- Remove Adafruit IO. Pump events and 10-minute readings go to Telegraf on the
  LAN as InfluxDB line protocol over HTTP, queued on the board if Telegraf is
  down (`telegraf/sump.conf` replaces the Adafruit IO config).
- Add JSN-SR04T water level (median of 5-ping bursts, speed of sound corrected
  for temperature, blind-zone handling, suspect-jump filter) and AHT20
  temperature/humidity (CRC-checked).
- Support the GY-346 (ADXL346) accelerometer (device ID 0xE6) as well as the
  ADXL345.
- ntfy: real-time pump on/off (no one-minute window; the stop includes the
  run time), high-water alarm repeated every 30 minutes with an all clear,
  "pump ran but the level didn't drop" alarm, daily report before the 3 AM
  reboot (replaces the nightly reboot push), sensor and Telegraf outage alerts.
  Pushes are queued and retried.
- OTA updates pushed from the Mac (`pio run -e sump-ota -t upload`), accepted
  only while the pump is idle, with automatic rollback if the new firmware isn't
  healthy. Partition table changed to `min_spiffs.csv`: the first flash must be
  over USB.

## Inherited from vibration-monitor

## 2026-09-22r1

- Cap the Adafruit IO client's network timeout at 5 s. A stalled DNS lookup,
  TCP connect, or TLS handshake could chain across the library's default 3 s
  per-call timeout and exceed the 30 s task watchdog before failing — this
  happened at least once in the field (task watchdog reboot on the pump
  device).

## 2026-09-21r12

- Make serial logging non-blocking (`Serial.setTxTimeoutMs(0)`), so a USB
  host that's attached but not reading can no longer stall the main loop.
  Excess output is dropped instead of blocking.

## 2026-09-21r10

- Add a 30 s hardware task watchdog as a backstop for a hung main loop
  (stuck I2C, network, or USB call) that the existing in-loop watchdogs
  can't see. A hang now resets the board and is reported via ntfy as an
  unexpected reset.
- Add the `[env:dryer]` PlatformIO build environment.

## 2026-09-20r9

- Publish JSON events via MQTT (Adafruit IO) and add I2C boot diagnostics.
- Fix banner formatting.
- Pin the PlatformIO platform release to work around an upload bug in the
  unpinned "stable" zip.

## 2026-09-20r3

- Generalize from a single sump-pump monitor to a multi-device appliance
  monitor with JSON events.

## 2026-09-20r1

- Add source files, update README, add Telegraf/Grafana setup for an
  InfluxDB appliance timeline (non-firmware, tooling only).

## 2026-09-19r1

- Add reliability hardening (WiFi/Adafruit IO watchdogs, scheduled nightly
  reboot, boot-state correction) and ntfy.sh reboot notifications.

## Initial version

- Add the ESP32-C3 sump pump vibration monitor.
