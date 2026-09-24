#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <time.h>
#include <sys/time.h>
#include <cstdarg>
#include <math.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>
#include <esp32c3/rom/rtc.h>
#include <Adafruit_ADXL345_U.h>
#include "secrets.h"
#include "aht20.h"
#include "jsn_sr04t.h"

// Bump on each flash you want to identify later -- format: YYYY-MM-DDrN.
#define FIRMWARE_VERSION "2026-09-24r2"

// ---- Configuration (build flags, see platformio.ini) ------------------------
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
#define ON_RMS_THRESHOLD 0.40f
#endif
#ifndef OFF_RMS_THRESHOLD
#define OFF_RMS_THRESHOLD 0.20f
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

// Network name for OTA uploads (sm2k.local) and DHCP.
#define OTA_HOSTNAME "sm2k"

// NVS (flash) namespace for persisted state.
#define NVS_NS "sm2k"

const char *BANNER =
R"( ___                   __  __         _             ___ __   __   __
/ __|_  _ _ __  _ __  |  \/  |__ _ __| |_ ___ _ _  |_  )  \ /  \ /  \
\__ \ || | '  \| '_ \ | |\/| / _` (_-<  _/ -_) '_|  / / () | () | () |
|___/\_,_|_|_|_| .__/ |_|  |_\__,_/__/\__\___|_|   /___\__/ \__/ \__/
               |_|)";

// ---- Pins -----------------------------------------------------------------
const int SDA_PIN = 5;
const int SCL_PIN = 6;   // GPIO 8/9 are LED/boot pins on this board
// JSN-SR04T: Trig direct, Echo through a 5 V -> 3.3 V divider. GPIO 2, 8 and 9
// are boot strapping pins on the C3, so they're avoided.
const int LEVEL_TRIG_PIN = 3;
const int LEVEL_ECHO_PIN = 4;

const int  LED_PIN        = 8;
const bool LED_ACTIVE_LOW = true;  // set false if the LED lights when the pin is HIGH
const uint32_t LED_BLINK_HALF_PERIOD_MS = 500;  // blink rate while the pump is running

// ---- Vibration detection --------------------------------------------------
// Samples are high-passed against a slow per-axis baseline (removes gravity and
// mounting angle), then RMS'd over a short window. Tune the thresholds from the
// "rms=" values printed over serial while the pump is idle and running.
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
const uint32_t AHT_STALE_MS       = 60000;
const uint32_t AHT_ALERT_MS       = 300000;  // push an alert after this long without a reading

// ---- Reporting --------------------------------------------------------------
// Readings go to Telegraf every 10 minutes, on the wall-clock 10 minutes.
const uint32_t READING_PERIOD_S   = 600;
// Points waiting for Telegraf, oldest first. 150 lines covers a day of
// 10-minute readings plus pump events; if it fills, the oldest are dropped.
const int      TG_QUEUE_SIZE      = 150;
const int      TG_LINE_LEN        = 224;
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

bool ledOn = false;            // instantaneous vibration indicator
bool deviceRunning = false;
uint32_t aboveSince = 0;       // 0 = not currently above ON threshold
uint32_t belowSince = 0;       // 0 = not currently below OFF threshold
uint32_t runStartMs = 0;
uint32_t lastStatusMs = 0;
float lastRms = 0;
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
bool     accelOk = false;      // accelerometer detected and answering
uint32_t accelOkSinceMs = 0;   // when it (re)started answering
uint32_t lastAccelRetryMs = 0;
bool     accelAlerted = false;
uint32_t lastSensorCheck = 0;
bool     bootCorrectionDone = false;
bool     otaTrial = false;     // running a fresh OTA image that hasn't passed its health check
uint8_t  otaRequiredHealth = HEALTH_ACCEL;
volatile bool otaDone = false;

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
  Serial.printf("[%s] %s\r\n", logTimestamp(), msg);
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
#ifdef NTFY_TOPIC
  if (WiFi.status() != WL_CONNECTED) return -1;
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(5);  // seconds
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(client, String("https://ntfy.sh/") + NTFY_TOPIC)) return -1;
  http.addHeader("Title", m.title);
  http.addHeader("Priority", String(m.priority));
  if (m.tags && m.tags[0]) http.addHeader("Tags", m.tags);
  int code = http.POST(String(m.body));
  http.end();
  logf("[ntfy] \"%s\" -> %d", m.title, code);
  return code;
#else
  return -1;
#endif
}

// Queues a push. The body is printf-formatted. Without NTFY_TOPIC it's only logged.
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
#ifdef NTFY_TOPIC
  if (ntfyCount == NTFY_QUEUE_SIZE) {
    logf("[ntfy] queue full, dropping oldest message");
    ntfyHead = (ntfyHead + 1) % NTFY_QUEUE_SIZE;
    ntfyCount--;
  }
  ntfyQueue[(ntfyHead + ntfyCount) % NTFY_QUEUE_SIZE] = m;
  ntfyCount++;
#endif
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
#ifdef TELEGRAF_URL
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
  if (http.begin(client, TELEGRAF_URL)) {
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
    logf("[tg] send failed (%d), %d point(s) queued", code, tgCount);
    lastTgFailMs = now ? now : 1;
  }
  return sent;
#else
  tgCount = 0;  // nowhere to send; the points were logged when queued
  return false;
#endif
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
void setLed(bool on) {
  digitalWrite(LED_PIN, (on != LED_ACTIVE_LOW) ? HIGH : LOW);
}

void readSample(float &x, float &y, float &z) {
  sensors_event_t event;
  accel.getEvent(&event);
  x = event.acceleration.x;
  y = event.acceleration.y;
  z = event.acceleration.z;
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
  readSample(bx, by, bz);
  accelOk = true;
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
  ledOn = false;
  setLed(false);
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
  i2cBusRecover();
  Wire.begin(SDA_PIN, SCL_PIN, 400000);
  if (startAccel()) return;
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
    Wire.begin(SDA_PIN, SCL_PIN, 400000);
    if (startAccel() && accelAlerted) {
      accelAlerted = false;
      notify(PRIO_INFO, "white_check_mark", DEVICE_NAME ": accelerometer OK",
             "Accelerometer answering again; pump on/off detection resumed.");
    }
    return;
  }

  if (now - lastSensorCheck < SENSOR_CHECK_INTERVAL_MS) return;
  lastSensorCheck = now;
  if (isAccelId(accel.getDeviceID())) {
    sensorFails = 0;
    return;
  }
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
  bool ok = drop >= MIN_DROP_CM;
  logf("Pump cycle: on at %.1f cm, off at %.1f cm, dropped %.1f cm in %.1f s%s",
       before, after, drop, dropRunSeconds, ok ? "" : " -- LESS THAN EXPECTED");
  addField(fields, sizeof(fields), "drop_cm=%.1f", drop);
  addField(fields, sizeof(fields), "result=\"%s\"", ok ? "ok" : "no_drop");
  tgPoint("sump_cycle", fields);
  if (!ok) {
    notify(PRIO_ALERT, "warning", DEVICE_NAME ": pump ran but level didn't drop",
           "Ran %.1f s; water %.1f -> %.1f cm (dropped %.1f cm, expected at least %.1f). "
           "Check the discharge line, check valve and pump.",
           dropRunSeconds, before, after, drop, MIN_DROP_CM);
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
    notify(PRIO_PUMP, "", DEVICE_NAME " stopped", "Stopped at %s (ran %.1f s)", at, seconds);
    dropCheckPending = true;
    dropStopMs = eventMs;
    dropRunSeconds = seconds;
  }
  tgPoint("sump_event", fields, ageMs);
  lastRecordedState = running ? 1 : 0;
  saveLastRecordedState(lastRecordedState);
}

// Called once per window with that window's RMS.
void processWindow(float rms, uint32_t now) {
  lastRms = rms;
  if (!accelOk) return;  // no pump detection without the accelerometer

  // Instantaneous vibration indicator (with hysteresis).
  if (rms > ON_RMS_THRESHOLD) ledOn = true;
  else if (rms < OFF_RMS_THRESHOLD) ledOn = false;

  // Debounced running/stopped state.
  if (rms > ON_RMS_THRESHOLD) {
    if (!aboveSince) aboveSince = now;
    belowSince = 0;
    if (!deviceRunning && now - aboveSince >= RUN_CONFIRM_MS) {
      setRunState(true, aboveSince);
    }
  } else if (rms < OFF_RMS_THRESHOLD) {
    if (!belowSince) belowSince = now;
    aboveSince = 0;
    if (deviceRunning && now - belowSince >= STOP_CONFIRM_MS) {
      setRunState(false, belowSince);
    }
  } else {
    // Hysteresis band: neither counts toward starting a run (footsteps are
    // impulsive, a motor is continuous) nor toward ending one.
    aboveSince = 0;
    belowSince = 0;
  }

  // LED: blinks while the pump is considered running; otherwise solid while any
  // vibration is detected, and fully off when quiet.
  if (deviceRunning) setLed((now / LED_BLINK_HALF_PERIOD_MS) % 2 == 0);
  else setLed(ledOn);
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
  bool high = levelNearLimit || levelCm >= HIGH_WATER_CM;
  if (!highWater && high) {
    highWater = true;
    lastHighAlertMs = now;
    logf("HIGH WATER: %s", levelText().c_str());
    char fields[96] = "";
    addField(fields, sizeof(fields), "type=\"high_water\",active=1i,level_cm=%.1f", levelCm);
    tgPoint("sump_alarm", fields);
    notify(PRIO_URGENT, "rotating_light", DEVICE_NAME ": HIGH WATER",
           "Water %s (alarm at %.1f cm). Pump %s.", levelText().c_str(), HIGH_WATER_CM,
           deviceRunning ? "running" : "not running");
  } else if (highWater && !levelNearLimit && levelCm < HIGH_WATER_CM - HIGH_WATER_HYST_CM) {
    highWater = false;
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
           "Water %s (alarm at %.1f cm). Pump %s.", levelText().c_str(), HIGH_WATER_CM,
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
    logf("[level] no reliable echo (%d echoes, %d too near, of %d pings)", burstEchoes, burstNear, LEVEL_PINGS);
    return;
  }
  lastEchoMs = now;
  if (levelAlerted) {
    levelAlerted = false;
    notify(PRIO_INFO, "white_check_mark", DEVICE_NAME ": level sensor OK", "Level readings are back.");
  }

  float depth = SENSOR_TO_FLOOR_CM - dist;
  if (haveLevel && fabsf(depth - levelCm) > LEVEL_MAX_STEP_CM) {
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
    if (now - lastAhtStartMs < AHT_INTERVAL_MS) return;
    lastAhtStartMs = now;
    if (!ahtReady) ahtReady = aht.begin();
    if (ahtReady && aht.start()) ahtMeasuring = true;
    else ahtReady = false;
    return;
  }

  float t, h;
  Aht20::Result r = aht.read(t, h);
  if (r == Aht20::BUSY && now - lastAhtStartMs < 1000) return;
  ahtMeasuring = false;
  if (r != Aht20::OK) {
    ahtReady = false;  // re-initialize before the next attempt
    logf("[AHT20] read failed (%s)", r == Aht20::BUSY ? "busy" : "no answer or bad CRC");
    return;
  }
  tempC = t;
  humidity = h;
  ahtAtMs = now;
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
  tgPoint("sump", fields);
  periodCycles = 0;
  periodRunMs = 0;
}

void sendDailyReport(uint32_t now) {
#ifndef NTFY_TOPIC
  return;
#endif
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
  WiFi.begin(WIFI_SSID, WIFI_PASS);
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
void setup(void) {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never block on a stalled USB host; excess output is dropped
  delay(2000);               // give USB-CDC time to attach so early messages aren't lost
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

  checkUnexpectedReset();
  checkOtaState();

  pinMode(LED_PIN, OUTPUT);
  setLed(false);
  sonar.begin();

  setupWiFi();
  setupTime();
  checkBootEpoch();
  setupOta();
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
  if (accelOk) readSample(x, y, z);

  float dx = x - bx, dy = y - by, dz = z - bz;
  sumSq += dx * dx + dy * dy + dz * dz;
  bx += BASELINE_ALPHA * dx;
  by += BASELINE_ALPHA * dy;
  bz += BASELINE_ALPHA * dz;

  if (++sampleCount < SAMPLES_PER_WINDOW) return;

  float rms = sqrt(sumSq / sampleCount);
  sumSq = 0;
  sampleCount = 0;

  // Once per 250 ms window.
  uint32_t now = millis();
  processWindow(rms, now);
  serviceLevel(now);
  serviceAht(now);
  checkBootEpoch();
  checkPeriodicReading(now);
  serviceNetwork(now);
  serviceOta();
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
    logf("rms=%.3f state=%s level=%s air=%s wifi=%s tgq=%d", lastRms,
         !accelOk ? "no-accel" : deviceRunning ? "RUNNING" : "idle", level, air,
         WiFi.status() == WL_CONNECTED ? "up" : "down", tgCount);
  }
}
