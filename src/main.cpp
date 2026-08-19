// argentstars — shows the GitHub star count of a repository on an M5Paper
// e-ink display, refreshing once an hour via RTC deep sleep.
//
// The GitHub API allows only 60 unauthenticated requests/hour PER PUBLIC IP,
// shared by everyone behind the same NAT (an office easily exhausts it). So:
//   1. API requests are conditional (ETag): 304 replies don't count at all.
//   2. If the API is rate-limited anyway, the star count is scraped from the
//      repo's public HTML page, which is not subject to the API quota.
//   3. On failure the retry is aligned to the advertised quota-reset time.

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
  String fetchedAt;   // local time string, already offset by TZ_OFFSET_S
  int retryAfterS = 0;  // suggested wait when !ok (0 = use default)
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

// "Mon, 11 Aug 2026 12:34:56 GMT" -> unix epoch (0 on parse failure)
static time_t parseDateHeader(const String& httpDate) {
  struct tm tm = {};
  if (strptime(httpDate.c_str(), "%a, %d %b %Y %H:%M:%S", &tm) == nullptr) {
    return 0;
  }
  return mktime(&tm);  // tm is UTC and so is the device's notion of time
}

static String formatLocal(time_t utc) {
  if (utc == 0) return "";
  time_t t = utc + TZ_OFFSET_S;
  struct tm local;
  gmtime_r(&t, &local);
  char buf[24];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &local);
  return String(buf);
}

// Primary source: GitHub REST API with a conditional (ETag) request.
static FetchResult fetchFromApi() {
  FetchResult res;

  WiFiClientSecure client;
  client.setInsecure();  // desk gadget: skip CA validation

  HTTPClient http;
  http.setUserAgent("argentstars-m5paper");
  http.setTimeout(15000);
  const char* headerKeys[] = {"date", "etag", "x-ratelimit-remaining",
                              "x-ratelimit-reset"};
  http.collectHeaders(headerKeys, 4);

  String url = String("https://api.github.com/repos/") + GITHUB_REPO;
  if (!http.begin(client, url)) {
    Serial.println("api: http.begin failed");
    return res;
  }
#ifdef GITHUB_TOKEN
  if (strlen(GITHUB_TOKEN) > 0) {
    http.addHeader("Authorization", String("Bearer ") + GITHUB_TOKEN);
  }
#endif
  String etag = prefs.isKey("etag") ? prefs.getString("etag", "") : String();
  long cached = prefs.getLong("count", -1);
  if (etag.length() && cached >= 0) {
    http.addHeader("If-None-Match", etag);
  }

  int code = http.GET();
  time_t now = parseDateHeader(http.header("date"));
  Serial.printf("api: GET -> %d (ratelimit remaining=%s reset=%s)\n", code,
                http.header("x-ratelimit-remaining").c_str(),
                http.header("x-ratelimit-reset").c_str());

  if (code == HTTP_CODE_OK) {
    JsonDocument filter;
    filter["stargazers_count"] = true;
    JsonDocument doc;
    DeserializationError err = deserializeJson(
        doc, http.getStream(), DeserializationOption::Filter(filter));
    if (!err && !doc["stargazers_count"].isNull()) {
      res.stars = doc["stargazers_count"].as<long>();
      res.fetchedAt = formatLocal(now);
      res.ok = true;
      if (http.header("etag").length()) {
        prefs.putString("etag", http.header("etag"));
      }
    } else {
      Serial.printf("api: JSON parse failed: %s\n", err.c_str());
    }
  } else if (code == HTTP_CODE_NOT_MODIFIED) {
    // Unchanged since last time; 304 does not count against the quota.
    res.stars = cached;
    res.fetchedAt = formatLocal(now);
    res.ok = true;
    Serial.println("api: 304 not modified, reusing cached count");
  } else if (code == 403 || code == 429) {
    long reset = http.header("x-ratelimit-reset").toInt();
    if (reset > 0 && now > 0 && reset > now) {
      res.retryAfterS = (int)(reset - now) + 60;
      Serial.printf("api: rate limited, resets in %d s\n", res.retryAfterS);
    }
  }
  http.end();
  return res;
}

// Fallback source: scrape the repo's HTML page. Exact count appears as
// `aria-label="1958 users starred this repository"`.
static FetchResult fetchFromHtml() {
  FetchResult res;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  http.setUserAgent("Mozilla/5.0 (compatible; argentstars-m5paper)");
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const char* headerKeys[] = {"date"};
  http.collectHeaders(headerKeys, 1);

  String url = String("https://github.com/") + GITHUB_REPO;
  if (!http.begin(client, url)) {
    Serial.println("html: http.begin failed");
    return res;
  }
  int code = http.GET();
  Serial.printf("html: GET -> %d\n", code);
  if (code != HTTP_CODE_OK) {
    http.end();
    return res;
  }
  time_t now = parseDateHeader(http.header("date"));

  static const char MARKER[] = " users starred this repository";
  const size_t KEEP = sizeof(MARKER) + 16;  // window overlap: marker + digits
  static char buf[2048 + 64];
  size_t have = 0;
  long found = -1;

  WiFiClient* stream = http.getStreamPtr();
  uint32_t deadline = millis() + 25000;
  while (http.connected() && millis() < deadline && found < 0) {
    size_t avail = stream->available();
    if (!avail) {
      delay(10);
      continue;
    }
    size_t space = 2048 - have;
    int n = stream->readBytes(buf + have, avail < space ? avail : space);
    if (n <= 0) break;
    have += n;
    buf[have] = '\0';

    char* hit = strstr(buf, MARKER);
    if (hit && hit > buf) {
      // walk back over the digits preceding the marker
      char* p = hit;
      while (p > buf && isdigit((unsigned char)p[-1])) --p;
      if (p < hit) found = atol(p);
    }
    if (found < 0 && have > KEEP) {
      memmove(buf, buf + have - KEEP, KEEP);
      have = KEEP;
    }
  }
  http.end();

  if (found >= 0) {
    res.stars = found;
    res.fetchedAt = formatLocal(now);
    res.ok = true;
    Serial.printf("html: scraped %ld stars\n", found);
  } else {
    Serial.println("html: marker not found in page");
  }
  return res;
}

static FetchResult fetchStars() {
  FetchResult api = fetchFromApi();
  if (api.ok) return api;
  Serial.println("api failed, trying html fallback");
  FetchResult html = fetchFromHtml();
  if (html.ok) return html;
  if (api.retryAfterS > 0) html.retryAfterS = api.retryAfterS;
  return html;
}

// Averaged reading, taken at boot BEFORE Wi-Fi/EPD load makes the battery
// voltage sag (a single sample under load once read 49% on a full battery).
static int readBatteryPercent() {
  M5.Power.getBatteryLevel();  // discard first sample (ADC warm-up)
  delay(10);
  long sum = 0;
  const int N = 8;
  for (int i = 0; i < N; ++i) {
    sum += M5.Power.getBatteryLevel();
    delay(10);
  }
  return (int)(sum / N);
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

static void drawStar(LovyanGFX& d, int cx, int cy, int rOuter, int color) {
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

// Composes the whole frame on `g`, which is normally an off-screen canvas so
// the panel never shows a partially drawn frame.
static void renderFrame(LovyanGFX& g, long stars, const String& updatedAt,
                        bool stale, int batteryPct) {
  g.fillScreen(TFT_WHITE);
  g.setTextColor(TFT_BLACK, TFT_WHITE);

  const int w = g.width();

  g.setTextDatum(top_center);
  g.setFont(&fonts::FreeSans12pt7b);
  g.setTextSize(1);
  g.drawString("GitHub Stars", w / 2, 48);

  g.setFont(&fonts::FreeSansBold18pt7b);
  g.drawString(GITHUB_REPO, w / 2, 92);

  // Star icon + count, centered together
  String txt = stars >= 0 ? formatCount(stars) : String("--");
  g.setFont(&fonts::FreeSansBold24pt7b);
  g.setTextSize(4);
  const int starR = 52;
  const int gap = 40;
  int txtW = g.textWidth(txt);
  int total = 2 * starR + gap + txtW;
  int x = (w - total) / 2;
  const int cy = 300;
  drawStar(g, x + starR, cy, starR, TFT_BLACK);
  g.setTextDatum(middle_left);
  g.drawString(txt, x + 2 * starR + gap, cy);

  g.setTextDatum(bottom_center);
  g.setFont(&fonts::FreeSans12pt7b);
  g.setTextSize(1);
  String status;
  if (updatedAt.length()) status = "Updated " + updatedAt;
  if (stale) status += status.length() ? "  (offline, showing last value)"
                                       : "Offline, showing last value";
  if (batteryPct >= 0) status += "   |   Battery " + String(batteryPct) + "%";
  g.drawString(status, w / 2, g.height() - 32);
}

static void drawScreen(long stars, const String& updatedAt, bool stale,
                       int batteryPct) {
  M5GFX& d = M5.Display;
  d.setEpdMode(epd_mode_t::epd_quality);  // full refresh, no ghosting

  // Render everything into a PSRAM canvas first, then push the finished frame
  // in one go: the EPD does a single refresh instead of updating while text
  // and the star are still being composed.
  M5Canvas canvas(&d);
  canvas.setColorDepth(4);  // 16-level grayscale, same as the panel
  canvas.setPsram(true);
  if (canvas.createSprite(d.width(), d.height())) {
    renderFrame(canvas, stars, updatedAt, stale, batteryPct);
    d.startWrite();
    canvas.pushSprite(0, 0);
    d.endWrite();
    canvas.deleteSprite();
  } else {
    // No memory for a full frame: draw straight to the display
    d.startWrite();
    renderFrame(d, stars, updatedAt, stale, batteryPct);
    d.endWrite();
  }
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

  int battery = readBatteryPercent();
  Serial.printf("Battery: %d%%\n", battery);

  prefs.begin("argentstars");

  FetchResult res;
  if (connectWifi()) {
    res = fetchStars();
  }
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  if (res.ok) {
    prefs.putLong("count", res.stars);
    prefs.putString("at", res.fetchedAt);
    drawScreen(res.stars, res.fetchedAt, false, battery);
  } else {
    // Keep the last known value on screen, marked as stale
    drawScreen(prefs.getLong("count", -1), prefs.getString("at", ""), true,
               battery);
  }
  prefs.end();

  int sleepFor;
  if (res.ok) {
    sleepFor = REFRESH_INTERVAL_S;
  } else {
    sleepFor = res.retryAfterS > 0 ? res.retryAfterS : RETRY_INTERVAL_S;
    if (sleepFor > REFRESH_INTERVAL_S) sleepFor = REFRESH_INTERVAL_S;
    if (sleepFor < 120) sleepFor = 120;
  }
  Serial.printf("Sleeping for %d s\n", sleepFor);
  Serial.flush();
  // RTC-timed sleep: powers down on battery, deep-sleeps on USB
  M5.Power.timerSleep(sleepFor);
}

void loop() {
  // Never reached: timerSleep() resets on wake and setup() runs again
}
