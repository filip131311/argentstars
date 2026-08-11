// argentstars — shows the GitHub star count of a repository on an M5Paper
// e-ink display, refreshing once an hour via RTC deep sleep.

#include <M5Unified.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>

#include "config.h"

static Preferences prefs;

struct FetchResult {
  bool ok = false;
  long stars = -1;
  String fetchedAt;  // local time string, already offset by TZ_OFFSET_S
};

static bool connectWifi() {
  Serial.printf("Connecting to %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; ++i) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed");
    return false;
  }
  Serial.printf("Connected, IP %s\n", WiFi.localIP().toString().c_str());
  return true;
}

// "Mon, 11 Aug 2026 12:34:56 GMT" -> "2026-08-11 14:34"
static String formatDateHeader(const String& httpDate) {
  struct tm tm = {};
  if (strptime(httpDate.c_str(), "%a, %d %b %Y %H:%M:%S", &tm) == nullptr) {
    return "";
  }
  time_t t = mktime(&tm) + TZ_OFFSET_S;
  struct tm local;
  gmtime_r(&t, &local);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
  return String(buf);
}

static FetchResult fetchStars() {
  FetchResult res;

  WiFiClientSecure client;
  client.setInsecure();  // desk gadget: skip CA validation

  HTTPClient http;
  http.setUserAgent("argentstars-m5paper");
  http.setTimeout(15000);
  const char* headerKeys[] = {"date"};
  http.collectHeaders(headerKeys, 1);

  String url = String("https://api.github.com/repos/") + GITHUB_REPO;
  if (!http.begin(client, url)) {
    Serial.println("http.begin failed");
    return res;
  }

  int code = http.GET();
  Serial.printf("GET %s -> %d\n", url.c_str(), code);
  if (code != HTTP_CODE_OK) {
    http.end();
    return res;
  }

  JsonDocument filter;
  filter["stargazers_count"] = true;
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  if (err || doc["stargazers_count"].isNull()) {
    Serial.printf("JSON parse failed: %s\n", err.c_str());
    http.end();
    return res;
  }

  res.stars = doc["stargazers_count"].as<long>();
  res.fetchedAt = formatDateHeader(http.header("date"));
  res.ok = true;
  http.end();
  Serial.printf("Stars: %ld (at %s)\n", res.stars, res.fetchedAt.c_str());
  return res;
}

// 1,957 style thousands separator
static String formatCount(long n) {
  String digits(n);
  String out;
  int len = digits.length();
  for (int i = 0; i < len; ++i) {
    if (i > 0 && (len - i) % 3 == 0) out += ',';
    out += digits[i];
  }
  return out;
}

static void drawStar(M5GFX& d, int cx, int cy, int rOuter, int color) {
  const int points = 5;
  const float rInner = rOuter * 0.42f;
  float vx[points * 2], vy[points * 2];
  for (int i = 0; i < points * 2; ++i) {
    float r = (i % 2 == 0) ? rOuter : rInner;
    float a = -M_PI / 2 + i * M_PI / points;
    vx[i] = cx + r * cosf(a);
    vy[i] = cy + r * sinf(a);
  }
  for (int i = 0; i < points * 2; ++i) {
    int j = (i + 1) % (points * 2);
    d.fillTriangle(cx, cy, vx[i], vy[i], vx[j], vy[j], color);
  }
}

static void drawScreen(long stars, const String& updatedAt, bool stale) {
  M5GFX& d = M5.Display;
  d.setEpdMode(epd_mode_t::epd_quality);  // full refresh, no ghosting
  d.startWrite();
  d.fillScreen(TFT_WHITE);
  d.setTextColor(TFT_BLACK, TFT_WHITE);

  const int w = d.width();

  d.setTextDatum(top_center);
  d.setFont(&fonts::FreeSans12pt7b);
  d.setTextSize(1);
  d.drawString("GitHub Stars", w / 2, 48);

  d.setFont(&fonts::FreeSansBold18pt7b);
  d.drawString(GITHUB_REPO, w / 2, 92);

  // Star icon + count, centered together
  String txt = stars >= 0 ? formatCount(stars) : String("--");
  d.setFont(&fonts::FreeSansBold24pt7b);
  d.setTextSize(4);
  const int starR = 52;
  const int gap = 40;
  int txtW = d.textWidth(txt);
  int total = 2 * starR + gap + txtW;
  int x = (w - total) / 2;
  const int cy = 300;
  drawStar(d, x + starR, cy, starR, TFT_BLACK);
  d.setTextDatum(middle_left);
  d.drawString(txt, x + 2 * starR + gap, cy);

  d.setTextDatum(bottom_center);
  d.setFont(&fonts::FreeSans12pt7b);
  d.setTextSize(1);
  String status;
  if (updatedAt.length()) status = "Updated " + updatedAt;
  if (stale) status += status.length() ? "  (offline, showing last value)"
                                       : "Offline, showing last value";
  int batt = M5.Power.getBatteryLevel();
  if (batt >= 0) status += "   |   Battery " + String(batt) + "%";
  d.drawString(status, w / 2, d.height() - 32);

  d.endWrite();
  d.waitDisplay();
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);

  // Panel is portrait-native; we want landscape
  if (M5.Display.width() < M5.Display.height()) {
    M5.Display.setRotation(M5.Display.getRotation() ^ 1);
  }

  prefs.begin("argentstars");

  FetchResult res;
  if (connectWifi()) {
    res = fetchStars();
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  if (res.ok) {
    bool changed = res.stars != prefs.getLong("count", -1);
    prefs.putLong("count", res.stars);
    prefs.putString("at", res.fetchedAt);
    // Redraw even when unchanged: refresh timestamp + battery, clears ghosting
    (void)changed;
    drawScreen(res.stars, res.fetchedAt, false);
  } else {
    // Keep the last known value on screen, marked as stale
    drawScreen(prefs.getLong("count", -1), prefs.getString("at", ""), true);
  }
  prefs.end();

  int sleepFor = res.ok ? REFRESH_INTERVAL_S : RETRY_INTERVAL_S;
  Serial.printf("Sleeping for %d s\n", sleepFor);
  Serial.flush();
  // RTC-timed sleep: powers down on battery, deep-sleeps on USB
  M5.Power.timerSleep(sleepFor);
}

void loop() {
  // Never reached: timerSleep() resets on wake and setup() runs again
}
