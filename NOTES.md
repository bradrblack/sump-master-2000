# Working notes

Handoff notes for the next session: the current state, how things were
checked, and what's still to do. The design is in [PLAN.md](PLAN.md) and the
how-to in [README.md](README.md). **Read this file and PLAN.md first.**

Last updated 2026-11-08 (see "Session 2026-11-07" below; it supersedes the
sensor and wiring status in "Current state").

## Current state

- **Board:** firmware `2026-09-24r3`, flashed over USB, on the main Wi-Fi
  network (it was on the guest network first, which can't reach
  192.168.3.252). All three sensors are now attached on the bench (see
  "Session 2026-11-07" and the 2026-11-08 results).
- **Telegraf:** `telegraf/sump.conf` is installed and working on
  192.168.3.252:8186, path `/sump` (an empty POST from the Mac returns 204).
- **Grafana (192.168.3.252:3000):** the dashboard is imported and showing boot
  records, RSSI and heap. The "Sump monitor silent" alert is provisioned
  (InfluxDB data source UID `afal3n4wvrx8ga`, bucket `data`) and **fired** at
  10:13:30 EDT with the board unplugged (state "Alerting (NoData)"). The
  contact-point test push arrived via ntfy. The "resolved" push after plugging
  the board back in is **unconfirmed**.
- **Repo:** github.com/bradrblack/sump-master-2000, branch `main`, all
  committed and pushed.

## Session 2026-11-07: bench bring-up of the I2C sensors (UNCOMMITTED test build)

**State at the end of the session:** the board is running overnight on the
bench, with the serial monitor writing `logs/device-monitor-*.log`
(`pio device monitor --filter log2file`). The accelerometer (ADXL346) and the
AHT20 both read. The JSN-SR04T level sensor is **not wired to this board yet**
(`level=--`, "level sensor not responding" push at 2 min is expected).
NeoPixel status LED on GPIO 8 works: green = all clear, blue = pump running
(both tested), red = high water (not tested; needs the level sensor).

**Cleaned up and committed as `2026-11-08r4` (DIAG lines, the AHT20 100 ms wait and the 8.5 dBm TX power are gone; I2C stays 100 kHz). The board is still running the r3 test build until it is reflashed. README.md and PLAN.md were updated on 2026-11-08 for the new wiring.** The list below is what was removed: Temporary or
unproven changes still in it:
- `src/aht20.cpp` and `src/sm2k.ino`: `// DIAG` logging lines (AHT20 raw
  bytes, device-ID reads in `beginAccel()`, and heap/I2C probes plus a bus
  re-init around every ntfy push in `serviceNtfy()`). Remove them; the bus
  re-init after a push is risky with this I2C driver.
- `I2C_CLOCK_HZ = 100000` (was 400000). 50000 made the 400 Hz sampling loop
  run late. **Never tested: 400 kHz with the capacitors fitted.** Try it.
- `WIFI_TX_POWER = 8.5 dBm` (was 15). Made no difference to the I2C problem;
  probably revert.
- `sm2k.ino` `serviceAht()`: a 100 ms wait before reading the AHT20. It wasn't
  the fix; remove.
- Kept on purpose: `INPUT_PULLDOWN` on the JSN Echo pin (a floating Echo with
  an edge interrupt is a bad idea), and the NeoPixel code (`rgbLedWrite`, replaces
  the old on/off LED and `ledOn`).
- New in `setupSensor()`: if the accelerometer isn't found at boot, stop the
  bus, recover it and restart it, up to 3 times, before falling back to the
  30 s retry.

**Hardware changes that mattered:**
- The sensors are now wired directly to the board's pins (no breadboard), with
  **a 470 uF capacitor across 3V3/GND at each sensor**. The caps are the most
  likely real fix for the intermittent bus failures (the AHT20's first read
  started working at once). Keep them.
- The original AHT20+BMP280 board was probably damaged when it was rewired (it
  ACKed its address but returned no data); a new one worked at once. The
  BMP280 half (0x77) is unused.
- The accelerometer's CS and SDO pins are unconnected and it works (the module
  has pull resistors). Tying CS to 3V3 and SDO to GND is still worth doing if the
  dropouts come back.
- JSN-SR04T newer model run from 3.3 V: Echo goes **straight to GPIO 4, no
  divider**. Re-test the I2C sensors for stability once it's connected (its
  ping current is on the same 3V3 rail).
- The "ADXL 362/346" board is an ADXL346 on I2C (0x53, ID 0xE6), not the
  SPI-only ADXL362.

**What went wrong, and what is and isn't known:**
- Symptoms: I2C `ESP_ERR_INVALID_STATE` floods, the ID read returning 0x00 or a
  garbage 0x63, both sensors dying together, and (twice, on cold power-ups) a
  crash inside the ESP-IDF I2C master RX interrupt (`i2c_isr_receive_handler`).
- **The cause is not proven.** An early theory ("sending an ntfy push kills the
  bus") was contradicted by a later log, so don't rely on it. Best guess: the
  shared 3.3 V rail or the bus lines were marginal (long jumpers, small
  regulator); the slower clock and the caps helped.
- A restart of the bus (stop, recover, start) has recovered the sensors every
  time it was tried. The first read after a bus start often fails or returns
  0x63; the second works. The retry loops handle that.
- One unrelated crash seen once: `assert failed: sys_untimeout ... Required to
  lock TCPIP core` from `NetworkManager::hostByName` inside `sendNtfy` (the
  first push after boot). Not reproduced; the board rebooted and recovered.
- The 400 Hz sampling loop spends a minute hammering a dead bus before the
  3-strike watchdog (30 s checks) declares the accelerometer lost.

**Firmware weaknesses found and fixed in `2026-11-08r5` (code written, bench-testing in
progress; see CHANGELOG):** stuck-at-zero accelerometer (measure bit + constant-signal
check), failed 400 Hz reads counted as strikes (and as no signal, not vibration, which
also removes the false "Pump cycle ... unverified" records), AHT20 quick retries after a
failed read, and the first level reading after boot must be confirmed. The earlier claim
that NTP once took ~160 s was wrong: the wait is capped at 10 s and real syncs took 0.5-3 s.

**2026-11-08 results:** the overnight run was clean (14 h, 3 AM reboot fine, one I2C error); level sensor wired to 3V3/GPIO3/GPIO4 with no divider and reads correctly; high-water alarm, red LED, ntfy push and "all clear" push all verified; 5 of 5 cold power-ups clean with all three sensors. Not yet tried: 400 kHz. A PCB design brief is in `hw/DESIGN.md`.

**Tomorrow (written 2026-11-07):** (1) the overnight log: look for crashes, the 3 AM daily reboot,
and dropouts; (2) do 4-5 cold power-ups (unplug 20-30 s) and count crashes and
"restarting the bus" lines; (3) try 400 kHz with the caps; (4) then clean up
the test build and commit, and update the README/PLAN.md for the new wiring
(direct wires, caps, 3.3 V JSN with no divider, NeoPixel).

## Session 2026-11-08 (later): PCB and case rev A

Generated the carrier schematic and routed board (`hw/tools/gen_carrier.py` ->
`hw/sump_carrier.kicad_*`) and the case (`hw/case/sump_case.scad` -> tray/lid
STLs). ERC/DRC clean. **Nothing ordered or printed yet.** Decisions: 2 caps
(7 x 12 mm, 2.5 mm pitch), 75.5 mm board, no SMD/jumpers/slot, 6 case-pin holes
incl. two under the ESP, strap tunnels. The SR04 is turned so its own pins
point at the carrier: female Dupont ends on them, male ends soldered into J3;
it snaps into 4 clips under the lid. GY-346, AHT20, caps, USB height and RESET
position and the SR04 outline/header/JST are measured; SR04 holes, transformer height and Dupont length are still placeholders.
Checklist and decisions: `hw/DESIGN.md` section 8. Case is 114 x 47 x 28 mm. Browser viewer: claude.ai
artifacts 4VSqGyKoZyfA9BqPo54RWs (case) and JjcE6u5YUD6zGgqs6JdeMw (PCB, KiCad GLB export) (built from scratchpad files, not in the repo).
Workflow: edit the tables in the script/SCAD and re-run; once you start
hand-editing in KiCad, stop re-running the generator (it overwrites).
Small leftover: `src/sm2k.ino` line ~87 comment still says Echo goes through a
5 V divider.

## Next steps

1. Confirm the "resolved" push arrives after plugging the board back in (the
   "silent" alert itself has fired).
2. Sensors are on the bench (3.3 V JSN, no divider). Remaining: mount them in
   place and rerun the I2C stability checks on the final wiring/PCB.
3. Mount the level sensor 30+ cm above the pit rim, measure the sensor-to-floor
   distance, and set `SENSOR_TO_FLOOR_CM` in `platformio.ini` (currently a
   95 cm placeholder).
4. Calibrate the vibration thresholds from `rms=` over serial, with the pump
   idle and running. Bench idle noise floor (2026-11-10, board on a desk) is
   ~0.20 m/s^2 RMS (p99 0.31), so OFF must stay well above 0.20; `2026-11-10r3`
   uses ON 0.50 / OFF 0.30. **Set them in `platformio.ini`** (its `-D` flags
   override the sketch defaults). If the real pump reads below ~0.6 in the pit, the
   noise floor there will decide how low these can go.
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
- **Grafana templates the webhook URL itself.** The first "silent" alert failed
  with `can't evaluate field title in type *templates.ExtendedData`: Grafana
  evaluated ntfy's `{{.title}}` placeholder in the URL. Escaping it
  (`{{"{{.title}}"}}`) failed the same way. The URL now uses ntfy's built-in
  `?template=grafana`, with no braces (tested by posting a sample Grafana
  payload to a throwaway ntfy.sh topic). That template ignores `p=4`, so
  alert pushes arrive at default priority. Verified from Grafana.
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
