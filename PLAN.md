# Sump Master 2000 — plan

Design plan for the sump-specific additions to the fork of vibration-monitor.
Last updated 2026-09-24.

## Status

- **Firmware: written** (`2026-09-24r1`), builds cleanly, **not yet tested on
  hardware**.
- **Telegraf config: written** (`telegraf/sump.conf`).
- **To do:** Grafana sump dashboard and the "no sump data" alert; hardware
  bring-up and calibration (pit dimensions, pump on/off levels).

## Hardware

- **Board:** ESP32-C3 SuperMini (4 MB flash), as in vibration-monitor.
- **Pump running:** GY-346 module (ADXL346 accelerometer) on I2C (GPIO 5 SDA,
  6 SCL). Register-compatible with the ADXL345 and driven by the same Adafruit
  library, but its device ID is 0xE6 (not 0xE5), which the library's `begin()`
  rejects; the firmware accepts either.
- **Water level:** JSN-SR04T waterproof ultrasonic sensor, mounted under the pit lid.
  - It is blind closer than about 20-25 cm and can then report a double echo
    (about twice the real distance), which would read as "low" just as the pit
    overflows. **Mount the transducer at least 30 cm above the highest level the
    water could physically reach** (the pit rim or floor level), using a short
    standpipe or bracket if needed.
  - **Trig on GPIO 3, Echo on GPIO 4** (through a voltage divider). Wiring and
    divider details are in [JSN-SR04T wiring](#jsn-sr04t-wiring) below.
- **Temperature/humidity:** AHT20 on the existing I2C bus, 3.3 V, mounted outside
  the pit.
- **No float switch for now.** It can be added later on a spare GPIO as an
  independent high-water check.

### Pit

- About 2 ft (61 cm) deep, empty at the moment.
- The highest the water can reach is the rim (beyond that it floods the
  floor), so with the 30 cm rule **the transducer sits at least 30 cm above the
  rim**, e.g. on a bracket or riser on the lid. Distance to the empty pit floor
  is then about 90+ cm, well within the sensor's range (it reads to several
  metres).
- If a pipe is used as the riser, keep it wide (4" or more) or test it: the
  sensor's beam is fairly wide, and a narrow pipe can return echoes from its
  own walls.
- High water is then roughly "distance from sensor under ~40 cm" (rim + 30 cm
  + margin); the exact threshold is set once the pump's normal on level is
  known.

### JSN-SR04T wiring

The sensor runs on 5 V and its Echo output is 5 V, but ESP32-C3 pins take
3.3 V at most. A two-resistor divider brings Echo down: Echo goes through R1
to the GPIO, and R2 goes from that GPIO to ground.

```
  JSN-SR04T                                ESP32-C3 SuperMini
  ---------                                ------------------
   5V   ─────────────────────────────────── 5V  (USB 5 V pin)
   GND  ─────────────────────────────────┬─ GND
   Trig ─────────────────────────────────┼─ GPIO 3   (direct, no divider)
                                         │
   Echo ───[ R1 1 kΩ ]───┬───────────────┼─ GPIO 4
                         │               │
                      [ R2 2 kΩ ]        │
                         │               │
                         └───────────────┘  (to GND)
```

The GPIO sees 5 V × R2 / (R1 + R2) = 5 × 2 / 3 ≈ 3.3 V.

| R1 (Echo side) | R2 (to GND) | GPIO sees |
|---|---|---|
| 1 kΩ | 2 kΩ (or two 1 kΩ in series) | 3.33 V |
| 2.2 kΩ | 3.3 kΩ | 3.0 V |
| 10 kΩ | 20 kΩ | 3.33 V |

Any of these works: the ESP32 reads anything above about 2.5 V as HIGH, and
2.2 kΩ / 3.3 kΩ leaves a little more margin below 3.3 V. Keep the resistors
between about 1 kΩ and 20 kΩ; much higher and the Echo edges get sloppy.

Notes:

- **Trig needs no divider.** It goes from the ESP32 to the sensor, and 3.3 V is
  enough to trigger it.
- **Shared ground** between the sensor and the ESP32 is required, or the divider
  reads garbage.
- **Power from the SuperMini's 5V pin** (USB). The sensor isn't reliable on 3.3 V.
- **Pins:** GPIO 2, 8 and 9 are boot strapping pins on the C3 and GPIO 5/6 are
  the I2C bus, so they're avoided. GPIO 3/4 also leave GPIO 0-5 free for
  deep-sleep wake later.
- **Placement:** put the divider next to the ESP32, not at the sensor end. The
  sensor's driver board sits near the ESP32; only the probe's own cable (about
  2.5 m) runs into the pit.
- **Board mode:** on v3.0 boards, leave the mode resistor pad (R27) empty. That's
  the default Trig/Echo mode the firmware uses.
- **Check before connecting GPIO 4:** power the sensor and measure the middle of
  the divider with a meter. It should read 0 V at rest and never more than about
  3.4 V. Some board versions output only 3.3 V on Echo, which the divider would
  bring down to about 2.2 V, too close to the HIGH threshold. If Echo measures
  3.3 V without the divider, wire it straight to GPIO 4 instead.

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

### No Adafruit IO

Adafruit IO is dropped entirely. Data goes to the LAN (Telegraf → InfluxDB →
Grafana) and notifications go to ntfy.sh. This removes the Adafruit IO library
(and its `WiFi101`/`WiFiNINA` `lib_ignore` workaround), the Adafruit IO
watchdog, its one-event-per-minute rate limit and the TLS/MQTT connection to
the cloud, which also frees flash for the OTA partitions.

### Data to the LAN (Telegraf → InfluxDB)

- **Readings every 10 minutes:** water level, temperature, humidity, pump state,
  and pump cycles and run time for the period.
- **Pump ON/OFF events** are sent as they happen (no longer rate limited), with
  the run length on OFF.
- Each point carries the device's own NTP timestamp, so late or buffered data
  lands at the right time in InfluxDB.
- The device sends InfluxDB line protocol (e.g.
  `sump,device=sump level_cm=41.2,temp_c=17.9,humidity=71.5 <ns>`), so Telegraf
  passes it straight through with no JSON parsing config.
- **Buffering:** if the server can't be reached, points queue in RAM (24 hours of
  10-minute readings is about 10 KB) and are sent in order when it's back.
- **Transport: HTTP POST straight to Telegraf's `http_listener_v2`** on the LAN
  (plain HTTP, no broker). Chosen over MQTT for simplicity: no broker to run, no
  always-open connection to watch, and the firmware already uses HTTP for ntfy.
  MQTT's efficiency edge doesn't matter on a mains-powered device sending a few
  small messages an hour. The Telegraf URL goes in `secrets.h`.
- **Server outage handling:** a Telegraf outage does not reboot the
  board (a reboot can't fix the server). After 30 minutes without a successful
  send, one ntfy alert; another when sending recovers.
- **Grafana:** a sump-specific dashboard with one shared time axis: pump state
  (state timeline), water level with the high-water threshold line,
  temperature and humidity, and annotations for alarms and reboots.
- The inherited `telegraf/appliance-events.*` and
  `grafana/appliance-monitor.json` (Adafruit IO based, and belonging to
  vibration-monitor) are replaced by sump-specific configs.

### Notifications (ntfy)

- **Daily report, just before the 3 AM reboot:** pump state (on/off), water
  level, temperature and humidity. Suggested extras: pump cycles and total run
  time for the last 24 hours, the highest level seen, the longest run, and the
  firmware version.
  - Sent at low ntfy priority, so it's waiting on the phone in the morning
    without making a sound at 3 AM.
  - It replaces the separate "Daily scheduled reboot" push
    (`NOTIFY_DAILY_REBOOT` off), so there's one message a night. It doubles as
    the "still alive" heartbeat. Exception reboots are still pushed.
  - If the pump is running at 3 AM the reboot is already held off; the report
    goes out when the reboot actually happens.
- **High water:** when the level passes the threshold (TBD; a blind-zone reading
  also counts), send an urgent-priority alert, then repeat every 30 minutes
  until the level drops back below the threshold minus a few cm of
  hysteresis, then send one "all clear".
- **Pump ON/OFF, in real time:** a push as soon as each start or stop is
  confirmed (the 3 s debounce). **The OFF push always includes the run time**
  (e.g. "sump stopped at 09:12:30 (ran 28.8 s)"), as the current firmware
  does; it's the most useful number for spotting a failing pump or float. The old
  one-per-minute window and "skip if it flipped back" logic are removed, so
  every cycle is reported. Low/default priority, so a busy wet-season day
  doesn't make a stream of noisy alerts.
- **Pump ran but the level didn't drop:** compare the level just before a run
  with the level just after. If it fell less than a minimum amount (a few cm,
  tuned from real runs), alert with both readings and the run length (clogged
  discharge, stuck check valve, pump running dry). If readings around the run
  are missing or suspect, report "can't verify" instead of guessing.
- **Sensor failures:** level sensor (sustained missing echoes), AHT20 and ADXL345.
- **LAN server unreachable** for 30 minutes, and recovered (see above).
- Reboot-reason notifications, OTA updates and rollbacks, as before.
- **Device silent (Grafana alert, not firmware):** Grafana alerting sends to
  ntfy when no sump data has reached InfluxDB for ~30 minutes. It covers what
  the board can't report itself: dead board, power loss, Wi-Fi gone, or a
  firmware hang the watchdogs didn't catch. The 10-minute readings act as the
  heartbeat.
- **ntfy.sh limits:** anonymous use of ntfy.sh has per-IP rate and daily message
  limits, shared with the `pump` board and anything else on the same public IP.
  Check the current limits against a heavy wet-season day (many cycles × two
  pushes each); the alarms matter more than the ON/OFF pushes if it ever gets
  close.

### Device name

`sump` (`[env:sump]`). The existing `pump` board keeps running under its own
name while its reliability testing continues, and will be renamed later.

### OTA updates

- Push OTA from the Mac over the home network using ArduinoOTA, password in
  `secrets.h`, via a new `[env:sump-ota]` build environment. No pull OTA.
- Partition table changes to `min_spiffs.csv` (1.9 MB per app slot; the default
  1.25 MB is too tight). This can't be changed over the air, so **the first flash
  of the new firmware must be over USB**.
- Updates are refused while the pump is running or data is queued.
- Automatic rollback: new firmware boots in trial mode and is marked good only
  after a few minutes of health (Wi-Fi up, all sensors answering); otherwise the
  board returns to the previous firmware. LAN server reachability is not part of
  the check, so a server outage during an update can't trigger a rollback. Check
  whether the pioarduino bootloader has rollback enabled; if not, implement it
  with a boot counter in NVS, checked first thing in `setup()`.
- The hardware watchdog is fed during the upload, and the nightly reboot is held
  off during an update.
- ntfy reports updates ("updated A → B") and rollbacks.
- No backup Wi-Fi network and no USB extension cable: the device is
  inconvenient but not hard to reach.

### Kept from vibration-monitor

The Wi-Fi, I2C sensor and 30 s task watchdogs, the 3 AM nightly reboot, and
reboot-reason notifications. The Adafruit IO watchdog is removed with Adafruit
IO.

## Open questions

- Sensor mounting: how the riser or bracket puts the transducer 30+ cm above the
  rim (see Pit below).
- The pump's on/off levels and the high-water threshold. The firmware logs the
  level at every pump start and stop, so these can be read from the first few
  real cycles rather than measured by hand.
