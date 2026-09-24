# Working notes

Handoff notes for the next session: the current state, how things were
checked, and what's still to do. The design is in [PLAN.md](PLAN.md) and the
how-to in [README.md](README.md). **Read this file and PLAN.md first.**

Last updated 2026-09-24.

## Current state

- **Board:** firmware `2026-09-24r3`, flashed over USB, on the main Wi-Fi
  network (it was on the guest network first, which can't reach
  192.168.3.252). **No sensors attached yet.** Unplugged at the end of the
  session to test the Grafana "silent" alert.
- **Telegraf:** `telegraf/sump.conf` is installed and working on
  192.168.3.252:8186, path `/sump` (an empty POST from the Mac returns 204).
- **Grafana (192.168.3.252:3000):** the dashboard is imported and showing boot
  records, RSSI and heap. Whether the "Sump monitor silent" alert
  (`grafana/sump-alerting.yaml`) is installed, and whether it fired, **is
  unconfirmed**: ask the user.
- **Repo:** github.com/bradrblack/sump-master-2000, branch `main`, all
  committed and pushed.

## Next steps

1. Confirm the "silent" alert fired while the board was unplugged, and that the
   "resolved" push arrived after plugging it back in.
2. Attach the sensors: GY-346 accelerometer, AHT20, and JSN-SR04T with the Echo
   divider (measure Echo first; see PLAN.md, JSN-SR04T wiring).
3. Mount the level sensor 30+ cm above the pit rim, measure the sensor-to-floor
   distance, and set `SENSOR_TO_FLOOR_CM` in `platformio.ini` (currently a
   95 cm placeholder).
4. Calibrate the vibration thresholds from `rms=` over serial, with the pump
   idle and running.
5. From the first real pump cycles (serial `Pump cycle: on at X cm, off at Y cm`,
   or the dashboard's on/off points), set `HIGH_WATER_CM` (placeholder 45) a few
   cm above the normal "on" level and `MIN_DROP_CM` (placeholder 5) well below
   the normal drop. Keep the dashboard variables `high_water_cm` and
   `min_drop_cm` equal to them.
6. **First OTA test:** `pio run -e sump-ota -t upload` with the sensors attached
   and the pump idle. Check for the "updated A -> B" push after about 3 minutes.
   Then test a rollback, e.g. a build that never reaches Wi-Fi, which should be
   rolled back with a push.

## Not yet verified on hardware

Everything sensor-related: the ADXL346 path (`beginAccel()` accepts device ID
0xE6), the JSN-SR04T interrupt timing and 20 µs trigger pulse, AHT20 reads and
CRC, the high-water and pump-didn't-drop alarms, the daily report at 3 AM, and
OTA plus rollback. What was verified: the firmware builds without warnings,
Wi-Fi works, Telegraf receives boot points and 10-minute readings, the Grafana
panels show RSSI, heap and boots, and the AHT20 CRC-8 matches the test vector
(0xBE 0xEF -> 0x92).

## Things learned this session

- **GY-346 = ADXL346.** It's register-compatible with the ADXL345, but its
  device ID is 0xE6. The Adafruit ADXL345 library's `begin()` rejects anything
  but 0xE5, so `beginAccel()` enables measurement itself (POWER_CTL = 0x08)
  when it sees 0xE6.
- **The OTA health check is based on "sensors healthy before the update".** r1
  counted a missing accelerometer as healthy (`sensorFails == 0`), so an OTA
  from r1 to r2 would have rolled back; r2 was flashed over USB. From r2 on,
  health uses `accelOk`.
- **Rollback support:** pioarduino 55.03.37 (Arduino core 3.3.7) builds with
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`. The sketch overrides the core's
  weak `verifyRollbackLater()` to return true and marks the image valid itself
  in `checkOtaTrial()`. ArduinoOTA's upload runs inside `handle()`, so
  `onProgress` feeds the 30 s task watchdog.
- **Firmware size:** 1.24 MB, which wouldn't fit the default 1.25 MB app slot
  with room to spare, hence `min_spiffs.csv` (1.9 MB slots).
- **With no sensors attached, the core logs `i2cRead() ... ESP_ERR_INVALID_STATE`**
  on every retry. It's harmless; r3 retries the AHT20 once a minute while it's
  missing, and the accelerometer every 30 s.
- **The Grafana dashboard JSON is generated:** edit `tools/make_dashboard.py`
  and run `python3 tools/make_dashboard.py`, then re-import (overwrite). The
  user prefers the board-health panel as a single dual-axis graph (RSSI left,
  heap right).
- **Two y-axes with one or two points** put both dots in the same spot
  (each axis autoscales around its own value). That's expected, not a data bug.
- **Placeholder secrets for test builds:** `src/secrets.h` and `secrets.ini` are
  now the user's real files. Never overwrite them. Build as-is.

## Open questions and ideas (not agreed yet)

- A local buzzer for high water, as a fallback when the internet (and so
  ntfy.sh) is down.
- A second, higher "critical" water level that repeats more often.
- Check the ntfy.sh anonymous per-IP limits against a heavy wet-season day
  (every cycle is two pushes, shared with the `pump` board).
- The Telegraf queue is RAM only, so a reboot loses anything unsent.
- The old `pump` board (vibration-monitor project) is still reporting to
  Adafruit IO while its reliability testing continues; it will be renamed
  later.
