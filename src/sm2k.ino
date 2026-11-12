#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <time.h>
#include <sys/time.h>
#include <cstdarg>
#include <math.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp32c3/rom/rtc.h>
#include <Adafruit_ADXL345_U.h>
// secrets.h is optional: everything in it can also be set from the web page.
#if __has_include("secrets.h")
#include "secrets.h"
#endif
#include "aht20.h"
#include "jsn_sr04t.h"
#include "web_page.h"

// Bump on each flash you want to identify later -- format: YYYY-MM-DDrN.
#define FIRMWARE_VERSION "2026-11-11r4"

// ---- Configuration (build flags, see platformio.ini) ------------------------
// The thresholds and pit numbers below are the DEFAULTS. Each can be changed
// from the web page (http://sm2k.local/) without reflashing; changed values are
// kept in NVS and read into `cfg` at boot (see "Calibration settings").
// DEVICE_NAME appears in every ntfy message and as the InfluxDB "device" tag;
// keep it to plain characters (no spaces, quotes or commas).
#ifndef DEVICE_NAME
#error "DEVICE_NAME is not set. Build the sump environment: `pio run -e sump` (see platformio.ini)."
#endif
#ifndef OTA_PASSWORD
#error "OTA_PASSWORD is not set. Copy secrets.ini.example to secrets.ini (see platformio.ini)."
#endif

// Vibration thresholds (m/s^2 RMS): above ON_RMS_THRESHOLD = vibrating, below
// OFF_RMS_THRESHOLD = quiet, with the band in between as hysteresis.
#ifndef ON_RMS_THRESHOLD
#define ON_RMS_THRESHOLD 0.50f
#endif
#ifndef OFF_RMS_THRESHOLD
#define OFF_RMS_THRESHOLD 0.30f
#endif
// A run starts after RUN_CONFIRM_MS of continuous vibration and ends after
// STOP_CONFIRM_MS of continuous quiet.
#ifndef RUN_CONFIRM_MS
#define RUN_CONFIRM_MS 3000UL
#endif
#ifndef STOP_CONFIRM_MS
#define STOP_CONFIRM_MS 3000UL
#endif

// Pit geometry. The level sensor measures distance down to the water; water
// depth above the pit floor is SENSOR_TO_FLOOR_CM minus that distance.
#ifndef SENSOR_TO_FLOOR_CM
#define SENSOR_TO_FLOOR_CM 95.0f
#endif
// High-water alarm when the water is at least this deep; clears once it falls
// HIGH_WATER_HYST_CM below it.
#ifndef HIGH_WATER_CM
#define HIGH_WATER_CM 45.0f
#endif
#ifndef HIGH_WATER_HYST_CM
#define HIGH_WATER_HYST_CM 5.0f
#endif
// A pump run should lower the water by at least this much.
#ifndef MIN_DROP_CM
#define MIN_DROP_CM 5.0f
#endif

// Connections. These build-time values (secrets.h) are only the starting
// values: each can be changed on the web page and is then kept in NVS (see
// "Connection settings"). An empty value turns that output off.
#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef TELEGRAF_URL
#define TELEGRAF_URL ""
#endif
#ifndef NTFY_TOPIC
#define NTFY_TOPIC ""
#endif
#ifndef MQTT_HOST
#define MQTT_HOST ""
#endif
#ifndef MQTT_PORT
#define MQTT_PORT 1883
#endif
#ifndef MQTT_USER
#define MQTT_USER ""
#endif
#ifndef MQTT_PASS
#define MQTT_PASS ""
#endif

// Network name for OTA uploads (sm2k.local) and DHCP.
#define OTA_HOSTNAME "sm2k"

// NVS (flash) namespace for persisted state.
#define NVS_NS "sm2k"

const char *BANNER =
R"(               ____  _
 ___ _ __ ___ |___ \| | __
/ __| '_ ` _ \  __) | |/ /
\__ \ | | | | |/ __/|   <
|___/_| |_| |_|_____|_|\_\

)";

// ---- Pins -----------------------------------------------------------------
const int SDA_PIN = 5;
const int SCL_PIN = 6;   // GPIO 8/9 are LED/boot pins on this board
const uint32_t I2C_CLOCK_HZ = 100000;  // 400 kHz was unreliable on the bench (50 kHz made the sampling loop late)
// JSN-SR04T: Trig direct, Echo through a 5 V -> 3.3 V divider. GPIO 2, 8 and 9
// are boot strapping pins on the C3, so they're avoided.
const int LEVEL_TRIG_PIN = 3;
const int LEVEL_ECHO_PIN = 4;

// Onboard NeoPixel (WS2812) status LED: green = all clear, blue = pump running,
// flashing red = high water (pump off), flashing purple = high water (pump on).
// Dimmed because a full-brightness NeoPixel is blinding.
const int     LED_PIN        = 8;
const uint8_t LED_BRIGHTNESS = 16;  // 0-255
const uint32_t LED_FLASH_HALF_PERIOD_MS = 500;

// ---- Vibration detection --------------------------------------------------
// Samples are high-passed against a slow per-axis baseline (removes gravity and
// mounting angle), then RMS'd over a short window. Tune the thresholds from the
// web page or the Grafana vibration panel, with the pump idle and running.
const uint32_t SAMPLE_PERIOD_US  = 2500;   // 400 Hz, matches accel data rate
const uint16_t SAMPLES_PER_WINDOW = 100;   // 250 ms windows
// High-pass cutoff ~ alpha * fs / 2pi = ~13 Hz: rejects footsteps / house rumble
// (<10 Hz) while passing motor vibration (~29-58 Hz for 1725/3450 RPM).
const float    BASELINE_ALPHA    = 0.2;

// ---- Water level (JSN-SR04T) ------------------------------------------------
// Every LEVEL_INTERVAL_MS a burst of pings is sent, one per 250 ms window (so
// sampling never waits on an echo), and the median is used.
const uint32_t LEVEL_INTERVAL_MS  = 5000;
const int      LEVEL_PINGS        = 5;
const int      LEVEL_MIN_ECHOES   = 3;       // pings that must agree for a burst to count
// The sensor is blind closer than ~20-25 cm and can then report a double echo
// (about twice the real distance). Anything under this counts as "at or above
// the limit", never as a real distance.
const float    LEVEL_MIN_CM       = 25.0f;
const float    LEVEL_MAX_CM       = 450.0f;
// A change bigger than this between readings is held as suspect until the next
// reading confirms it (within LEVEL_CONFIRM_CM). A pump draining the pit moves
// the level only a few cm per reading.
const float    LEVEL_MAX_STEP_CM  = 15.0f;
const float    LEVEL_CONFIRM_CM   = 5.0f;
const uint32_t LEVEL_STALE_MS     = 60000;   // older readings count as "no current reading"
const uint32_t LEVEL_MISSING_ALERT_MS = 120000;
// The pump-run check compares the level before the run with the first reading
// taken at least LEVEL_SETTLE_MS after it stops, giving up after LEVEL_AFTER_TIMEOUT_MS.
const uint32_t LEVEL_SETTLE_MS        = 3000;
const uint32_t LEVEL_AFTER_TIMEOUT_MS = 60000;
const uint32_t HIGH_WATER_REPEAT_MS   = 1800000;  // repeat the alarm every 30 min

// ---- Temperature / humidity (AHT20) -------------------------------------------
const uint32_t AHT_INTERVAL_MS    = 10000;
const uint32_t AHT_MISSING_RETRY_MS = 60000;  // retry this often while it isn't answering
const uint32_t AHT_STALE_MS       = 60000;
const uint32_t AHT_ALERT_MS       = 300000;  // push an alert after this long without a reading

// ---- Reporting --------------------------------------------------------------
// Readings go to Telegraf every 10 minutes, on the wall-clock 10 minutes.
const uint32_t READING_PERIOD_S   = 600;
// Points waiting for Telegraf, oldest first. 150 lines covers a day of
// 10-minute readings plus pump events; if it fills, the oldest are dropped.
const int      TG_QUEUE_SIZE      = 150;
const int      TG_LINE_LEN        = 320;
const int      TG_BATCH_LINES     = 20;
const uint32_t TG_RETRY_MS        = 30000;
const uint32_t TG_DOWN_ALERT_MS   = 1800000;  // alert after 30 min without a successful send

// ntfy priorities: 1 min, 2 low, 3 default, 4 high, 5 urgent.
const uint8_t PRIO_REPORT = 2;  // daily report: silent, waiting in the morning
const uint8_t PRIO_PUMP   = 3;  // pump on/off
const uint8_t PRIO_INFO   = 3;  // reboots, updates, all clear, recoveries
const uint8_t PRIO_ALERT  = 4;  // sensor failures, pump didn't lower the level
const uint8_t PRIO_URGENT = 5;  // high water
const int      NTFY_QUEUE_SIZE    = 10;
const uint32_t NTFY_RETRY_MS      = 10000;

// ---- Reliability ----------------------------------------------------------
// Many ESP32-C3 mini boards have a marginal antenna/regulator design and fail to
// join at full TX power (~19.5 dBm); capping it is the usual fix. Drop to
// WIFI_POWER_8_5dBm if joins are flaky at this level.
const wifi_power_t WIFI_TX_POWER = WIFI_POWER_15dBm;
const uint32_t WIFI_CONNECT_TIMEOUT_MS = 30000;   // at boot: reboot if no WiFi by then
const uint32_t WIFI_RETRY_MS           = 30000;   // reset the radio and retry this often
const uint32_t WIFI_DOWN_REBOOT_MS     = 120000;  // running: reboot if WiFi down this long

// The accelerometer is a GY-346 module (ADXL346). It's register-compatible
// with the ADXL345, so the Adafruit ADXL345 library drives it, but its device
// ID differs (0xE6 vs 0xE5) and the library's begin() only accepts 0xE5; see
// beginAccel(). Either chip is accepted.
// The accelerometer is polled for its fixed device ID; a wedged or unplugged I2C
// bus otherwise just produces a stream of zeros (looks like an idle pump).
// If it's missing (at boot or later) the board keeps running without it --
// water level, alarms and reporting matter more than pump on/off -- pushes
// one alert, and retries it (with I2C bus recovery) every ACCEL_RETRY_MS.
const uint32_t SENSOR_CHECK_INTERVAL_MS = 30000;
const int      SENSOR_FAIL_LIMIT        = 3;       // consecutive failed checks => treat as missing
const int      SAMPLE_FAIL_LIMIT        = 200;     // consecutive failed 400 Hz reads (0.5 s) => accelerometer lost
const int      FLAT_WINDOW_LIMIT        = 40;      // consecutive windows (10 s) with rms exactly 0 => not measuring
const int      AHT_FAST_RETRIES         = 5;       // quick retries (AHT_FAST_RETRY_MS) before backing off
const uint32_t AHT_FAST_RETRY_MS        = 2000;
const uint32_t ACCEL_RETRY_MS           = 30000;
const uint8_t  ADXL345_DEVICE_ID        = 0xE5;
const uint8_t  ADXL346_DEVICE_ID        = 0xE6;

// Scheduled reboot once a day at a fixed local hour, to guard against slow heap
// fragmentation. The daily report is pushed just before it. POSIX TZ string
// (not a fixed offset) so DST is handled -- US Eastern.
#define TZ_STRING   "EST5EDT,M3.2.0,M11.1.0/2"
#define REBOOT_HOUR 3
const char *NTP_SERVER = "pool.ntp.org";
#define REASON_DAILY_REBOOT "Daily scheduled reboot"
#define REASON_OTA_UPDATE   "OTA update"

// If the board was restarted while the pump's last recorded event was a
// "start", correct it once the pump has been observed for this long and is
// really idle.
const uint32_t BOOT_SETTLE_MS = 15000;

// OTA: a freshly updated image runs as a trial. It's marked good once it has
// run for OTA_TRIAL_MIN_MS with Wi-Fi up and every sensor that was healthy
// before the update healthy again; otherwise, after OTA_TRIAL_TIMEOUT_MS, it
// rolls back. A crash or reboot during the trial also rolls back (bootloader).
const uint32_t OTA_TRIAL_MIN_MS     = 180000;
const uint32_t OTA_TRIAL_TIMEOUT_MS = 600000;
const uint8_t  HEALTH_ACCEL = 0x01;
const uint8_t  HEALTH_LEVEL = 0x02;
const uint8_t  HEALTH_AHT   = 0x04;

Adafruit_ADXL345_Unified accel = Adafruit_ADXL345_Unified(12345);
Aht20 aht;
JsnSr04t sonar(LEVEL_TRIG_PIN, LEVEL_ECHO_PIN);

// ---- State ----------------------------------------------------------------
float bx, by, bz;              // per-axis baseline
double sumSq = 0;
uint16_t sampleCount = 0;
uint32_t nextSampleUs = 0;

bool deviceRunning = false;
uint32_t aboveSince = 0;       // 0 = not currently above ON threshold
uint32_t belowSince = 0;       // 0 = not currently below OFF threshold
uint32_t runStartMs = 0;
uint32_t lastStatusMs = 0;
float lastRms = 0;
float maxRmsSinceStatus = 0;   // loudest window since the last status line
int   loudWindowsSinceStatus = 0; // windows >= the stop threshold since the last status line
int lastRecordedState = -1;    // last pump event recorded: 1 start, 0 stop, -1 unknown; persisted in NVS

// Pump statistics since boot (the daily reboot makes this roughly a day) and
// since the last 10-minute reading.
uint32_t cycleCount = 0;
uint32_t totalRunMs = 0;
uint32_t longestRunMs = 0;
uint32_t periodCycles = 0;
uint32_t periodRunMs = 0;
time_t   bootEpoch = 0;        // wall-clock boot time, once NTP has synced

// Water level.
enum LevelStatus { LEVEL_OK, LEVEL_NEAR_LIMIT, LEVEL_MISSING };
bool     burstActive = false;
int      burstPing = 0;
uint32_t lastBurstMs = 0;
float    burstCm[LEVEL_PINGS];
int      burstEchoes = 0;
int      burstNear = 0;
bool     haveLevel = false;
float    levelCm = 0;          // accepted water depth above the floor
float    levelDistCm = 0;      // accepted sensor-to-water distance
bool     levelNearLimit = false;
uint32_t levelAtMs = 0;        // when the accepted reading was taken
bool     candidateValid = false;
float    candidateCm = 0;      // a suspect jump waiting to be confirmed
uint32_t lastEchoMs = 0;       // last burst that saw the water at all (0 = never)
bool     levelAlerted = false;
float    maxLevelCm = NAN;     // highest accepted level since boot
bool     highWater = false;
uint32_t lastHighAlertMs = 0;

// Pump-run level check.
float    runLevelBefore = NAN;
bool     dropCheckPending = false;
uint32_t dropStopMs = 0;
float    dropRunSeconds = 0;

// Temperature / humidity.
bool     ahtReady = false;
bool     ahtMeasuring = false;
uint32_t lastAhtStartMs = 0;
uint32_t ahtAtMs = 0;          // last good reading (0 = never)
float    tempC = NAN;
float    humidity = NAN;
bool     ahtAlerted = false;

// Telegraf queue (ring buffer).
char     tgQueue[TG_QUEUE_SIZE][TG_LINE_LEN];
int      tgHead = 0;
int      tgCount = 0;
uint32_t tgDropped = 0;
uint32_t lastTgFailMs = 0;     // 0 = last attempt succeeded
uint32_t lastTgOkMs = 0;
bool     tgAlerted = false;
uint32_t lastReadingSlot = UINT32_MAX;

// ntfy queue (ring buffer).
struct NtfyMsg {
  char title[64];
  char body[320];
  uint8_t priority;
  const char *tags;
};
NtfyMsg  ntfyQueue[NTFY_QUEUE_SIZE];
int      ntfyHead = 0;
int      ntfyCount = 0;
uint32_t lastNtfyFailMs = 0;   // 0 = last attempt succeeded

// Network, watchdogs, OTA.
uint32_t lastWifiAttempt = 0;
uint32_t wifiDownSince = 0;    // 0 = WiFi currently up
int      lastRebootDay = -1;   // persisted in NVS so the daily reboot fires once, not in a loop
int      sensorFails = 0;
int      accelReadFails = 0;   // consecutive failed sample reads
int      flatWindows = 0;      // consecutive windows with a constant (zero) signal
int      ahtRetries = 0;       // failed AHT20 attempts since the last good reading
bool     accelOk = false;      // accelerometer detected and answering
uint32_t accelOkSinceMs = 0;   // when it (re)started answering
uint32_t lastAccelRetryMs = 0;
bool     accelAlerted = false;
uint32_t lastSensorCheck = 0;
bool     bootCorrectionDone = false;
bool     otaTrial = false;     // running a fresh OTA image that hasn't passed its health check
uint8_t  otaRequiredHealth = HEALTH_ACCEL;
volatile bool otaDone = false;

// ---- Calibration settings ---------------------------------------------------
// Changeable from the web page; kept in NVS (keys c_*). Defaults are the build
// flags above.
struct Settings {
  float    onRms, offRms;              // m/s^2 RMS
  uint32_t runConfirmMs, stopConfirmMs;
  float    sensorToFloorCm, highWaterCm, highWaterHystCm, minDropCm;
};
const Settings DEFAULT_SETTINGS = { ON_RMS_THRESHOLD, OFF_RMS_THRESHOLD, RUN_CONFIRM_MS, STOP_CONFIRM_MS,
                                    SENSOR_TO_FLOOR_CM, HIGH_WATER_CM, HIGH_WATER_HYST_CM, MIN_DROP_CM };
Settings cfg = DEFAULT_SETTINGS;

// ---- Connection settings ------------------------------------------------------
// Changeable from the web page; kept in NVS (keys n_*). An empty Telegraf URL,
// ntfy topic or MQTT host turns that output off. The admin password (for
// saving settings on the web page) starts as the OTA password.
struct NetConfig {
  char     wifiSsid[33];
  char     wifiPass[65];
  char     telegrafUrl[128];
  char     ntfyTopic[65];
  char     mqttHost[64];
  uint16_t mqttPort;
  char     mqttUser[33];
  char     mqttPass[65];
  char     adminPass[33];
};
NetConfig net;

// ---- MQTT / Home Assistant ------------------------------------------------------
// State goes to <base>/state as one retained JSON message; <base>/status is
// "online"/"offline" (the broker publishes "offline" if the board vanishes).
// Home Assistant finds the entities through MQTT Discovery.
const uint32_t MQTT_RETRY_MS    = 30000;
const uint32_t MQTT_STATE_MS    = 30000;   // state is also sent at once on a pump or alarm change
WiFiClient   mqttNet;
PubSubClient mqtt(mqttNet);
char     mqttBase[48];
uint32_t lastMqttAttemptMs = 0;
uint32_t lastMqttStateMs = 0;
bool     mqttDirty = true;      // state changed: publish now
int      mqttDiscoveryIdx = -1; // next discovery message to send (-1 = none pending)
bool     mqttFailLogged = false;
const char *mqttProblem();

struct HaEntity {
  const char *component;   // "sensor" or "binary_sensor"
  const char *id;          // also the key in the state JSON
  const char *name;
  const char *deviceClass; // "" = none
  const char *unit;
  const char *stateClass;
  bool        diagnostic;
};
const HaEntity HA_ENTITIES[] = {
  {"binary_sensor", "pump",           "Pump",                      "running",         "",      "",                 false},
  {"binary_sensor", "high_water",     "High water",                "problem",         "",      "",                 false},
  {"sensor",        "level_cm",       "Water depth",               "distance",        "cm",    "measurement",      false},
  {"sensor",        "temp_c",         "Air temperature",           "temperature",     "°C",    "measurement",      false},
  {"sensor",        "humidity",       "Humidity",                  "humidity",        "%",     "measurement",      false},
  {"sensor",        "vibration",      "Vibration",                 "",                "m/s²",  "measurement",      false},
  {"sensor",        "last_run_s",     "Last pump run",             "duration",        "s",     "",                 false},
  {"sensor",        "cycles",         "Pump cycles since restart", "",                "",      "total_increasing", false},
  {"binary_sensor", "level_problem",  "Level sensor",              "problem",         "",      "",                 true},
  {"binary_sensor", "pump_sensor_problem", "Pump sensor",          "problem",         "",      "",                 true},
  {"sensor",        "rssi",           "Wi-Fi signal",              "signal_strength", "dBm",   "measurement",      true},
  {"sensor",        "uptime_s",       "Uptime",                    "duration",        "s",     "",                 true},
  {"sensor",        "version",        "Firmware",                  "",                "",      "",                 true},
};
const int HA_ENTITY_COUNT = sizeof(HA_ENTITIES) / sizeof(HA_ENTITIES[0]);

// ---- Vibration statistics (for calibration) -----------------------------------
// Window RMS values, split by whether the pump was idle or running. Windows
// seen while a start or stop is still being confirmed wait in vibPending and
// are counted once it's known which state they belonged to.
struct RmsAcc {
  double   sum = 0;
  uint32_t n = 0;
  float    mn = 0, mx = 0;
  void add(float v) { if (!n || v < mn) mn = v; if (!n || v > mx) mx = v; sum += v; n++; }
  void absorb(const RmsAcc &o) {
    if (!o.n) return;
    if (!n || o.mn < mn) mn = o.mn;
    if (!n || o.mx > mx) mx = o.mx;
    sum += o.sum; n += o.n;
  }
  float avg() const { return n ? (float)(sum / n) : 0; }
  void reset() { *this = RmsAcc(); }
};
RmsAcc   vibIdle, vibRun;       // since the last 10-minute reading
RmsAcc   vibIdleBoot;           // idle, since boot
RmsAcc   vibThisRun, vibLastRun;
RmsAcc   vibPending;
float    lastRunSeconds = 0;
uint32_t lastRunEndMs = 0;
float    statusMaxRms = 0;      // loudest window in the last full second (for the web page)

// ---- Log kept for the web page -------------------------------------------------
// The last LOG_LINES log lines (not the once-a-second status line).
const int LOG_LINES    = 120;
const int LOG_LINE_LEN = 160;
char     logRing[LOG_LINES][LOG_LINE_LEN];
uint32_t logSeq = 0;            // lines logged so far; line i is logRing[i % LOG_LINES]
bool     logKeep = true;        // false while printing a line that shouldn't be kept

WebServer web(80);

// The Arduino core marks a new OTA image good as soon as it boots unless this
// returns true; the health check in checkOtaTrial() does it instead.
extern "C" bool verifyRollbackLater() { return true; }

// ==========================================
// Timestamped logging
// ==========================================
// Prefixes each line with the synced wall-clock time once NTP has synced, or
// uptime in seconds before that.
const char *logTimestamp() {
  static char buf[32];
  time_t now = time(nullptr);
  if (now > 100000) {
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
  } else {
    snprintf(buf, sizeof(buf), "+%lus", millis() / 1000);
  }
  return buf;
}

void logf(const char *fmt, ...) {
  char msg[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(msg, sizeof(msg), fmt, args);
  va_end(args);
  const char *ts = logTimestamp();
  Serial.printf("[%s] %s\r\n", ts, msg);
  if (logKeep) {
    snprintf(logRing[logSeq % LOG_LINES], LOG_LINE_LEN, "[%s] %s", ts, msg);
    logSeq++;
  }
}

// Formats when something happened, ageMs ago (state changes are confirmed
// after a debounce delay). Leaves the buffer empty if the clock hasn't synced.
void formatEventTime(char *buf, size_t len, uint32_t ageMs, const char *fmt) {
  buf[0] = 0;
  time_t now = time(nullptr);
  if (now < 100000) return;
  time_t t = now - (time_t)(ageMs / 1000);
  struct tm timeinfo;
  localtime_r(&t, &timeinfo);
  strftime(buf, len, fmt, &timeinfo);
}

// Wall-clock time ageMs ago in nanoseconds (line protocol's default precision),
// or 0 if the clock hasn't synced.
uint64_t epochNs(uint32_t ageMs) {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 100000) return 0;
  int64_t ms = (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000 - ageMs;
  return (uint64_t)ms * 1000000ULL;
}

bool levelFresh(uint32_t now) { return haveLevel && now - levelAtMs < LEVEL_STALE_MS; }
bool ahtFresh(uint32_t now)   { return ahtAtMs && now - ahtAtMs < AHT_STALE_MS; }

// "12.3 cm (4.8 in)", or "at least 70.0 cm (sensor limit)" when the water is
// in the sensor's blind zone.
String levelText() {
  char buf[64];
  if (levelNearLimit) {
    snprintf(buf, sizeof(buf), "at least %.1f cm deep (at the sensor's limit)", levelCm);
  } else {
    snprintf(buf, sizeof(buf), "%.1f cm deep (%.1f in)", levelCm, levelCm / 2.54f);
  }
  return String(buf);
}

// ==========================================
// Notifications (ntfy.sh)
// ==========================================
// Messages are queued and sent from the main loop (one per 250 ms window), so
// a slow or failed request never happens inside sensor handling, and a push
// that fails (internet down) is retried rather than lost.
Preferences prefs;

int sendNtfy(const NtfyMsg &m) {
  if (!net.ntfyTopic[0] || WiFi.status() != WL_CONNECTED) return -1;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);  // seconds
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(client, String("https://ntfy.sh/") + net.ntfyTopic)) return -1;
  http.addHeader("Title", m.title);
  http.addHeader("Priority", String(m.priority));
  if (m.tags && m.tags[0]) http.addHeader("Tags", m.tags);
  int code = http.POST(String(m.body));
  http.end();
  logf("[ntfy] \"%s\" -> %d", m.title, code);
  return code;
}

// Queues a push. The body is printf-formatted. With no ntfy topic set it's only logged.
void notify(uint8_t priority, const char *tags, const char *title, const char *fmt, ...) {
  NtfyMsg m;
  snprintf(m.title, sizeof(m.title), "%s", title);
  va_list args;
  va_start(args, fmt);
  vsnprintf(m.body, sizeof(m.body), fmt, args);
  va_end(args);
  m.priority = priority;
  m.tags = tags;
  logf("[ntfy] %s: %s", m.title, m.body);
  if (!net.ntfyTopic[0]) return;
  if (ntfyCount == NTFY_QUEUE_SIZE) {
    logf("[ntfy] queue full, dropping oldest message");
    ntfyHead = (ntfyHead + 1) % NTFY_QUEUE_SIZE;
    ntfyCount--;
  }
  ntfyQueue[(ntfyHead + ntfyCount) % NTFY_QUEUE_SIZE] = m;
  ntfyCount++;
}

// Sends the oldest queued push. Returns true if one was sent.
bool serviceNtfy(uint32_t now, bool force = false) {
  if (ntfyCount == 0 || WiFi.status() != WL_CONNECTED) return false;
  if (!force && lastNtfyFailMs && now - lastNtfyFailMs < NTFY_RETRY_MS) return false;
  int code = sendNtfy(ntfyQueue[ntfyHead]);
  bool sent = code >= 200 && code < 300;
  // A 4xx other than rate limiting (429) won't succeed on retry.
  bool hopeless = code >= 400 && code < 500 && code != 429;
  if (sent || hopeless) {
    if (hopeless) logf("[ntfy] dropping message after HTTP %d", code);
    ntfyHead = (ntfyHead + 1) % NTFY_QUEUE_SIZE;
    ntfyCount--;
    lastNtfyFailMs = 0;
  } else {
    lastNtfyFailMs = now ? now : 1;
  }
  return sent;
}

// ==========================================
// Data to Telegraf (InfluxDB line protocol over HTTP)
// ==========================================
// Appends one key=value field to a line-protocol field set.
void addField(char *buf, size_t len, const char *fmt, ...) {
  size_t n = strlen(buf);
  if (n + 2 >= len) return;
  if (n) buf[n++] = ',';
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf + n, len - n, fmt, args);
  va_end(args);
}

// Copies s into out with line-protocol string escaping (quotes, backslashes).
void escapeString(const char *s, char *out, size_t len) {
  size_t n = 0;
  for (; *s && n + 2 < len; s++) {
    if (*s == '"' || *s == '\\') out[n++] = '\\';
    out[n++] = *s;
  }
  out[n] = 0;
}

// Queues one point, timestamped ageMs ago (Telegraf's arrival time is used if
// the clock hasn't synced yet).
void tgPoint(const char *measurement, const char *fields, uint32_t ageMs = 0) {
  char line[TG_LINE_LEN];
  uint64_t ns = epochNs(ageMs);
  int n = ns ? snprintf(line, sizeof(line), "%s,device=%s %s %llu", measurement, DEVICE_NAME,
                        fields, (unsigned long long)ns)
             : snprintf(line, sizeof(line), "%s,device=%s %s", measurement, DEVICE_NAME, fields);
  if (n >= (int)sizeof(line)) {
    logf("[tg] point too long, dropped: %s", measurement);
    return;
  }
  if (tgCount == TG_QUEUE_SIZE) {
    tgHead = (tgHead + 1) % TG_QUEUE_SIZE;
    tgCount--;
    tgDropped++;
  }
  memcpy(tgQueue[(tgHead + tgCount) % TG_QUEUE_SIZE], line, n + 1);
  tgCount++;
  logf("[tg] queued: %s", line);
}

// POSTs up to TG_BATCH_LINES queued points. Returns true on success.
bool serviceTelegraf(uint32_t now, bool force = false) {
  if (!net.telegrafUrl[0]) {
    tgCount = 0;  // nowhere to send; the points were logged when queued
    return false;
  }
  if (tgCount == 0 || WiFi.status() != WL_CONNECTED) return false;
  if (!force && lastTgFailMs && now - lastTgFailMs < TG_RETRY_MS) return false;

  int lines = tgCount < TG_BATCH_LINES ? tgCount : TG_BATCH_LINES;
  String body;
  body.reserve(lines * 100);
  for (int i = 0; i < lines; i++) {
    body += tgQueue[(tgHead + i) % TG_QUEUE_SIZE];
    body += '\n';
  }
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(4000);
  int code = -1;
  if (http.begin(client, net.telegrafUrl)) {
    http.addHeader("Content-Type", "text/plain; charset=utf-8");
    code = http.POST(body);
    http.end();
  }
  bool sent = code >= 200 && code < 300;
  // A 4xx means Telegraf rejected the data itself; retrying would block the
  // queue forever, so drop the batch.
  bool rejected = code >= 400 && code < 500;
  if (sent || rejected) {
    if (rejected) logf("[tg] Telegraf rejected %d point(s) (HTTP %d), dropped", lines, code);
    tgHead = (tgHead + lines) % TG_QUEUE_SIZE;
    tgCount -= lines;
    lastTgFailMs = 0;
    lastTgOkMs = now;
    if (tgAlerted) {
      tgAlerted = false;
      notify(PRIO_INFO, "white_check_mark", DEVICE_NAME ": Telegraf reachable again",
             "Sending data again. %lu point(s) were dropped while the queue was full.",
             (unsigned long)tgDropped);
    }
    tgDropped = 0;
  } else {
    logf("[tg] send failed (%d: %s), %d point(s) queued", code,
         code < 0 ? HTTPClient::errorToString(code).c_str() : "HTTP status", tgCount);
    lastTgFailMs = now ? now : 1;
  }
  return sent;
}

// A Telegraf outage doesn't reboot the board (that can't fix the server);
// data queues up, and one push says so.
void checkTelegrafWatchdog(uint32_t now) {
  if (tgCount == 0 || tgAlerted || now - lastTgOkMs < TG_DOWN_ALERT_MS) return;
  tgAlerted = true;
  notify(PRIO_INFO, "warning", DEVICE_NAME ": can't reach Telegraf",
         "No data sent for %lu min; %d point(s) queued on the board.",
         (unsigned long)((now - lastTgOkMs) / 60000), tgCount);
}

// Sends what's queued (Telegraf and ntfy) before a deliberate restart, for up
// to timeoutMs. Stops early if a send fails.
void flushQueues(uint32_t timeoutMs) {
  uint32_t start = millis();
  while ((tgCount > 0 || ntfyCount > 0) && millis() - start < timeoutMs) {
    esp_task_wdt_reset();
    bool progress = false;
    if (tgCount > 0) progress |= serviceTelegraf(millis(), true);
    if (ntfyCount > 0) progress |= serviceNtfy(millis(), true);
    if (!progress) break;
  }
}

// ==========================================
// Reboot reporting
// ==========================================
// Self-triggered reboots would otherwise be silent. The reason is written to
// NVS flash right before restarting, then read back and pushed once WiFi is up
// on the next boot. A manual power cycle leaves no reason behind.

// Reads the recorded reboot reason ("" if none), without the driver logging an
// error for a key that was never written.
String readRebootReason() {
  return prefs.isKey("last_reason") ? prefs.getString("last_reason", "") : String("");
}

void recordRebootReason(const char *reason) {
  prefs.begin(NVS_NS, false);
  prefs.putString("last_reason", reason);
  // The daily reboot and updates aren't symptoms of anything -- only count
  // exception reboots, so "reboot #N" means something went wrong N times.
  if (strcmp(reason, REASON_DAILY_REBOOT) != 0 && strcmp(reason, REASON_OTA_UPDATE) != 0) {
    prefs.putUInt("reboot_count", prefs.getUInt("reboot_count", 0) + 1);
  }
  prefs.end();
}

void restartWithReason(const char *reason) {
  logf("Restarting: %s", reason);
  recordRebootReason(reason);
  delay(100);
  ESP.restart();
}

// Detects a reboot that bypassed our own restartWithReason() -- a crash, a
// hung loop tripping the hardware watchdog, a brownout. Must run before
// anything else in setup() can record a reason for this boot.
void checkUnexpectedReset() {
  prefs.begin(NVS_NS, false);
  String reason = readRebootReason();
  prefs.end();
  if (reason.length() > 0) return; // a deliberate reboot already recorded this

  esp_reset_reason_t r = esp_reset_reason();
  // POWERON/EXT are power cycles or the reset button; SW is our own
  // ESP.restart() (or an OTA rollback), which records its own reason.
  if (r == ESP_RST_POWERON || r == ESP_RST_EXT || r == ESP_RST_SW) return;

  // A serial tool toggling RTS (flashing, opening a monitor) isn't classified
  // by esp_reset_reason(); check the raw code so that doesn't look like a crash.
  RESET_REASON raw = rtc_get_reset_reason(0);
  if (raw == USB_UART_CHIP_RESET || raw == USB_JTAG_CHIP_RESET) return;

  const char *desc;
  switch (r) {
    case ESP_RST_PANIC:     desc = "crash/panic"; break;
    case ESP_RST_INT_WDT:   desc = "interrupt watchdog"; break;
    case ESP_RST_TASK_WDT:  desc = "task watchdog"; break;
    case ESP_RST_WDT:       desc = "other watchdog"; break;
    case ESP_RST_BROWNOUT:  desc = "brownout"; break;
    case ESP_RST_DEEPSLEEP: desc = "deep sleep wake"; break;
    default:                desc = "unknown"; break;
  }
  char buf[48];
  snprintf(buf, sizeof(buf), "Unexpected reset (%s)", desc);
  recordRebootReason(buf);
}

// Pushes the reason for the last reboot (if one was recorded) and records a
// boot point for the Grafana annotations. The daily reboot is covered by the
// daily report and updates by their own push, so those stay quiet.
void reportBoot() {
  prefs.begin(NVS_NS, false);
  String reason = readRebootReason();
  uint32_t count = prefs.getUInt("reboot_count", 0);
  if (reason.length() > 0) {
    prefs.putString("last_reason", ""); // clear so a normal boot stays quiet
  }
  prefs.end();

  char esc[96], fields[160] = "";
  escapeString(reason.length() ? reason.c_str() : "power on or reset", esc, sizeof(esc));
  addField(fields, sizeof(fields), "version=\"%s\"", FIRMWARE_VERSION);
  addField(fields, sizeof(fields), "reason=\"%s\"", esc);
  tgPoint("sump_boot", fields);

  if (reason.length() == 0 || reason == REASON_DAILY_REBOOT || reason == REASON_OTA_UPDATE) return;
  notify(PRIO_INFO, "arrows_counterclockwise", DEVICE_NAME " rebooted",
         "%s (reboot #%lu) at %s", reason.c_str(), (unsigned long)count, logTimestamp());
}

void saveLastRecordedState(int state) {
  prefs.begin(NVS_NS, false);
  prefs.putInt("last_state", state);
  prefs.end();
}

// ==========================================
// OTA updates (push from the Mac with ArduinoOTA)
// ==========================================
uint8_t currentHealth(uint32_t now) {
  uint8_t h = 0;
  if (accelOk) h |= HEALTH_ACCEL;
  if (lastEchoMs && now - lastEchoMs < LEVEL_STALE_MS) h |= HEALTH_LEVEL;
  if (ahtFresh(now)) h |= HEALTH_AHT;
  return h;
}

String describeHealth(uint8_t missing) {
  String s;
  if (missing & HEALTH_ACCEL) s += "accelerometer, ";
  if (missing & HEALTH_LEVEL) s += "level sensor, ";
  if (missing & HEALTH_AHT)   s += "temperature/humidity sensor, ";
  if (s.length()) s.remove(s.length() - 2);
  return s;
}

// Called at boot. Detects a trial run of a fresh OTA image, and reports a
// rollback if the previous trial failed (the bootloader has already switched
// back to this image by the time it runs).
void checkOtaState() {
  esp_ota_img_states_t state;
  otaTrial = esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) == ESP_OK &&
             state == ESP_OTA_IMG_PENDING_VERIFY;

  prefs.begin(NVS_NS, false);
  String trialVer = prefs.isKey("trial_ver") ? prefs.getString("trial_ver", "") : String("");
  String goodVer  = prefs.isKey("fw_ver") ? prefs.getString("fw_ver", "") : String("");
  otaRequiredHealth = prefs.getUChar("pre_health", HEALTH_ACCEL);
  if (otaTrial) {
    logf("[OTA] running new firmware as a trial (previous: %s)", goodVer.length() ? goodVer.c_str() : "unknown");
    prefs.putString("trial_ver", FIRMWARE_VERSION);
  } else {
    // trial_ver is removed when a trial passes, so if it's still set, the
    // bootloader rolled that image back.
    if (trialVer.length() > 0) {
      String why = prefs.isKey("trial_fail") ? prefs.getString("trial_fail", "")
                                             : String("it crashed or restarted before passing its health check");
      notify(PRIO_ALERT, "warning", DEVICE_NAME ": update rolled back",
             "Firmware %s failed: %s. Back on %s.", trialVer.c_str(), why.c_str(), FIRMWARE_VERSION);
    }
    prefs.remove("trial_ver");
    prefs.remove("trial_fail");
    if (goodVer != FIRMWARE_VERSION) prefs.putString("fw_ver", FIRMWARE_VERSION);  // e.g. a USB flash
  }
  prefs.end();
}

void failOtaTrial(const char *why) {
  logf("[OTA] trial failed (%s), rolling back", why);
  prefs.begin(NVS_NS, false);
  prefs.putString("trial_fail", why);
  prefs.end();
  flushQueues(5000);
  esp_ota_mark_app_invalid_rollback_and_reboot();
}

void checkOtaTrial(uint32_t now) {
  if (!otaTrial) return;
  bool wifiUp = WiFi.status() == WL_CONNECTED;
  uint8_t missing = otaRequiredHealth & ~currentHealth(now);
  if (now >= OTA_TRIAL_MIN_MS && wifiUp && missing == 0) {
    esp_ota_mark_app_valid_cancel_rollback();
    otaTrial = false;
    prefs.begin(NVS_NS, false);
    String prev = prefs.isKey("fw_ver") ? prefs.getString("fw_ver", "") : String("unknown");
    prefs.putString("fw_ver", FIRMWARE_VERSION);
    prefs.remove("trial_ver");
    prefs.remove("pre_health");
    prefs.end();
    logf("[OTA] trial passed, firmware %s marked good", FIRMWARE_VERSION);
    notify(PRIO_INFO, "arrow_up", DEVICE_NAME " updated",
           "Firmware %s -> %s, passed its health check.", prev.c_str(), FIRMWARE_VERSION);
  } else if (now >= OTA_TRIAL_TIMEOUT_MS) {
    char why[128];
    if (!wifiUp) snprintf(why, sizeof(why), "Wi-Fi down");
    else snprintf(why, sizeof(why), "not answering: %s", describeHealth(missing).c_str());
    failOtaTrial(why);
  }
}

// Restarts into a freshly received update (set by ArduinoOTA's onEnd).
void restartAfterOta() {
  logf("[OTA] update received, restarting into it");
  flushQueues(5000);
  restartWithReason(REASON_OTA_UPDATE);
}

void setupOta() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  // Reboot from loop() instead, after sending what's queued.
  ArduinoOTA.setRebootOnSuccess(false);
  ArduinoOTA.onStart([]() {
    logf("[OTA] update starting");
    // The new image must bring back every sensor that works now.
    prefs.begin(NVS_NS, false);
    prefs.putUChar("pre_health", currentHealth(millis()));
    prefs.end();
  });
  // The upload runs inside ArduinoOTA.handle() and can take longer than the
  // task watchdog's 30 s, so feed it as data arrives.
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) { esp_task_wdt_reset(); });
  ArduinoOTA.onEnd([]() { otaDone = true; });
  ArduinoOTA.onError([](ota_error_t err) { logf("[OTA] update failed (error %d)", (int)err); });
  ArduinoOTA.begin();
  logf("[OTA] ready: %s.local", OTA_HOSTNAME);
}

// Accepts updates only while the pump is idle (an upload stops sampling for
// ~15 s). An invitation that arrives while it runs goes unanswered, and the
// upload from the Mac fails; retry once the pump stops.
void serviceOta() {
  if (!deviceRunning) ArduinoOTA.handle();
  if (otaDone) restartAfterOta();
}

// ==========================================
// Pump (vibration) sensor
// ==========================================
// green = all clear, blue = pump running, flashing red = high water with the
// pump not running, flashing purple = high water with the pump running.
// Only writes to the LED when the colour changes.
void updateStatusLed() {
  static int lastColor = -1;
  const bool flashOn = (millis() / LED_FLASH_HALF_PERIOD_MS) % 2 == 0;
  int color;  // 0 off, 1 red, 2 purple, 3 blue, 4 green
  if (highWater) color = flashOn ? (deviceRunning ? 2 : 1) : 0;
  else color = deviceRunning ? 3 : 4;
  if (color == lastColor) return;
  lastColor = color;
  const uint8_t b = LED_BRIGHTNESS;
  switch (color) {
    case 1:  rgbLedWrite(LED_PIN, b, 0, 0); break;
    case 2:  rgbLedWrite(LED_PIN, b, 0, b); break;
    case 3:  rgbLedWrite(LED_PIN, 0, 0, b); break;
    case 4:  rgbLedWrite(LED_PIN, 0, b, 0); break;
    default: rgbLedWrite(LED_PIN, 0, 0, 0); break;
  }
}

// Reads X/Y/Z in one burst and reports whether the transfer worked (the
// Adafruit getEvent() gives no sign of a failed read, just zeros or garbage).
// Scaled the same way the library does, so the thresholds are unchanged.
bool readSample(float &x, float &y, float &z) {
  const uint8_t addr = ADXL345_DEFAULT_ADDRESS;
  Wire.beginTransmission(addr);
  Wire.write((uint8_t)ADXL345_REG_DATAX0);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, (uint8_t)6) != 6) return false;
  int16_t raw[3];
  for (int16_t &r : raw) {
    uint8_t lo = Wire.read();
    uint8_t hi = Wire.read();
    r = (int16_t)((hi << 8) | lo);
  }
  const float scale = ADXL345_MG2G_MULTIPLIER * SENSORS_GRAVITY_STANDARD;
  x = raw[0] * scale;
  y = raw[1] * scale;
  z = raw[2] * scale;
  return true;
}

// A reset can leave a sensor holding SDA low mid-byte, which no amount of
// rebooting the ESP32 clears. Clocking SCL up to 9 times lets it finish the
// byte and release the bus; then a STOP condition frees it.
void i2cBusRecover() {
  pinMode(SDA_PIN, INPUT_PULLUP);
  pinMode(SCL_PIN, OUTPUT);
  digitalWrite(SCL_PIN, HIGH);
  delayMicroseconds(10);
  for (int i = 0; i < 9 && digitalRead(SDA_PIN) == LOW; i++) {
    digitalWrite(SCL_PIN, LOW);  delayMicroseconds(10);
    digitalWrite(SCL_PIN, HIGH); delayMicroseconds(10);
  }
  // STOP: SDA low -> high while SCL high.
  pinMode(SDA_PIN, OUTPUT);
  digitalWrite(SDA_PIN, LOW);  delayMicroseconds(10);
  digitalWrite(SCL_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(SDA_PIN, HIGH); delayMicroseconds(10);
}

// Diagnostic for a missing sensor: logs the level of each line at rest (both
// should idle HIGH via pull-ups; SDA or SCL stuck LOW means a short or a wedged
// device) and every address that ACKs. Expected: ADXL346 at 0x53 (ALT ADDRESS
// pin low) or 0x1D (high), AHT20 at 0x38.
void i2cScanLog() {
  logf("[I2C] idle levels: SDA=%d SCL=%d", digitalRead(SDA_PIN), digitalRead(SCL_PIN));
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      logf("[I2C] device ACKs at 0x%02X", addr);
      found++;
    }
  }
  if (!found) logf("[I2C] scan found no devices");
}

// accel.begin() often fails on its first call after the bus has been idle
// (it re-runs Wire.begin(), and the first transfer then fails with
// ESP_ERR_INVALID_STATE) yet succeeds when repeated straight away, even though
// the sensor is wired correctly. So retry a few times before giving up.
bool isAccelId(uint8_t id) { return id == ADXL346_DEVICE_ID || id == ADXL345_DEVICE_ID; }

bool beginAccel() {
  for (int attempt = 1; attempt <= 5; attempt++) {
    bool ok = accel.begin();
    // begin() rejects the ADXL346's ID before enabling measurements; finish
    // the job here (POWER_CTL: measure mode), as begin() does for an ADXL345.
    if (!ok && accel.getDeviceID() == ADXL346_DEVICE_ID) {
      accel.writeRegister(ADXL345_REG_POWER_CTL, 0x08);
      ok = true;
    }
    if (ok) {
      logf("[I2C] %s detected%s", accel.getDeviceID() == ADXL346_DEVICE_ID ? "ADXL346" : "ADXL345",
           attempt > 1 ? (String(" on attempt ") + attempt).c_str() : "");
      return true;
    }
    delay(50);
  }
  return false;
}

// Initializes the accelerometer if it answers.
bool startAccel() {
  if (!beginAccel()) return false;
  accel.setRange(ADXL345_RANGE_16_G);
  accel.setDataRate(ADXL345_DATARATE_400_HZ);
  if (!readSample(bx, by, bz)) return false;
  accelOk = true;
  accelReadFails = 0;
  flatWindows = 0;
  accelOkSinceMs = millis();
  sensorFails = 0;
  logf("[I2C] accelerometer initialized");
  return true;
}

// Carries on without the accelerometer: pump detection pauses (the run state
// is frozen, not guessed), everything else continues, and one push says so.
void accelLost(const char *why) {
  accelOk = false;
  lastAccelRetryMs = millis();
  if (deviceRunning) {
    // Lost mid-run: the run's end can't be known. Close it out (no run time)
    // so a stale "running" doesn't hold off OTA and the nightly reboot.
    deviceRunning = false;
    dropCheckPending = false;
    tgPoint("sump_event", "event=\"stop\",running=0i,note=\"sensor_lost\"");
    lastRecordedState = 0;
    saveLastRecordedState(0);
  }
  aboveSince = 0;
  belowSince = 0;
  vibPending.reset();
  vibThisRun.reset();
  logf("[I2C] accelerometer %s; pump detection paused, retrying every %lu s",
       why, (unsigned long)(ACCEL_RETRY_MS / 1000));
  if (accelAlerted) return;
  accelAlerted = true;
  notify(PRIO_ALERT, "warning", DEVICE_NAME ": accelerometer not responding",
         "Accelerometer (ADXL346) %s. Pump on/off detection is paused; water level, alarms "
         "and reporting carry on. Retrying every %lu s.", why, (unsigned long)(ACCEL_RETRY_MS / 1000));
}

// Finds the accelerometer at boot, without waiting for it: if it's missing
// the board runs without it (see accelLost()).
void setupSensor() {
  // On a cold power-up the first bus start can leave the sensors unreachable
  // (every read fails, or returns garbage); stopping the bus, recovering it and
  // starting it again has always worked. That's what the 30 s retry does, so do
  // it here too, a few times, before giving up and waiting for that retry.
  for (int attempt = 1; attempt <= 3; attempt++) {
    if (attempt > 1) {
      logf("[I2C] accelerometer not found, restarting the bus (try %d of 3)", attempt);
      Wire.end();
      delay(200);
    }
    i2cBusRecover();
    Wire.begin(SDA_PIN, SCL_PIN, I2C_CLOCK_HZ);
    if (startAccel()) return;
  }
  logf("[I2C] accelerometer (ADXL346) not detected at boot -- check wiring (SDA=%d SCL=%d)", SDA_PIN, SCL_PIN);
  i2cScanLog();
  accelLost("not detected at boot");
}

// Periodically confirms the accelerometer still answers over I2C (without this
// a disconnected/wedged sensor reads as zeros and looks like an idle pump), and
// retries it while it's missing.
void checkSensorWatchdog(uint32_t now) {
  if (!accelOk) {
    if (now - lastAccelRetryMs < ACCEL_RETRY_MS) return;
    lastAccelRetryMs = now;
    // A sensor holding SDA low needs the same bus recovery as at boot.
    Wire.end();
    i2cBusRecover();
    Wire.begin(SDA_PIN, SCL_PIN, I2C_CLOCK_HZ);
    if (startAccel() && accelAlerted) {
      accelAlerted = false;
      notify(PRIO_INFO, "white_check_mark", DEVICE_NAME ": accelerometer OK",
             "Accelerometer answering again; pump on/off detection resumed.");
    }
    return;
  }

  if (now - lastSensorCheck < SENSOR_CHECK_INTERVAL_MS) return;
  lastSensorCheck = now;
  // Answering its ID isn't enough: also check it is still in measure mode
  // (POWER_CTL bit 3), or it reads a constant and the pump looks idle forever.
  bool idOk = isAccelId(accel.getDeviceID());
  if (idOk && (accel.readRegister(ADXL345_REG_POWER_CTL) & 0x08)) {
    sensorFails = 0;
    return;
  }
  if (idOk) logf("[I2C] accelerometer answers but is not in measure mode");
  sensorFails++;
  logf("[I2C] accelerometer not responding (%d/%d)", sensorFails, SENSOR_FAIL_LIMIT);
  if (sensorFails >= SENSOR_FAIL_LIMIT) accelLost("stopped responding on I2C");
}

// ==========================================
// Pump runs and the level check
// ==========================================
// Records the level just before a run, to compare with the level after it.
void startDropCheck(uint32_t now) {
  if (dropCheckPending) {
    // Started again before the level was re-read after the last run.
    dropCheckPending = false;
    logf("Pump cycle: restarted before the level was re-read, not checked");
  }
  // A reading from the first seconds of the run (before the start was
  // confirmed) is still close to the "on" level.
  runLevelBefore = (levelFresh(now) && !levelNearLimit) ? levelCm : NAN;
}

// Compares the level before and after a run (after = NAN if there's no
// reading). Logged for every run, since it's also how the pump's normal on/off
// levels are learned.
void finishDropCheck(float after) {
  dropCheckPending = false;
  float before = runLevelBefore;
  char fields[160] = "";
  addField(fields, sizeof(fields), "seconds=%.1f", dropRunSeconds);
  if (!isnan(before)) addField(fields, sizeof(fields), "level_before_cm=%.1f", before);
  if (!isnan(after))  addField(fields, sizeof(fields), "level_after_cm=%.1f", after);

  if (isnan(before) || isnan(after)) {
    logf("Pump cycle: ran %.1f s, level before %s, after %s -- can't verify", dropRunSeconds,
         isnan(before) ? "unknown" : String(before, 1).c_str(),
         isnan(after) ? "unknown" : String(after, 1).c_str());
    addField(fields, sizeof(fields), "result=\"unverified\"");
    tgPoint("sump_cycle", fields);
    // The level sensor's own alert already covers a dead sensor.
    if (!levelAlerted) {
      notify(PRIO_INFO, "grey_question", DEVICE_NAME ": can't verify pump run",
             "Ran %.1f s, but there was no level reading %s the run, so it's unknown whether the level dropped.",
             dropRunSeconds, isnan(before) ? "before" : "after");
    }
    return;
  }

  float drop = before - after;
  bool ok = drop >= cfg.minDropCm;
  logf("Pump cycle: on at %.1f cm, off at %.1f cm, dropped %.1f cm in %.1f s%s",
       before, after, drop, dropRunSeconds, ok ? "" : " -- LESS THAN EXPECTED");
  addField(fields, sizeof(fields), "drop_cm=%.1f", drop);
  addField(fields, sizeof(fields), "result=\"%s\"", ok ? "ok" : "no_drop");
  tgPoint("sump_cycle", fields);
  if (!ok) {
    notify(PRIO_ALERT, "warning", DEVICE_NAME ": pump ran but level didn't drop",
           "Ran %.1f s; water %.1f -> %.1f cm (dropped %.1f cm, expected at least %.1f). "
           "Check the discharge line, check valve and pump.",
           dropRunSeconds, before, after, drop, cfg.minDropCm);
  }
}

void setRunState(bool running, uint32_t eventMs) {
  deviceRunning = running;
  uint32_t ageMs = millis() - eventMs;
  char at[32];
  formatEventTime(at, sizeof(at), ageMs, "%Y-%m-%d %H:%M:%S %Z");
  if (!at[0]) snprintf(at, sizeof(at), "uptime %lus", eventMs / 1000);
  char fields[128] = "";

  if (running) {
    runStartMs = eventMs;
    startDropCheck(millis());
    logf("%s STARTED", DEVICE_NAME);
    addField(fields, sizeof(fields), "event=\"start\",running=1i");
    if (!isnan(runLevelBefore)) addField(fields, sizeof(fields), "level_cm=%.1f", runLevelBefore);
    if (!isnan(runLevelBefore)) {
      notify(PRIO_PUMP, "", DEVICE_NAME " started", "Started at %s (water %.1f cm)", at, runLevelBefore);
    } else {
      notify(PRIO_PUMP, "", DEVICE_NAME " started", "Started at %s", at);
    }
  } else {
    uint32_t dur = eventMs - runStartMs;
    float seconds = dur / 1000.0f;
    cycleCount++;
    totalRunMs += dur;
    periodCycles++;
    periodRunMs += dur;
    if (dur > longestRunMs) longestRunMs = dur;
    logf("%s STOPPED - ran %.1f s (cycle %lu, total run %.1f s)", DEVICE_NAME,
         seconds, (unsigned long)cycleCount, totalRunMs / 1000.0);
    addField(fields, sizeof(fields), "event=\"stop\",running=0i,seconds=%.1f", seconds);
    // How hard the pump shook during this run, for setting the thresholds.
    if (vibThisRun.n) {
      addField(fields, sizeof(fields), "rms_avg=%.3f,rms_min=%.3f", vibThisRun.avg(), vibThisRun.mn);
      vibLastRun = vibThisRun;
      lastRunSeconds = seconds;
      lastRunEndMs = millis();
      logf("[vib] run: average %.3f, lowest %.3f, highest %.3f m/s^2 RMS", vibThisRun.avg(), vibThisRun.mn, vibThisRun.mx);
    }
    vibThisRun.reset();
    notify(PRIO_PUMP, "", DEVICE_NAME " stopped", "Stopped at %s (ran %.1f s)", at, seconds);
    dropCheckPending = true;
    dropStopMs = eventMs;
    dropRunSeconds = seconds;
  }
  tgPoint("sump_event", fields, ageMs);
  mqttDirty = true;
  lastRecordedState = running ? 1 : 0;
  saveLastRecordedState(lastRecordedState);
}

void countIdle(const RmsAcc &a)    { vibIdle.absorb(a); vibIdleBoot.absorb(a); }
void countIdle(float rms)          { vibIdle.add(rms); vibIdleBoot.add(rms); }
void countRunning(const RmsAcc &a) { vibRun.absorb(a); vibThisRun.absorb(a); }
void countRunning(float rms)       { vibRun.add(rms); vibThisRun.add(rms); }

// Called once per window with that window's RMS.
void processWindow(float rms, uint32_t now) {
  lastRms = rms;
  if (rms > maxRmsSinceStatus) maxRmsSinceStatus = rms;
  if (rms >= cfg.offRms) loudWindowsSinceStatus++;
  if (!accelOk) return;  // no pump detection without the accelerometer

  // Debounced running/stopped state. Each window is also counted in the idle or
  // running vibration statistics; windows seen while a start or stop is being
  // confirmed wait in vibPending until it's known which they were.
  if (rms > cfg.onRms) {
    if (!aboveSince) aboveSince = now;
    belowSince = 0;
    if (deviceRunning) {
      countRunning(vibPending);   // a stop that didn't happen
      vibPending.reset();
      countRunning(rms);
    } else {
      vibPending.add(rms);
      if (now - aboveSince >= cfg.runConfirmMs) {
        countRunning(vibPending);
        vibPending.reset();
        setRunState(true, aboveSince);
      }
    }
  } else if (rms < cfg.offRms) {
    if (!belowSince) belowSince = now;
    aboveSince = 0;
    if (!deviceRunning) {
      countIdle(vibPending);      // a start that didn't happen (a bump, footsteps)
      vibPending.reset();
      countIdle(rms);
    } else {
      vibPending.add(rms);
      if (now - belowSince >= cfg.stopConfirmMs) {
        countIdle(vibPending);
        vibPending.reset();
        setRunState(false, belowSince);
      }
    }
  } else {
    // Hysteresis band: neither counts toward starting a run (footsteps are
    // impulsive, a motor is continuous) nor toward ending one.
    aboveSince = 0;
    belowSince = 0;
    if (deviceRunning) { countRunning(vibPending); countRunning(rms); }
    else               { countIdle(vibPending);    countIdle(rms); }
    vibPending.reset();
  }
}

// If we restarted (crash, watchdog, nightly reboot...) while the pump's last
// recorded event was a "start", Grafana and ntfy would keep implying it's
// running. After a short settle time, if it's really idle, record a stop
// (marked as a restart correction, with no run length).
void checkBootStateCorrection(uint32_t now) {
  // Needs the pump observed (accelerometer answering) for the settle time.
  if (bootCorrectionDone || !accelOk || now - accelOkSinceMs < BOOT_SETTLE_MS) return;
  bootCorrectionDone = true;
  if (lastRecordedState == 1 && !deviceRunning) {
    logf("Last event was a start but the pump is idle after a restart; correcting");
    tgPoint("sump_event", "event=\"stop\",running=0i,note=\"restart\"");
    notify(PRIO_PUMP, "", DEVICE_NAME " stopped",
           "Stopped while the board was restarting (%s); run time unknown", logTimestamp());
    lastRecordedState = 0;
    saveLastRecordedState(0);
  }
}

// ==========================================
// Water level
// ==========================================
// Speed of sound in cm/us, corrected for air temperature when it's known.
float soundCmPerUs(uint32_t now) {
  float t = ahtFresh(now) ? tempC : 20.0f;
  return (331.3f + 0.606f * t) / 10000.0f;
}

void checkHighWater(uint32_t now) {
  bool high = levelNearLimit || levelCm >= cfg.highWaterCm;
  if (!highWater && high) {
    highWater = true;
    mqttDirty = true;
    lastHighAlertMs = now;
    logf("HIGH WATER: %s", levelText().c_str());
    char fields[96] = "";
    addField(fields, sizeof(fields), "type=\"high_water\",active=1i,level_cm=%.1f", levelCm);
    tgPoint("sump_alarm", fields);
    notify(PRIO_URGENT, "rotating_light", DEVICE_NAME ": HIGH WATER",
           "Water %s (alarm at %.1f cm). Pump %s.", levelText().c_str(), cfg.highWaterCm,
           deviceRunning ? "running" : "not running");
  } else if (highWater && !levelNearLimit && levelCm < cfg.highWaterCm - cfg.highWaterHystCm) {
    highWater = false;
    mqttDirty = true;
    logf("High water cleared: %s", levelText().c_str());
    char fields[96] = "";
    addField(fields, sizeof(fields), "type=\"high_water\",active=0i,level_cm=%.1f", levelCm);
    tgPoint("sump_alarm", fields);
    notify(PRIO_INFO, "white_check_mark", DEVICE_NAME ": water level OK",
           "All clear: water back down to %s.", levelText().c_str());
  }
}

// Repeats the high-water alarm every 30 minutes until the level drops, whether
// or not there's a current reading.
void checkHighWaterRepeat(uint32_t now) {
  if (!highWater || now - lastHighAlertMs < HIGH_WATER_REPEAT_MS) return;
  lastHighAlertMs = now;
  if (levelFresh(now)) {
    notify(PRIO_URGENT, "rotating_light", DEVICE_NAME ": STILL HIGH WATER",
           "Water %s (alarm at %.1f cm). Pump %s.", levelText().c_str(), cfg.highWaterCm,
           deviceRunning ? "running" : "not running");
  } else {
    notify(PRIO_URGENT, "rotating_light", DEVICE_NAME ": STILL HIGH WATER",
           "No current level reading; last was %s. Pump %s.", levelText().c_str(),
           deviceRunning ? "running" : "not running");
  }
}

// A new reading has been accepted.
void onLevelAccepted(uint32_t now) {
  if (isnan(maxLevelCm) || levelCm > maxLevelCm) maxLevelCm = levelCm;
  checkHighWater(now);
  if (dropCheckPending && levelAtMs >= dropStopMs + LEVEL_SETTLE_MS) {
    finishDropCheck(levelNearLimit ? NAN : levelCm);
  }
}

// Combines a finished burst into one reading and filters it.
void evaluateBurst(uint32_t now) {
  LevelStatus status;
  float dist = 0;
  if (burstEchoes >= LEVEL_MIN_ECHOES) {
    // Median of the valid echoes (insertion sort; at most LEVEL_PINGS values).
    for (int i = 1; i < burstEchoes; i++) {
      float v = burstCm[i];
      int j = i - 1;
      while (j >= 0 && burstCm[j] > v) { burstCm[j + 1] = burstCm[j]; j--; }
      burstCm[j + 1] = v;
    }
    dist = burstCm[burstEchoes / 2];
    status = LEVEL_OK;
  } else if (burstNear >= LEVEL_MIN_ECHOES) {
    dist = LEVEL_MIN_CM;
    status = LEVEL_NEAR_LIMIT;
  } else {
    status = LEVEL_MISSING;
  }

  if (status == LEVEL_MISSING) {
    static uint32_t lastLogMs = 0;
    if (!lastLogMs || now - lastLogMs >= 60000) {
      lastLogMs = now;
      logf("[level] no reliable echo (%d echoes, %d too near, of %d pings)", burstEchoes, burstNear, LEVEL_PINGS);
    }
    return;
  }
  lastEchoMs = now;
  if (levelAlerted) {
    levelAlerted = false;
    notify(PRIO_INFO, "white_check_mark", DEVICE_NAME ": level sensor OK", "Level readings are back.");
  }

  float depth = cfg.sensorToFloorCm - dist;
  // The first reading after boot has nothing to compare with, so it has to be
  // confirmed by a second burst (a loose contact can otherwise raise the high
  // water alarm on one bad reading).
  if (!haveLevel) {
    if (!candidateValid || fabsf(depth - candidateCm) > LEVEL_CONFIRM_CM) {
      candidateValid = true;
      candidateCm = depth;
      logf("[level] first reading %.1f cm, waiting for confirmation", depth);
      return;
    }
  } else if (fabsf(depth - levelCm) > LEVEL_MAX_STEP_CM) {
    if (!candidateValid || fabsf(depth - candidateCm) > LEVEL_CONFIRM_CM) {
      candidateValid = true;
      candidateCm = depth;
      logf("[level] suspect jump %.1f -> %.1f cm, waiting for confirmation", levelCm, depth);
      return;
    }
    logf("[level] jump to %.1f cm confirmed", depth);
  }
  candidateValid = false;
  haveLevel = true;
  levelCm = depth;
  levelDistCm = dist;
  levelNearLimit = status == LEVEL_NEAR_LIMIT;
  levelAtMs = now;
  onLevelAccepted(now);
}

// Called once per 250 ms window: sends one ping per window during a burst and
// collects the previous ping's echo.
void serviceLevel(uint32_t now) {
  if (!burstActive) {
    if (now - lastBurstMs < LEVEL_INTERVAL_MS) return;
    lastBurstMs = now;
    burstActive = true;
    burstPing = 0;
    burstEchoes = 0;
    burstNear = 0;
    sonar.trigger();
    return;
  }

  uint32_t echoUs;
  JsnSr04t::Ping r = sonar.result(echoUs);
  if (r == JsnSr04t::PENDING) return;
  if (r == JsnSr04t::ECHO) {
    float cm = echoUs * soundCmPerUs(now) / 2.0f;
    if (cm < LEVEL_MIN_CM) burstNear++;
    else if (cm <= LEVEL_MAX_CM) burstCm[burstEchoes++] = cm;
  }
  if (++burstPing < LEVEL_PINGS) {
    sonar.trigger();
    return;
  }
  burstActive = false;
  evaluateBurst(now);
}

void checkLevelWatchdogs(uint32_t now) {
  uint32_t since = lastEchoMs ? now - lastEchoMs : now;
  if (!levelAlerted && since >= LEVEL_MISSING_ALERT_MS) {
    levelAlerted = true;
    notify(PRIO_ALERT, "warning", DEVICE_NAME ": level sensor not responding",
           "No reliable echo for %lu min. High-water and pump-run checks are blind until it recovers.",
           (unsigned long)(since / 60000));
  }
  if (dropCheckPending && now - dropStopMs >= LEVEL_AFTER_TIMEOUT_MS) {
    finishDropCheck(NAN);
  }
}

// ==========================================
// Temperature / humidity
// ==========================================
// Triggers a measurement every AHT_INTERVAL_MS and reads it in the next
// window (it needs 80 ms).
void serviceAht(uint32_t now) {
  if (!ahtMeasuring) {
    uint32_t interval = ahtReady ? AHT_INTERVAL_MS
                        : ahtRetries < AHT_FAST_RETRIES ? AHT_FAST_RETRY_MS : AHT_MISSING_RETRY_MS;
    if (lastAhtStartMs && now - lastAhtStartMs < interval) return;
    lastAhtStartMs = now;
    if (!ahtReady) ahtReady = aht.begin();
    if (ahtReady && aht.start()) {
      ahtMeasuring = true;
    } else {
      ahtReady = false;
      ahtRetries++;
    }
    return;
  }

  float t, h;
  Aht20::Result r = aht.read(t, h);
  if (r == Aht20::BUSY && now - lastAhtStartMs < 1000) return;
  ahtMeasuring = false;
  if (r != Aht20::OK) {
    ahtReady = false;  // re-initialize before the next attempt
    ahtRetries++;
    logf("[AHT20] read failed (%s)", r == Aht20::BUSY ? "busy" : "no answer or bad CRC");
    return;
  }
  tempC = t;
  humidity = h;
  ahtAtMs = now;
  ahtRetries = 0;
  if (ahtAlerted) {
    ahtAlerted = false;
    notify(PRIO_INFO, "white_check_mark", DEVICE_NAME ": temp/humidity sensor OK", "AHT20 readings are back.");
  }
}

void checkAhtWatchdog(uint32_t now) {
  uint32_t since = ahtAtMs ? now - ahtAtMs : now;
  if (ahtAlerted || since < AHT_ALERT_MS) return;
  ahtAlerted = true;
  notify(PRIO_ALERT, "warning", DEVICE_NAME ": temp/humidity sensor not responding",
         "No AHT20 reading for %lu min (I2C address 0x38).", (unsigned long)(since / 60000));
}

// ==========================================
// Periodic reading and daily report
// ==========================================
// Every 10 minutes on the wall clock (uptime-based until NTP syncs).
void checkPeriodicReading(uint32_t now) {
  time_t t = time(nullptr);
  uint32_t slot = t > 100000 ? (uint32_t)(t / READING_PERIOD_S) : now / (READING_PERIOD_S * 1000);
  if (slot == lastReadingSlot) return;
  bool first = lastReadingSlot == UINT32_MAX;
  lastReadingSlot = slot;
  if (first) return;  // wait for the first full boundary

  char fields[TG_LINE_LEN] = "";
  if (levelFresh(now)) {
    addField(fields, sizeof(fields), "level_cm=%.1f", levelCm);
    if (!levelNearLimit) addField(fields, sizeof(fields), "distance_cm=%.1f", levelDistCm);
    addField(fields, sizeof(fields), "level_status=\"%s\"", levelNearLimit ? "near_limit" : "ok");
  } else {
    addField(fields, sizeof(fields), "level_status=\"missing\"");
  }
  if (ahtFresh(now)) {
    addField(fields, sizeof(fields), "temp_c=%.1f", tempC);
    addField(fields, sizeof(fields), "humidity=%.1f", humidity);
  }
  addField(fields, sizeof(fields), "running=%di", deviceRunning ? 1 : 0);
  addField(fields, sizeof(fields), "pump_sensor=%di", accelOk ? 1 : 0);
  addField(fields, sizeof(fields), "cycles=%lui", (unsigned long)periodCycles);
  addField(fields, sizeof(fields), "run_s=%.1f", periodRunMs / 1000.0f);
  addField(fields, sizeof(fields), "rssi=%di", (int)WiFi.RSSI());
  addField(fields, sizeof(fields), "heap=%lui", (unsigned long)ESP.getFreeHeap());
  // Vibration over this period, split by pump state, and the thresholds in
  // force, so the dashboard can show whether they sit between the two.
  if (vibIdle.n) addField(fields, sizeof(fields), "rms_idle_avg=%.3f,rms_idle_max=%.3f", vibIdle.avg(), vibIdle.mx);
  if (vibRun.n)  addField(fields, sizeof(fields), "rms_run_avg=%.3f,rms_run_min=%.3f", vibRun.avg(), vibRun.mn);
  addField(fields, sizeof(fields), "rms_on=%.3f,rms_off=%.3f", cfg.onRms, cfg.offRms);
  tgPoint("sump", fields);
  periodCycles = 0;
  periodRunMs = 0;
  vibIdle.reset();
  vibRun.reset();
}

void sendDailyReport(uint32_t now) {
  if (!net.ntfyTopic[0]) return;
  char level[80], climate[64], since[32] = "boot", stats[160];
  if (levelFresh(now)) snprintf(level, sizeof(level), "%s", levelText().c_str());
  else snprintf(level, sizeof(level), "no current reading");
  if (ahtFresh(now)) {
    snprintf(climate, sizeof(climate), "%.1f C (%.1f F), humidity %.0f%%",
             tempC, tempC * 9.0f / 5.0f + 32.0f, humidity);
  } else {
    snprintf(climate, sizeof(climate), "no current reading");
  }
  if (bootEpoch) {
    struct tm tm;
    localtime_r(&bootEpoch, &tm);
    strftime(since, sizeof(since), "%a %H:%M", &tm);
  }
  snprintf(stats, sizeof(stats), "Since %s: %lu cycle(s), %.1f min total run, longest %.1f s",
           since, (unsigned long)cycleCount, totalRunMs / 60000.0f, longestRunMs / 1000.0f);
  if (!isnan(maxLevelCm)) {
    size_t n = strlen(stats);
    snprintf(stats + n, sizeof(stats) - n, ", highest water %.1f cm", maxLevelCm);
  }

  NtfyMsg m;
  snprintf(m.title, sizeof(m.title), "%s daily report", DEVICE_NAME);
  snprintf(m.body, sizeof(m.body),
           "Pump: %s\nWater: %s\nAir: %s\n%s\nFirmware %s",
           !accelOk ? "unknown (accelerometer not responding)" : deviceRunning ? "on" : "off",
           level, climate, stats, FIRMWARE_VERSION);
  m.priority = PRIO_REPORT;
  m.tags = "clipboard";
  logf("[ntfy] daily report:\n%s", m.body);
  for (int attempt = 0; attempt < 3; attempt++) {
    esp_task_wdt_reset();
    int code = sendNtfy(m);
    if (code >= 200 && code < 300) return;
    delay(2000);
  }
  logf("[ntfy] daily report could not be sent");
}

// Reboots once per day at REBOOT_HOUR local time (see TZ_STRING), sending the
// daily report first. Held off while the pump is running, anything is pending
// or high water is active; the whole REBOOT_HOUR hour is the window.
void checkDailyReboot(uint32_t nowMs) {
  time_t now = time(nullptr);
  if (now < 100000) return; // NTP hasn't synced yet

  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  if (timeinfo.tm_hour != REBOOT_HOUR || timeinfo.tm_mday == lastRebootDay) return;
  if (deviceRunning || ntfyCount > 0 || dropCheckPending || highWater || otaTrial) return;

  lastRebootDay = timeinfo.tm_mday;
  // Persisted, not just RAM: otherwise the reboot this triggers would see "not
  // rebooted today" again on the next boot (still inside the same hour) and
  // loop for the rest of the hour.
  prefs.begin(NVS_NS, false);
  prefs.putInt("last_reboot_day", lastRebootDay);
  prefs.end();
  sendDailyReport(nowMs);
  flushQueues(5000);
  restartWithReason(REASON_DAILY_REBOOT);
}

// ==========================================
// Network
// ==========================================
void startNetwork() {
  WiFi.setHostname(OTA_HOSTNAME);
  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_TX_POWER);
  WiFi.setAutoReconnect(true);
  WiFi.begin(net.wifiSsid, net.wifiPass);
}

// Bounded connect at boot: reboot and retry rather than sit there offline.
void setupWiFi() {
  startNetwork();
  lastWifiAttempt = millis();
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(250);
    if (millis() - start > WIFI_CONNECT_TIMEOUT_MS) {
      Serial.println();
      restartWithReason("WiFi connect timeout at boot");
    }
  }
  Serial.println();
  logf("[WiFi] IP address %s", WiFi.localIP().toString().c_str());
}

void setupTime() {
  // Re-kick SNTP now that WiFi is up (the early configTzTime() in setup() ran
  // with no network, so its first sync attempt failed and backed off).
  configTzTime(TZ_STRING, NTP_SERVER);
  logf("[NTP] Syncing time");
  time_t now = time(nullptr);
  uint32_t start = millis();
  while (now < 100000 && millis() - start < 10000) {
    Serial.print(".");
    delay(250);
    now = time(nullptr);
  }
  Serial.println();
  if (now < 100000) {
    logf("[NTP] failed to sync (will keep retrying in the background)");
    return;
  }
  logf("[NTP] synced");
}

// Notes the boot time once the clock is valid (for the daily report).
void checkBootEpoch() {
  if (bootEpoch) return;
  time_t now = time(nullptr);
  if (now > 100000) bootEpoch = now - millis() / 1000;
}

// Keeps WiFi up and sends queued data and pushes.
void serviceNetwork(uint32_t now) {
  if (WiFi.status() != WL_CONNECTED) {
    if (now - lastWifiAttempt >= WIFI_RETRY_MS) {
      lastWifiAttempt = now;
      // A connect attempt can get stuck; fully reset the radio first.
      WiFi.disconnect(true);
      delay(200);
      startNetwork();  // brief blocking only
    }
    return;
  }
  // At most one request of each kind per window, so sampling resumes quickly.
  serviceTelegraf(now);
  serviceNtfy(now);
}

// Reboots if WiFi has stayed down too long; auto-reconnect and the retry in
// serviceNetwork() handle brief drops, this is the backstop.
void checkWiFiWatchdog() {
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiDownSince == 0) {
      wifiDownSince = millis();
    } else if (millis() - wifiDownSince > WIFI_DOWN_REBOOT_MS) {
      restartWithReason("WiFi down watchdog");
    }
  } else {
    wifiDownSince = 0;
  }
}

// ==========================================
// Arduino entry points
// ==========================================
// ==========================================
// Calibration settings (NVS) and the web page
// ==========================================
// Returns nullptr if the settings make sense, or what's wrong with them.
const char *settingsProblem(const Settings &c) {
  if (!(c.onRms >= 0.05f && c.onRms <= 20.0f)) return "The start threshold must be between 0.05 and 20 m/s^2.";
  if (!(c.offRms >= 0.02f && c.offRms <= c.onRms * 0.95f)) return "The stop threshold must be at least 0.02 and clearly below the start threshold.";
  if (c.runConfirmMs < 500 || c.runConfirmMs > 60000) return "The start time must be between 0.5 and 60 seconds.";
  if (c.stopConfirmMs < 500 || c.stopConfirmMs > 60000) return "The stop time must be between 0.5 and 60 seconds.";
  if (!(c.sensorToFloorCm >= 30.0f && c.sensorToFloorCm <= LEVEL_MAX_CM)) return "Sensor to pit floor must be between 30 and 450 cm.";
  if (!(c.highWaterCm >= 1.0f && c.highWaterCm < c.sensorToFloorCm)) return "The high-water depth must be at least 1 cm and less than the sensor-to-floor distance.";
  if (!(c.highWaterHystCm >= 0.5f && c.highWaterHystCm <= c.highWaterCm)) return "The alarm-clear distance must be between 0.5 cm and the high-water depth.";
  if (!(c.minDropCm >= 0.0f && c.minDropCm <= 100.0f)) return "The expected drop must be between 0 and 100 cm.";
  return nullptr;
}

void loadSettings() {
  Settings c = DEFAULT_SETTINGS;
  prefs.begin(NVS_NS, false);
  bool any = prefs.isKey("c_on");
  if (any) {
    c.onRms           = prefs.getFloat("c_on", c.onRms);
    c.offRms          = prefs.getFloat("c_off", c.offRms);
    c.runConfirmMs    = prefs.getULong("c_runms", c.runConfirmMs);
    c.stopConfirmMs   = prefs.getULong("c_stopms", c.stopConfirmMs);
    c.sensorToFloorCm = prefs.getFloat("c_floor", c.sensorToFloorCm);
    c.highWaterCm     = prefs.getFloat("c_high", c.highWaterCm);
    c.highWaterHystCm = prefs.getFloat("c_hyst", c.highWaterHystCm);
    c.minDropCm       = prefs.getFloat("c_drop", c.minDropCm);
  }
  prefs.end();
  const char *problem = settingsProblem(c);
  if (problem) {
    logf("[cfg] saved settings rejected (%s); using the firmware defaults", problem);
    c = DEFAULT_SETTINGS;
  }
  cfg = c;
  logf("[cfg] %s: start %.2f / stop %.2f m/s^2, confirm %.1f / %.1f s, floor %.1f cm, high water %.1f cm (-%.1f), drop %.1f cm",
       any && !problem ? "saved settings" : "firmware defaults", cfg.onRms, cfg.offRms, cfg.runConfirmMs / 1000.0f,
       cfg.stopConfirmMs / 1000.0f, cfg.sensorToFloorCm, cfg.highWaterCm, cfg.highWaterHystCm, cfg.minDropCm);
}

// Puts new settings into force and saves them (or, with useDefaults, forgets
// the saved ones). Depths already measured are shifted if the sensor-to-floor
// distance changed, so the level doesn't appear to jump.
void applySettings(const Settings &c, bool useDefaults) {
  float shift = c.sensorToFloorCm - cfg.sensorToFloorCm;
  cfg = c;
  prefs.begin(NVS_NS, false);
  if (useDefaults) {
    for (const char *k : {"c_on", "c_off", "c_runms", "c_stopms", "c_floor", "c_high", "c_hyst", "c_drop"}) prefs.remove(k);
  } else {
    prefs.putFloat("c_on", c.onRms);
    prefs.putFloat("c_off", c.offRms);
    prefs.putULong("c_runms", c.runConfirmMs);
    prefs.putULong("c_stopms", c.stopConfirmMs);
    prefs.putFloat("c_floor", c.sensorToFloorCm);
    prefs.putFloat("c_high", c.highWaterCm);
    prefs.putFloat("c_hyst", c.highWaterHystCm);
    prefs.putFloat("c_drop", c.minDropCm);
  }
  prefs.end();
  if (shift != 0) {
    if (haveLevel) levelCm += shift;
    if (!isnan(maxLevelCm)) maxLevelCm += shift;
    if (!isnan(runLevelBefore)) runLevelBefore += shift;
    candidateValid = false;
  }
  // The debounce timers were measured against the old thresholds.
  aboveSince = 0;
  belowSince = 0;
  logf("[cfg] %s: start %.2f / stop %.2f m/s^2, confirm %.1f / %.1f s, floor %.1f cm, high water %.1f cm (-%.1f), drop %.1f cm",
       useDefaults ? "reset to firmware defaults" : "settings changed", cfg.onRms, cfg.offRms, cfg.runConfirmMs / 1000.0f,
       cfg.stopConfirmMs / 1000.0f, cfg.sensorToFloorCm, cfg.highWaterCm, cfg.highWaterHystCm, cfg.minDropCm);
  char fields[200] = "";
  addField(fields, sizeof(fields), "rms_on=%.3f,rms_off=%.3f,run_confirm_s=%.1f,stop_confirm_s=%.1f", cfg.onRms, cfg.offRms,
           cfg.runConfirmMs / 1000.0f, cfg.stopConfirmMs / 1000.0f);
  addField(fields, sizeof(fields), "floor_cm=%.1f,high_water_cm=%.1f,hyst_cm=%.1f,min_drop_cm=%.1f", cfg.sensorToFloorCm,
           cfg.highWaterCm, cfg.highWaterHystCm, cfg.minDropCm);
  tgPoint("sump_config", fields);
  uint32_t now = millis();
  if (levelFresh(now)) checkHighWater(now);
}

// Appends to a JSON buffer; a NaN or missing number is written as null.
void jsonAdd(char *buf, size_t len, const char *fmt, ...) {
  size_t n = strlen(buf);
  if (n + 1 >= len) return;
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf + n, len - n, fmt, args);
  va_end(args);
}
void jsonNum(char *buf, size_t len, const char *key, float v, int decimals, bool valid = true) {
  if (valid && !isnan(v)) jsonAdd(buf, len, "\"%s\":%.*f,", key, decimals, v);
  else jsonAdd(buf, len, "\"%s\":null,", key);
}

void handleWebStatus() {
  static char j[1500];
  uint32_t now = millis();
  bool lvl = levelFresh(now), air = ahtFresh(now);
  j[0] = 0;
  jsonAdd(j, sizeof(j), "{\"device\":\"%s\",\"version\":\"%s\",\"time\":\"%s\",\"uptime_s\":%lu,", DEVICE_NAME, FIRMWARE_VERSION,
          logTimestamp(), (unsigned long)(now / 1000));
  jsonAdd(j, sizeof(j), "\"state\":\"%s\",\"high_water\":%s,", !accelOk ? "no-accel" : deviceRunning ? "running" : "idle",
          highWater ? "true" : "false");
  jsonNum(j, sizeof(j), "rms", lastRms, 3);
  jsonNum(j, sizeof(j), "rms_max", statusMaxRms, 3);
  jsonNum(j, sizeof(j), "level_cm", levelCm, 1, lvl);
  jsonNum(j, sizeof(j), "dist_cm", levelDistCm, 1, lvl && !levelNearLimit);
  jsonAdd(j, sizeof(j), "\"level_near\":%s,", lvl && levelNearLimit ? "true" : "false");
  jsonNum(j, sizeof(j), "temp_c", tempC, 1, air);
  jsonNum(j, sizeof(j), "humidity", humidity, 0, air);
  jsonAdd(j, sizeof(j), "\"rssi\":%d,\"heap\":%lu,\"tgq\":%d,\"cycles\":%lu,", (int)WiFi.RSSI(), (unsigned long)ESP.getFreeHeap(),
          tgCount, (unsigned long)cycleCount);
  jsonAdd(j, sizeof(j), "\"idle\":{\"n\":%lu,\"avg\":%.3f,\"max\":%.3f},", (unsigned long)vibIdleBoot.n, vibIdleBoot.avg(), vibIdleBoot.mx);
  jsonAdd(j, sizeof(j), "\"last_run\":{\"n\":%lu,\"avg\":%.3f,\"min\":%.3f,\"max\":%.3f,\"seconds\":%.1f,\"ago_s\":%lu},",
          (unsigned long)vibLastRun.n, vibLastRun.avg(), vibLastRun.mn, vibLastRun.mx, lastRunSeconds,
          (unsigned long)((now - lastRunEndMs) / 1000));
  jsonAdd(j, sizeof(j), "\"cfg\":{\"on\":%.3g,\"off\":%.3g,\"run_ms\":%lu,\"stop_ms\":%lu,\"floor_cm\":%.4g,\"high_cm\":%.4g,"
          "\"hyst_cm\":%.4g,\"drop_cm\":%.4g},", cfg.onRms, cfg.offRms, (unsigned long)cfg.runConfirmMs,
          (unsigned long)cfg.stopConfirmMs, cfg.sensorToFloorCm, cfg.highWaterCm, cfg.highWaterHystCm, cfg.minDropCm);
  // Connections. The ntfy topic and the passwords are secrets, so only whether
  // they're set is shown (this endpoint needs no login).
  jsonAdd(j, sizeof(j), "\"conn\":{\"wifi\":\"%s\",\"tg_url\":\"%s\",\"ntfy\":%s,\"mqtt_host\":\"%s\",\"mqtt_port\":%u,"
          "\"mqtt_user\":\"%s\",\"mqtt_pass\":%s,\"mqtt\":\"%s\"}}", net.wifiSsid, net.telegrafUrl,
          net.ntfyTopic[0] ? "true" : "false", net.mqttHost, (unsigned)net.mqttPort, net.mqttUser,
          net.mqttPass[0] ? "true" : "false", !net.mqttHost[0] ? "off" : mqtt.connected() ? "connected" : mqttProblem());
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "application/json", j);
}

// GET /api/log?since=N: the first line is the number of lines logged so far
// (pass it back as `since` next time), then every kept line from N on.
void handleWebLog() {
  uint32_t since = web.hasArg("since") ? (uint32_t)strtoul(web.arg("since").c_str(), nullptr, 10) : 0;
  uint32_t oldest = logSeq > (uint32_t)LOG_LINES ? logSeq - LOG_LINES : 0;
  if (since < oldest || since > logSeq) since = oldest;
  String out;
  out.reserve(16 + (logSeq - since) * 90);
  out += logSeq;
  out += '\n';
  for (uint32_t i = since; i < logSeq; i++) {
    out += logRing[i % LOG_LINES];
    out += '\n';
  }
  web.sendHeader("Cache-Control", "no-store");
  web.send(200, "text/plain; charset=utf-8", out);
}

void sendSettingsError(const char *problem) {
  String page = F("<!doctype html><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
                  "<body style=\"font:16px system-ui;padding:24px\"><h3>Settings not saved</h3><p>");
  page += problem;
  page += F("</p><p><a href=\"/\">Back</a></p>");
  web.send(400, "text/html; charset=utf-8", page);
}

// Digest authentication, so the admin password itself never crosses the
// network: user "admin", and the admin password (the OTA password until it's
// changed on the page). Asks the browser for them and returns false if needed.
bool webAuthorized() {
  if (web.authenticate("admin", net.adminPass)) return true;
  web.requestAuthentication(DIGEST_AUTH, "Sump Master 2000");
  return false;
}

// POST /settings (the calibration form on the page).
void handleWebSettings() {
  if (!webAuthorized()) return;
  if (web.hasArg("reset")) {
    applySettings(DEFAULT_SETTINGS, true);
    web.sendHeader("Location", "/?reset=1");
    web.send(303, "text/plain", "");
    return;
  }
  for (const char *k : {"on", "off", "run_s", "stop_s", "floor", "high", "hyst", "drop"}) {
    if (!web.hasArg(k) || !web.arg(k).length()) return sendSettingsError("Every field needs a number.");
  }
  Settings c;
  c.onRms           = web.arg("on").toFloat();
  c.offRms          = web.arg("off").toFloat();
  c.runConfirmMs    = (uint32_t)lroundf(constrain(web.arg("run_s").toFloat(), 0.0f, 1000.0f) * 1000.0f);
  c.stopConfirmMs   = (uint32_t)lroundf(constrain(web.arg("stop_s").toFloat(), 0.0f, 1000.0f) * 1000.0f);
  c.sensorToFloorCm = web.arg("floor").toFloat();
  c.highWaterCm     = web.arg("high").toFloat();
  c.highWaterHystCm = web.arg("hyst").toFloat();
  c.minDropCm       = web.arg("drop").toFloat();
  const char *problem = settingsProblem(c);
  if (problem) return sendSettingsError(problem);
  applySettings(c, false);
  web.sendHeader("Location", "/?saved=1");
  web.send(303, "text/plain", "");
}

// ==========================================
// Connection settings (NVS)
// ==========================================
// True if every character is printable ASCII other than space, quotes and
// backslash (so values can go into JSON, URLs and headers as they are).
bool plainText(const char *s) {
  for (; *s; s++) if (*s <= ' ' || *s > '~' || *s == '"' || *s == '\\' || *s == '\'') return false;
  return true;
}

void loadNetString(const char *key, char *out, size_t len, const char *fallback) {
  if (prefs.isKey(key)) prefs.getString(key, out, len);
  else snprintf(out, len, "%s", fallback);
}

void loadNetConfig() {
  prefs.begin(NVS_NS, false);
  loadNetString("n_ssid", net.wifiSsid, sizeof(net.wifiSsid), WIFI_SSID);
  loadNetString("n_wpass", net.wifiPass, sizeof(net.wifiPass), WIFI_PASS);
  loadNetString("n_tgurl", net.telegrafUrl, sizeof(net.telegrafUrl), TELEGRAF_URL);
  loadNetString("n_ntfy", net.ntfyTopic, sizeof(net.ntfyTopic), NTFY_TOPIC);
  loadNetString("n_mhost", net.mqttHost, sizeof(net.mqttHost), MQTT_HOST);
  loadNetString("n_muser", net.mqttUser, sizeof(net.mqttUser), MQTT_USER);
  loadNetString("n_mpass", net.mqttPass, sizeof(net.mqttPass), MQTT_PASS);
  loadNetString("n_admin", net.adminPass, sizeof(net.adminPass), OTA_PASSWORD);
  net.mqttPort = prefs.getUShort("n_mport", MQTT_PORT);
  prefs.end();
  logf("[net] Wi-Fi \"%s\"; Telegraf %s; ntfy %s; MQTT %s%s", net.wifiSsid, net.telegrafUrl[0] ? net.telegrafUrl : "off",
       net.ntfyTopic[0] ? "on" : "off", net.mqttHost[0] ? net.mqttHost : "off", net.mqttUser[0] ? " (with login)" : "");
}

void saveNetConfig() {
  prefs.begin(NVS_NS, false);
  prefs.putString("n_ssid", net.wifiSsid);
  prefs.putString("n_wpass", net.wifiPass);
  prefs.putString("n_tgurl", net.telegrafUrl);
  prefs.putString("n_ntfy", net.ntfyTopic);
  prefs.putString("n_mhost", net.mqttHost);
  prefs.putString("n_muser", net.mqttUser);
  prefs.putString("n_mpass", net.mqttPass);
  prefs.putString("n_admin", net.adminPass);
  prefs.putUShort("n_mport", net.mqttPort);
  prefs.end();
}

// ==========================================
// MQTT / Home Assistant
// ==========================================
// Why the last connection attempt failed, in words.
const char *mqttProblem() {
  switch (mqtt.state()) {
    case MQTT_CONNECT_BAD_CREDENTIALS:
    case MQTT_CONNECT_UNAUTHORIZED:    return "the broker refused the user name or password";
    case MQTT_CONNECT_FAILED:          return "no answer from the broker";
    case MQTT_CONNECTION_TIMEOUT:      return "the broker stopped answering";
    case MQTT_CONNECTION_LOST:         return "the connection dropped";
    case MQTT_CONNECT_BAD_CLIENT_ID:   return "the broker refused the client ID";
    case MQTT_CONNECT_UNAVAILABLE:     return "the broker is not accepting connections";
    default:                           return "not connected";
  }
}

void setupMqtt() {
  snprintf(mqttBase, sizeof(mqttBase), "sm2k/%s", DEVICE_NAME);
  mqtt.setBufferSize(1024);
  mqtt.setKeepAlive(30);
  mqtt.setSocketTimeout(3);          // seconds
  mqttNet.setConnectionTimeout(2000);
}

// Tells Home Assistant about one entity (retained, so it survives HA restarts).
bool publishHaDiscovery(const HaEntity &e) {
  char topic[96], msg[760];
  snprintf(topic, sizeof(topic), "homeassistant/%s/sm2k_%s/%s/config", e.component, DEVICE_NAME, e.id);
  int n = snprintf(msg, sizeof(msg),
                   "{\"name\":\"%s\",\"uniq_id\":\"sm2k_%s_%s\",\"stat_t\":\"%s/state\",\"avty_t\":\"%s/status\","
                   "\"val_tpl\":\"{{ value_json.%s | default('None') }}\"",
                   e.name, DEVICE_NAME, e.id, mqttBase, mqttBase, e.id);
  if (e.deviceClass[0]) n += snprintf(msg + n, sizeof(msg) - n, ",\"dev_cla\":\"%s\"", e.deviceClass);
  if (e.unit[0])        n += snprintf(msg + n, sizeof(msg) - n, ",\"unit_of_meas\":\"%s\"", e.unit);
  if (e.stateClass[0])  n += snprintf(msg + n, sizeof(msg) - n, ",\"stat_cla\":\"%s\"", e.stateClass);
  if (e.diagnostic)     n += snprintf(msg + n, sizeof(msg) - n, ",\"ent_cat\":\"diagnostic\"");
  n += snprintf(msg + n, sizeof(msg) - n,
                ",\"dev\":{\"ids\":[\"sm2k_%s\"],\"name\":\"Sump Master 2000\",\"mf\":\"bradrblack\","
                "\"mdl\":\"Sump Master 2000\",\"sw\":\"%s\",\"cu\":\"http://%s/\"}}",
                DEVICE_NAME, FIRMWARE_VERSION, WiFi.localIP().toString().c_str());
  if (n >= (int)sizeof(msg)) return false;
  return mqtt.publish(topic, msg, true);
}

// The whole state as one retained JSON message. A reading that isn't current
// is left out, which Home Assistant shows as "unknown".
bool publishMqttState(uint32_t now) {
  char topic[64], msg[520] = "{";
  snprintf(topic, sizeof(topic), "%s/state", mqttBase);
  bool lvl = levelFresh(now), air = ahtFresh(now);
  jsonAdd(msg, sizeof(msg), "\"pump\":\"%s\",\"high_water\":\"%s\",", deviceRunning ? "ON" : "OFF", highWater ? "ON" : "OFF");
  if (lvl) jsonAdd(msg, sizeof(msg), "\"level_cm\":%.1f,", levelCm);
  if (air) jsonAdd(msg, sizeof(msg), "\"temp_c\":%.1f,\"humidity\":%.0f,", tempC, humidity);
  if (accelOk) jsonAdd(msg, sizeof(msg), "\"vibration\":%.3f,", statusMaxRms);
  if (vibLastRun.n) jsonAdd(msg, sizeof(msg), "\"last_run_s\":%.1f,", lastRunSeconds);
  jsonAdd(msg, sizeof(msg), "\"cycles\":%lu,\"level_problem\":\"%s\",\"pump_sensor_problem\":\"%s\",",
          (unsigned long)cycleCount, lvl ? "OFF" : "ON", accelOk ? "OFF" : "ON");
  jsonAdd(msg, sizeof(msg), "\"rssi\":%d,\"uptime_s\":%lu,\"version\":\"%s\"}", (int)WiFi.RSSI(),
          (unsigned long)(now / 1000), FIRMWARE_VERSION);
  return mqtt.publish(topic, msg, true);
}

// Once per 250 ms window: keeps the broker connection up and sends at most one
// message, so sampling resumes quickly. A broker that's down costs one short
// connection attempt every MQTT_RETRY_MS.
void serviceMqtt(uint32_t now) {
  if (!net.mqttHost[0]) {
    if (mqtt.connected()) mqtt.disconnect();
    return;
  }
  if (WiFi.status() != WL_CONNECTED) return;
  if (!mqtt.connected()) {
    if (lastMqttAttemptMs && now - lastMqttAttemptMs < MQTT_RETRY_MS) return;
    lastMqttAttemptMs = now ? now : 1;
    char clientId[40], will[64];
    snprintf(clientId, sizeof(clientId), "sm2k-%s", DEVICE_NAME);
    snprintf(will, sizeof(will), "%s/status", mqttBase);
    mqtt.setServer(net.mqttHost, net.mqttPort);
    bool ok = mqtt.connect(clientId, net.mqttUser[0] ? net.mqttUser : nullptr, net.mqttUser[0] ? net.mqttPass : nullptr,
                           will, 0, true, "offline");
    if (!ok) {
      if (!mqttFailLogged) logf("[mqtt] can't connect to %s:%u: %s (state %d); retrying every %lu s", net.mqttHost,
                                (unsigned)net.mqttPort, mqttProblem(), mqtt.state(), (unsigned long)(MQTT_RETRY_MS / 1000));
      mqttFailLogged = true;
      return;
    }
    mqttFailLogged = false;
    logf("[mqtt] connected to %s:%u", net.mqttHost, (unsigned)net.mqttPort);
    mqtt.publish(will, "online", true);
    mqttDiscoveryIdx = 0;
    mqttDirty = true;
    return;
  }
  mqtt.loop();
  if (mqttDiscoveryIdx >= 0) {
    if (!publishHaDiscovery(HA_ENTITIES[mqttDiscoveryIdx])) logf("[mqtt] discovery for %s not sent", HA_ENTITIES[mqttDiscoveryIdx].id);
    if (++mqttDiscoveryIdx >= HA_ENTITY_COUNT) mqttDiscoveryIdx = -1;
    return;
  }
  if (mqttDirty || now - lastMqttStateMs >= MQTT_STATE_MS) {
    if (publishMqttState(now)) mqttDirty = false;
    lastMqttStateMs = now;
  }
}

// POST /connections (the connections form on the page). An empty Telegraf URL
// or MQTT host turns that output off. Secret fields left empty keep their
// saved value.
void handleWebConnections() {
  if (!webAuthorized()) return;
  NetConfig c = net;
  String tg = web.arg("tg_url"), topic = web.arg("ntfy"), host = web.arg("mqtt_host"), user = web.arg("mqtt_user"),
         pass = web.arg("mqtt_pass"), admin = web.arg("admin"), port = web.arg("mqtt_port");
  tg.trim(); topic.trim(); host.trim(); user.trim();
  if (tg.length() >= sizeof(c.telegrafUrl) || !plainText(tg.c_str()) || (tg.length() && !tg.startsWith("http://")))
    return sendSettingsError("The Telegraf address must start with http:// (or be empty to turn it off).");
  if (topic.length() >= sizeof(c.ntfyTopic) || !plainText(topic.c_str()) || topic.indexOf('/') >= 0)
    return sendSettingsError("The ntfy topic can only contain letters, digits, - and _.");
  if (host.length() >= sizeof(c.mqttHost) || !plainText(host.c_str()) || host.indexOf('/') >= 0 || host.indexOf(':') >= 0)
    return sendSettingsError("The MQTT broker must be a host name or IP address only (no mqtt:// and no port).");
  long p = port.length() ? port.toInt() : 1883;
  if (p < 1 || p > 65535) return sendSettingsError("The MQTT port must be between 1 and 65535.");
  if (user.length() >= sizeof(c.mqttUser) || !plainText(user.c_str())) return sendSettingsError("The MQTT user name is too long or has unusual characters.");
  if (pass.length() >= sizeof(c.mqttPass) || !plainText(pass.c_str())) return sendSettingsError("The MQTT password is too long or has spaces, quotes or a backslash.");
  if (admin.length() && (admin.length() < 8 || admin.length() >= sizeof(c.adminPass) || !plainText(admin.c_str())))
    return sendSettingsError("A new admin password must be 8 to 32 characters, with no spaces, quotes or backslash.");

  snprintf(c.telegrafUrl, sizeof(c.telegrafUrl), "%s", tg.c_str());
  if (web.hasArg("ntfy_off")) c.ntfyTopic[0] = 0;
  else if (topic.length()) snprintf(c.ntfyTopic, sizeof(c.ntfyTopic), "%s", topic.c_str());
  snprintf(c.mqttHost, sizeof(c.mqttHost), "%s", host.c_str());
  c.mqttPort = (uint16_t)p;
  snprintf(c.mqttUser, sizeof(c.mqttUser), "%s", user.c_str());
  if (!user.length()) c.mqttPass[0] = 0;
  else if (pass.length()) snprintf(c.mqttPass, sizeof(c.mqttPass), "%s", pass.c_str());
  if (admin.length()) snprintf(c.adminPass, sizeof(c.adminPass), "%s", admin.c_str());

  bool mqttChanged = strcmp(c.mqttHost, net.mqttHost) || c.mqttPort != net.mqttPort || strcmp(c.mqttUser, net.mqttUser) ||
                     strcmp(c.mqttPass, net.mqttPass);
  bool adminChanged = strcmp(c.adminPass, net.adminPass) != 0;
  net = c;
  saveNetConfig();
  if (mqttChanged) {
    mqtt.disconnect();
    lastMqttAttemptMs = 0;
    mqttFailLogged = false;
  }
  if (!net.telegrafUrl[0]) tgCount = 0;
  if (!net.ntfyTopic[0]) ntfyCount = 0;
  logf("[net] connections changed: Telegraf %s; ntfy %s; MQTT %s%s%s", net.telegrafUrl[0] ? net.telegrafUrl : "off",
       net.ntfyTopic[0] ? "on" : "off", net.mqttHost[0] ? net.mqttHost : "off", net.mqttUser[0] ? " (with login)" : "",
       adminChanged ? "; admin password changed" : "");
  web.sendHeader("Location", "/?conn=1");
  web.send(303, "text/plain", "");
}

void setupWeb() {
  web.enableDelay(false);  // handleClient() must return at once when nobody is connected
  web.on("/", HTTP_GET, []() { web.send_P(200, "text/html; charset=utf-8", WEB_PAGE); });
  web.on("/api/status", HTTP_GET, handleWebStatus);
  web.on("/api/log", HTTP_GET, handleWebLog);
  web.on("/settings", HTTP_POST, handleWebSettings);
  web.on("/connections", HTTP_POST, handleWebConnections);
  web.onNotFound([]() { web.send(404, "text/plain", "Not found"); });
  web.begin();
  MDNS.addService("http", "tcp", 80);
  logf("[web] ready: http://%s.local/ (http://%s/)", OTA_HOSTNAME, WiFi.localIP().toString().c_str());
}

// Once per 250 ms window. A request is answered in a few milliseconds, between
// vibration samples; a few calls let one be accepted, read and answered in the
// same window.
void serviceWeb() {
  if (WiFi.status() != WL_CONNECTED) return;
  for (int i = 0; i < 3; i++) web.handleClient();
}

void setup(void) {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block on a stalled USB host; excess output is dropped
  delay(3000);               // give USB-CDC time to attach so early messages aren't lost
  Serial.println();

  // Set the timezone before any logging: the RTC survives a soft reset, so
  // time() can already be valid at boot and would otherwise be read as UTC.
  configTzTime(TZ_STRING, NTP_SERVER);

  Serial.println(BANNER);
  Serial.println();

  logf("--- Sump Master 2000 (%s) v%s ---", DEVICE_NAME, FIRMWARE_VERSION);

  // Opened read-write so the namespace is created on a brand-new device (a
  // read-only open of a missing namespace logs an error on first boot).
  prefs.begin(NVS_NS, false);
  lastRebootDay = prefs.getInt("last_reboot_day", -1);
  lastRecordedState = prefs.getInt("last_state", -1);
  prefs.end();
  loadSettings();
  loadNetConfig();

  checkUnexpectedReset();
  checkOtaState();

  updateStatusLed();
  sonar.begin();

  setupWiFi();
  setupTime();
  checkBootEpoch();
  setupOta();
  setupWeb();
  setupMqtt();
  reportBoot();
  setupSensor();
  ahtReady = aht.begin();
  if (!ahtReady) logf("[AHT20] not detected at boot (I2C 0x38), will keep trying");

  // Hardware-backed backstop for a hung loop (stuck I2C/network/USB call): the
  // software watchdogs above run inside loop() and can't see that. Armed last,
  // since setup() legitimately blocks for tens of seconds. The longest normal
  // stall in loop() is a few seconds of HTTPS to ntfy, so 30 s is generous.
  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms = 30000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  if (esp_task_wdt_reconfigure(&wdtCfg) != ESP_OK) esp_task_wdt_init(&wdtCfg);
  esp_task_wdt_add(NULL);

  lastTgOkMs = millis();
  logf("Monitoring the sump...");
  nextSampleUs = micros();
}

void loop(void) {
  esp_task_wdt_reset();
  // Sample at a fixed rate.
  uint32_t nowUs = micros();
  if ((int32_t)(nowUs - nextSampleUs) < 0) return;
  nextSampleUs += SAMPLE_PERIOD_US;
  if ((int32_t)(nowUs - nextSampleUs) > 0) nextSampleUs = nowUs + SAMPLE_PERIOD_US; // fell behind

  float x = bx, y = by, z = bz;  // no signal while the accelerometer is missing
  if (accelOk) {
    if (readSample(x, y, z)) {
      accelReadFails = 0;
    } else {
      // A failed read counts as no signal, never as vibration. A sustained run
      // of them means the sensor or the bus is gone.
      x = bx; y = by; z = bz;
      if (++accelReadFails >= SAMPLE_FAIL_LIMIT) {
        accelReadFails = 0;
        accelLost("stopped responding on I2C (reads failing)");
      }
    }
  }

  float dx = x - bx, dy = y - by, dz = z - bz;
  sumSq += dx * dx + dy * dy + dz * dz;
  bx += BASELINE_ALPHA * dx;
  by += BASELINE_ALPHA * dy;
  bz += BASELINE_ALPHA * dz;

  if (++sampleCount < SAMPLES_PER_WINDOW) return;

  float rms = sqrt(sumSq / sampleCount);
  sumSq = 0;
  sampleCount = 0;

  // A working accelerometer always has some noise; an exactly constant signal
  // means it has stopped measuring (e.g. it lost its measure-mode setting).
  if (accelOk && rms < 0.0005f) {
    if (++flatWindows >= FLAT_WINDOW_LIMIT) {
      flatWindows = 0;
      accelLost("reads a constant (not measuring)");
    }
  } else {
    flatWindows = 0;
  }

  // Once per 250 ms window.
  uint32_t now = millis();
  processWindow(rms, now);
  updateStatusLed();
  serviceLevel(now);
  serviceAht(now);
  checkBootEpoch();
  checkPeriodicReading(now);
  serviceNetwork(now);
  serviceOta();
  serviceWeb();
  serviceMqtt(now);
  checkWiFiWatchdog();
  checkSensorWatchdog(now);
  checkLevelWatchdogs(now);
  checkAhtWatchdog(now);
  checkTelegrafWatchdog(now);
  checkHighWaterRepeat(now);
  checkBootStateCorrection(now);
  checkOtaTrial(now);
  checkDailyReboot(now);

  if (now - lastStatusMs >= 1000) {
    lastStatusMs = now;
    char level[24], air[24];
    if (levelFresh(now)) snprintf(level, sizeof(level), "%s%.1fcm", levelNearLimit ? ">=" : "", levelCm);
    else snprintf(level, sizeof(level), "--");
    if (ahtFresh(now)) snprintf(air, sizeof(air), "%.1fC/%.0f%%", tempC, humidity);
    else snprintf(air, sizeof(air), "--");
    logKeep = false;  // once a second: serial only, it would push everything else out of the web log
    logf("rms=%.3f max=%.3f loud=%d state=%s level=%s air=%s wifi=%s tgq=%d", lastRms,
         maxRmsSinceStatus, loudWindowsSinceStatus, !accelOk ? "no-accel" : deviceRunning ? "RUNNING" : "idle", level, air,
         WiFi.status() == WL_CONNECTED ? "up" : "down", tgCount);
    logKeep = true;
    statusMaxRms = maxRmsSinceStatus;
    maxRmsSinceStatus = 0;
    loudWindowsSinceStatus = 0;
  }
}
