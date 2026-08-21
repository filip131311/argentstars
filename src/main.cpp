// argentstars — M5Paper e-ink dashboard with several screens (side wheel switches them):
//   0. GitHub star count of a repository (refreshed hourly)
//   1. Countdown of days until a target date (refreshed at local midnight)
//
// Between refreshes the device light-sleeps (the program stays resident; a
// deep-sleep reboot on this hardware browns out intermittently during the
// e-paper re-init and loses the wake-up cause). It wakes either from the
// timer (scheduled refresh of the screen currently shown) or from the side
// switch (GPIO37 = up, GPIO39 = down, GPIO38 = push). Up/down move to the
// previous/next screen (cyclic); the new screen then keeps its own refresh
// cadence until the next press. Push refreshes the current screen now.
//
// GitHub notes: the API allows only 60 unauthenticated requests/hour PER
// PUBLIC IP, shared by everyone behind the same NAT (an office easily
// exhausts it). So:
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
#include <esp_sleep.h>
#include <esp_system.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <time.h>

#include "config.h"
#include "timeutil.h"

static Preferences prefs;

// ---------------------------------------------------------------------------
// Clock: the BM8563 RTC keeps UTC across deep sleep / power loss (it has its
// own backup). It is set from the HTTP Date header of every successful fetch.

static time_t rtcNow() {
  tm t = M5.Rtc.getDateTime().get_tm();
  if (t.tm_year + 1900 < 2025) return 0;  // never set
  return mktime(&t);  // TZ is unset on the device, so mktime() == timegm()
}

static void rtcSet(time_t utc) {
  if (utc <= 0) return;
  tm t;
  gmtime_r(&utc, &t);
  M5.Rtc.setDateTime(&t);
}

// "Mon, 11 Aug 2026 12:34:56 GMT" -> unix epoch (0 on parse failure)
static time_t parseDateHeader(const String& httpDate) {
  struct tm tm = {};
  if (strptime(httpDate.c_str(), "%a, %d %b %Y %H:%M:%S", &tm) == nullptr) {
    return 0;
  }
  return mktime(&tm);
}

static String formatLocal(time_t utc, const char* fmt = "%Y-%m-%d %H:%M") {
  if (utc == 0) return "";
  time_t t = utc + TZ_OFFSET_S;
  struct tm local;
  gmtime_r(&t, &local);
  char buf[24];
  strftime(buf, sizeof(buf), fmt, &local);
  return String(buf);
}

// ---------------------------------------------------------------------------
// Network

struct FetchResult {
  bool ok = false;
  long stars = -1;
  time_t fetchedUtc = 0;
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

static void disconnectWifi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
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
  res.fetchedUtc = now;
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
  res.fetchedUtc = parseDateHeader(http.header("date"));
  if (code != HTTP_CODE_OK) {
    http.end();
    return res;
  }

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
    res.ok = true;
    Serial.printf("html: scraped %ld stars\n", found);
  } else {
    Serial.println("html: marker not found in page");
  }
  return res;
}

// Fetches the star count, stores it in prefs and syncs the RTC from the
// server's Date header. Needs Wi-Fi to be connected.
static FetchResult fetchStars() {
  FetchResult api = fetchFromApi();
  FetchResult res = api;
  if (!api.ok) {
    Serial.println("api failed, trying html fallback");
    res = fetchFromHtml();
    if (!res.ok && api.retryAfterS > 0) res.retryAfterS = api.retryAfterS;
    if (res.fetchedUtc == 0) res.fetchedUtc = api.fetchedUtc;
  }
  if (res.fetchedUtc > 0) rtcSet(res.fetchedUtc);
  if (res.ok) {
    prefs.putLong("count", res.stars);
    prefs.putString("at", formatLocal(res.fetchedUtc));
  }
  return res;
}

// ---------------------------------------------------------------------------
// Misc hardware helpers

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

// Side wheel pins (active low)
static const gpio_num_t BTN_PREV = GPIO_NUM_37;   // wheel up
static const gpio_num_t BTN_PUSH = GPIO_NUM_38;   // wheel push
static const gpio_num_t BTN_NEXT = GPIO_NUM_39;   // wheel down
static const gpio_num_t BTN_PINS[] = {BTN_PREV, BTN_PUSH, BTN_NEXT};

enum Button { BTN_NONE, BTN_PREV_PRESSED, BTN_NEXT_PRESSED, BTN_PUSH_PRESSED };

// Which wheel position is being held.
static Button readButton() {
  if (digitalRead(BTN_PREV) == LOW) return BTN_PREV_PRESSED;
  if (digitalRead(BTN_NEXT) == LOW) return BTN_NEXT_PRESSED;
  if (digitalRead(BTN_PUSH) == LOW) return BTN_PUSH_PRESSED;
  return BTN_NONE;
}

// Button that caused the last wake-up, captured right after the sleep
// returns — before the e-paper re-init, which takes long enough for a
// normal press to be released.
static Button wakeButton = BTN_NONE;

// Samples the wheel for a short while; a tilt (prev/next) wins over push,
// which the wheel can briefly brush on its way.
static Button captureButton() {
  Button best = BTN_NONE;
  uint32_t start = millis();
  while (millis() - start < 80) {
    Button b = readButton();
    if (b == BTN_PREV_PRESSED || b == BTN_NEXT_PRESSED) return b;
    if (b != BTN_NONE) best = b;
    delay(5);
  }
  return best;
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

// ---------------------------------------------------------------------------
// Screen abstraction
//
// Each screen owns its data refresh and its refresh cadence. refresh() is
// called only when the screen's deadline has passed (or is unknown); it may
// use the network and returns the number of seconds until the next refresh.
// render() draws the screen from cached data and must not block on network.

struct Context {
  time_t now = 0;    // UTC, 0 if the RTC has never been synced
  int battery = -1;  // percent, -1 unknown
  bool stale = false;  // set when the last refresh failed
};

struct Screen {
  const char* name;
  int (*refresh)(Context& ctx);
  void (*render)(LovyanGFX& g, const Context& ctx);
};

static void drawFooter(LovyanGFX& g, const String& status, int screenIdx,
                       int screenCount) {
  g.setTextDatum(bottom_center);
  g.setFont(&fonts::FreeSans12pt7b);
  g.setTextSize(1);
  g.drawString(status, g.width() / 2, g.height() - 32);

  // Page indicator dots
  const int r = 6, gap = 28;
  int x = g.width() / 2 - (screenCount - 1) * gap / 2;
  int y = g.height() - 14;
  for (int i = 0; i < screenCount; ++i, x += gap) {
    if (i == screenIdx) g.fillCircle(x, y, r, TFT_BLACK);
    else g.drawCircle(x, y, r, TFT_BLACK);
  }
}

static String batteryStatus(const char* label, int pct) {
  return pct >= 0 ? String("   |   ") + label + " " + pct + "%" : String();
}

// --- Screen 0: GitHub stars -------------------------------------------------

static int starsRefresh(Context& ctx) {
  FetchResult res;
  if (connectWifi()) res = fetchStars();
  disconnectWifi();
  ctx.stale = !res.ok;
  if (res.ok) return REFRESH_INTERVAL_S;
  int retry = res.retryAfterS > 0 ? res.retryAfterS : RETRY_INTERVAL_S;
  if (retry > REFRESH_INTERVAL_S) retry = REFRESH_INTERVAL_S;
  if (retry < 120) retry = 120;
  return retry;
}

static void starsRender(LovyanGFX& g, const Context& ctx) {
  long stars = prefs.getLong("count", -1);
  String updatedAt = prefs.getString("at", "");
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

  String status;
  if (updatedAt.length()) status = "Updated " + updatedAt;
  if (ctx.stale) status += status.length() ? "  (offline, showing last value)"
                                           : "Offline, showing last value";
  status += batteryStatus("Battery", ctx.battery);
  drawFooter(g, status, 0, 2);
}

// --- Screen 1: countdown ----------------------------------------------------

static int countdownRefresh(Context& ctx) {
  if (ctx.now == 0) {
    // No valid clock yet: sync it from the server's Date header (this also
    // refreshes the cached star count as a side effect).
    Serial.println("countdown: RTC not set, syncing over Wi-Fi");
    if (connectWifi()) fetchStars();
    disconnectWifi();
    ctx.now = rtcNow();
  }
  ctx.stale = (ctx.now == 0);
  if (ctx.now == 0) return RETRY_INTERVAL_S;
  // Wake a few seconds after local midnight so the day has surely rolled over
  return (int)secondsToLocalMidnight(ctx.now, TZ_OFFSET_S) + 5;
}

static void countdownRender(LovyanGFX& g, const Context& ctx) {
  const int w = g.width();

  // efont has the Latin Extended-A glyphs (ś) that the FreeSans fonts lack
  g.setTextDatum(top_center);
  g.setFont(&fonts::efontJA_24_b);
  g.setTextSize(2);
  g.drawString(COUNTDOWN_LABEL, w / 2, 60);

  String big;
  if (ctx.now == 0) {
    big = "--";
  } else {
    long days = daysUntil(ctx.now, TZ_OFFSET_S, COUNTDOWN_YEAR, COUNTDOWN_MONTH,
                          COUNTDOWN_DAY);
    if (days < 0) days = 0;
    big = String(days);
  }
  g.setFont(&fonts::FreeSansBold24pt7b);
  g.setTextSize(5);
  g.setTextDatum(middle_center);
  g.drawString(big, w / 2, 310);

  String status;
  if (ctx.now) status = "Dzisiaj " + formatLocal(ctx.now, "%Y-%m-%d");
  else status = "Brak synchronizacji czasu";
  status += batteryStatus("Bateria", ctx.battery);
  drawFooter(g, status, 1, 2);
}

static const Screen SCREENS[] = {
    {"stars", starsRefresh, starsRender},
    {"countdown", countdownRefresh, countdownRender},
};
static const int SCREEN_COUNT = sizeof(SCREENS) / sizeof(SCREENS[0]);

// Composes the whole frame off-screen, then pushes it in a single EPD refresh
// so the panel never shows a partially drawn frame.
static void drawScreen(const Screen& s, const Context& ctx) {
  M5GFX& d = M5.Display;
  d.setEpdMode(epd_mode_t::epd_quality);  // full refresh, no ghosting

  auto paint = [&](LovyanGFX& g) {
    g.fillScreen(TFT_WHITE);
    g.setTextColor(TFT_BLACK, TFT_WHITE);
    s.render(g, ctx);
  };

  M5Canvas canvas(&d);
  canvas.setColorDepth(4);  // 16-level grayscale, same as the panel
  canvas.setPsram(true);
  if (canvas.createSprite(d.width(), d.height())) {
    paint(canvas);
    d.startWrite();
    canvas.pushSprite(0, 0);
    d.endWrite();
    canvas.deleteSprite();
  } else {
    d.startWrite();
    paint(d);
    d.endWrite();
  }
  d.waitDisplay();
}

// ---------------------------------------------------------------------------

static String deadlineKey(int idx) { return String("next") + idx; }

// Light-sleeps until the timer fires or the side switch is pushed.
// Returns the wake-up cause.
static esp_sleep_wakeup_cause_t sleepFor(int seconds) {
  if (seconds < 60) seconds = 60;
  Serial.printf("Sleeping for %d s (side switch wakes early)\n", seconds);
  Serial.flush();
  // Wait for the wheel to be released so we don't wake immediately
  while (readButton() != BTN_NONE) delay(10);
  delay(50);  // debounce
  // Light sleep can wake on any GPIO level, so all three wheel positions work
  for (gpio_num_t pin : BTN_PINS) gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
  esp_sleep_enable_gpio_wakeup();
  M5.Power.lightSleep((uint64_t)seconds * 1000000ULL, /*touch_wakeup=*/false);
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  wakeButton = (cause == ESP_SLEEP_WAKEUP_GPIO) ? captureButton() : BTN_NONE;
  for (gpio_num_t pin : BTN_PINS) gpio_wakeup_disable(pin);
  M5.Display.wakeup();
  return cause;
}

// One wake-up: handle the wheel, refresh/draw the current screen, return
// how long to sleep.
static int handleWake(esp_sleep_wakeup_cause_t cause) {
  Button btn = wakeButton;
  int dir = btn == BTN_NEXT_PRESSED ? +1 : btn == BTN_PREV_PRESSED ? -1 : 0;
  bool forceRefresh = (btn == BTN_PUSH_PRESSED);
  Serial.printf("Wake cause: %d, button: %s\n", (int)cause,
                dir > 0 ? "next" : dir < 0 ? "prev" : forceRefresh ? "push" : "none");

  int screenIdx = wrapIndex(prefs.getInt("screen", 0), SCREEN_COUNT);
#ifdef FORCE_SCREEN
  // Test hook: PLATFORMIO_BUILD_FLAGS=-DFORCE_SCREEN=1 pio run -t upload
  // starts on that screen after a cold boot / reset.
  if (cause == ESP_SLEEP_WAKEUP_UNDEFINED && dir == 0) {
    screenIdx = wrapIndex(FORCE_SCREEN, SCREEN_COUNT);
    prefs.putInt("screen", screenIdx);
  }
#endif

  bool switched = false;
  if (dir != 0) {
    screenIdx = wrapIndex(screenIdx + dir, SCREEN_COUNT);
    prefs.putInt("screen", screenIdx);
    switched = true;
  }
  const Screen& screen = SCREENS[screenIdx];

  Context ctx;
  ctx.now = rtcNow();
  ctx.battery = readBatteryPercent();
  Serial.printf("Screen: %s, battery %d%%, now %s\n", screen.name, ctx.battery,
                ctx.now ? formatLocal(ctx.now).c_str() : "(unset)");

  // Each screen has its own next-refresh deadline (UTC epoch in prefs).
  // Refresh when the deadline is unknown/passed or the clock is invalid.
  long deadline = prefs.getLong(deadlineKey(screenIdx).c_str(), 0);
  bool due = (ctx.now == 0) || deadline == 0 || ctx.now >= deadline - 30;
  if (dir == 0) due = true;  // timer wake, push or cold boot: always refresh

  int sleepS;
  if (due) {
    int interval = screen.refresh(ctx);
    if (ctx.now == 0) ctx.now = rtcNow();
    if (ctx.now) prefs.putLong(deadlineKey(screenIdx).c_str(), ctx.now + interval);
    sleepS = interval;
    drawScreen(screen, ctx);
  } else {
    sleepS = (int)(deadline - ctx.now);
    if (switched) drawScreen(screen, ctx);
  }
  return sleepS;
}

static esp_sleep_wakeup_cause_t lastCause = ESP_SLEEP_WAKEUP_UNDEFINED;

void setup() {
  auto cfg = M5.config();
  cfg.output_power = false;  // no EXT 5V boost: less load on a weak supply
  M5.begin(cfg);
  Serial.begin(115200);
  Serial.printf("\nargentstars boot, reset reason %d\n", (int)esp_reset_reason());

  // Panel is portrait-native; we want landscape
  if (M5.Display.width() < M5.Display.height()) {
    M5.Display.setRotation(M5.Display.getRotation() ^ 1);
  }
  for (gpio_num_t pin : BTN_PINS) pinMode(pin, INPUT_PULLUP);
  prefs.begin("argentstars");
}

void loop() {
  int sleepS = handleWake(lastCause);
  lastCause = sleepFor(sleepS);
}
