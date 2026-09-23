# Sump Master 2000 — plan

Design plan for the sump-specific additions to the fork of vibration-monitor.
Nothing below is implemented yet unless marked done. Last updated 2026-09-23.

## Hardware

- **Board:** ESP32-C3 SuperMini (4 MB flash), as in vibration-monitor.
- **Pump running:** ADXL345 accelerometer on I2C (GPIO 5 SDA, 6 SCL), unchanged.
- **Water level:** JSN-SR04T waterproof ultrasonic sensor, mounted under the pit lid.
  - It is blind closer than about 20-25 cm and can then report a double echo
    (about twice the real distance), which would read as "low" just as the pit
    overflows. **Mount the transducer at least 30 cm above the highest level the
    water could physically reach** (the pit rim or floor level), using a short
    standpipe or bracket if needed.
  - 5 V supply; the Echo line needs a voltage divider down to 3.3 V. GPIO pins
    to be chosen (avoid 8/9).
- **Temperature/humidity:** AHT20 on the existing I2C bus, 3.3 V, mounted outside
  the pit.
- **No float switch for now.** It can be added later on a spare GPIO as an
  independent high-water check.

## Firmware

### Level measurement

- Measured every ~5 s. The echo is timed in the background (interrupt or RMT),
  so the 400 Hz vibration sampling never pauses.
- Each reading is the median of a burst of pings, corrected for the speed of
  sound using the AHT20 temperature.
- A reading near the sensor's minimum range counts as "at or above the limit",
  never as a real distance.
- A sudden jump is marked suspect rather than believed. Missing echoes over a
  sustained period send a sensor-failure alert.
- Pit geometry (sensor-to-water distances at the pump's on and off levels, and
  the high-water threshold) are build flags, set once the pit is measured.

### Temperature/humidity

- AHT20 readings are CRC-checked, and its status/calibration bit is watched as
  a dead-sensor check (like the ADXL345 device-ID watchdog).

### Alarms (ntfy)

- **High water:** alert when the level passes the threshold (a blind-zone reading
  also counts). Hysteresis of a few cm so one event gives one alert, then an
  "all clear" when it drops back.
- **Pump ran but the level didn't drop:** compare the level just before a run
  with the level just after. If it fell less than a minimum amount (a few cm,
  tuned from real runs), alert with both readings and the run length (clogged
  discharge, stuck check valve, pump running dry). If readings around the run
  are missing or suspect, report "can't verify" instead of guessing.
- Both alarms are sent regardless of the `NOTIFY_RUN_STATE` start/stop pushes.

### Reporting

- **Device name:** `sump` (`[env:sump]`). The existing `pump` board keeps running
  under its own name while its reliability testing continues, and will be
  renamed later.
- **Pump ON/OFF events:** unchanged, on the shared `appliance-events` feed as
  `"device":"sump"`.
- **Readings:** every 10 minutes, one JSON message on a new Adafruit IO feed
  named `sump`: water level, temperature, humidity, pump cycles and run time for
  the period. JSON only, no separate numeric feeds or Adafruit IO gauges.
- **Telegraf:** a new config subscribes to the `sump` feed and writes the
  readings to InfluxDB. **Grafana:** new panels for level, temperature and
  humidity, alongside the existing appliance timeline.

### OTA updates

- Push OTA from the Mac over the home network using ArduinoOTA, password in
  `secrets.h`, via a new `[env:sump-ota]` build environment. No pull OTA.
- Partition table changes to `min_spiffs.csv` (1.9 MB per app slot; the default
  1.25 MB is too tight). This can't be changed over the air, so **the first flash
  of the new firmware must be over USB**.
- Updates are refused while the pump is running or events are queued.
- Automatic rollback: new firmware boots in trial mode and is marked good only
  after a few minutes of health (Wi-Fi and Adafruit IO up, all sensors
  answering); otherwise the board returns to the previous firmware. Check whether
  the pioarduino bootloader has rollback enabled; if not, implement it with a
  boot counter in NVS, checked first thing in `setup()`.
- The hardware watchdog is fed during the upload, and the nightly reboot is held
  off during an update.
- ntfy reports updates ("updated A → B") and rollbacks.
- No backup Wi-Fi network and no USB extension cable: the device is
  inconvenient but not hard to reach.

### Kept from vibration-monitor

All existing watchdogs (Wi-Fi, Adafruit IO, I2C sensor, 30 s task watchdog), the
3 AM nightly reboot, and reboot-reason notifications.

## Housekeeping

- `platformio.ini` has `default_envs = pump`, but the only environment is
  `[env:sump]`; change it to `sump`.
- The sketch file is `src/sm2k.ino`, but the README and CHANGELOG say
  `src/sm3k.ino`. Settle on one name.
- The README's "Notes and gotchas" still describes the esptool upload crash and
  the esptool 4.x workaround; the pinned platform (55.03.37) fixed that.

## Open questions

- Pit dimensions: sensor mounting height, the pump's on/off levels, and the
  high-water threshold.
- GPIO pins for the JSN-SR04T Trig/Echo.
