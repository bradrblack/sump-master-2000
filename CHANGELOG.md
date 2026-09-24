# Changelog

Notable changes to the Sump Master 2000 firmware, newest first. Versions refer
to `FIRMWARE_VERSION` in `src/sm2k.ino`.

Sump Master 2000 is a fork of the vibration-monitor project, taken at
`2026-09-22r1`. Entries below that point are the original project's history.

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
