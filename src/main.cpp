// argentstars — M5Paper e-ink dashboard with several swipeable screens:
//   0. GitHub star count of a repository (refreshed hourly)
//   1. Countdown of days until a target date (refreshed at local midnight)
//
// Between refreshes the device deep-sleeps. It wakes either from the RTC
// timer (scheduled refresh of the screen currently shown) or from the touch
// panel INT line (GPIO36). On a touch wake a horizontal swipe moves to the
// next/previous screen (cyclic); the new screen then keeps its own refresh
// cadence until the next swipe.
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
#include <driver/gpio.h>
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

// Tracks the finger after a touch wake-up and classifies the movement.
// Returns +1 (swipe left -> next screen), -1 (swipe right -> previous), 0.
// By the time we get here the wake-up boot has consumed a few hundred ms,
// so the finger is usually mid-swipe: we measure from where it is now.
static int detectSwipe() {
  const uint32_t MAX_WAIT_MS = 1500;
  const int MIN_DISTANCE = M5.Display.width() / 10;  // ~96 px in landscape
  uint32_t start = millis();
  int firstX = -1, firstY = -1, lastX = -1, lastY = -1;
  int misses = 0;
  while (millis() - start < MAX_WAIT_MS) {
    int16_t x, y;
    if (M5.Display.getTouch(&x, &y)) {
      if (firstX < 0) {
        firstX = x;
        firstY = y;
      }
      lastX = x;
      lastY = y;
      misses = 0;
    } else if (firstX >= 0 && ++misses >= 3) {
      break;  // finger released
    }
    delay(10);
  }
  if (firstX < 0) {
    Serial.println("touch: woke up but no touch seen (gesture too quick?)");
    return 0;
  }
  int dir = classifySwipe(lastX - firstX, lastY - firstY, MIN_DISTANCE);
  Serial.printf("touch: (%d,%d)->(%d,%d) in %lu ms -> %s\n", firstX, firstY,
                lastX, lastY, (unsigned long)(millis() - start),
                dir > 0 ? "next" : dir < 0 ? "prev" : "tap");
  return dir;
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

  String big, unit;
  if (ctx.now == 0) {
    big = "--";
    unit = "brak zegara";
  } else {
    long days = daysUntil(ctx.now, TZ_OFFSET_S, COUNTDOWN_YEAR, COUNTDOWN_MONTH,
                          COUNTDOWN_DAY);
    if (days < 0) days = 0;
    big = String(days);
    unit = days == 1 ? "dzień" : "dni";
  }
  g.setFont(&fonts::FreeSansBold24pt7b);
  g.setTextSize(5);
  g.setTextDatum(middle_center);
  g.drawString(big, w / 2, 290);

  g.setFont(&fonts::efontJA_24);
  g.setTextSize(2);
  g.setTextDatum(top_center);
  g.drawString(unit, w / 2, 390);

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

static void goToSleep(int seconds) {
  if (seconds < 60) seconds = 60;
  Serial.printf("Sleeping for %d s (touch wakes early)\n", seconds);
  Serial.flush();
  // Keep the main-power hold line (GPIO2) high through deep sleep; on battery
  // M5.Power.timerSleep() would power off instead, which would make the touch
  // panel unable to wake us.
  gpio_hold_en(GPIO_NUM_2);
  gpio_deep_sleep_hold_en();
  M5.Power.deepSleep((uint64_t)seconds * 1000000ULL, /*touch_wakeup=*/true);
}

void setup() {
  gpio_hold_dis(GPIO_NUM_2);
  auto cfg = M5.config();
  M5.begin(cfg);
  Serial.begin(115200);

  // Panel is portrait-native; we want landscape
  if (M5.Display.width() < M5.Display.height()) {
    M5.Display.setRotation(M5.Display.getRotation() ^ 1);
  }

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  bool touchWake = (cause == ESP_SLEEP_WAKEUP_EXT0);
  Serial.printf("Wake cause: %s\n", touchWake ? "touch"
                                    : cause == ESP_SLEEP_WAKEUP_TIMER ? "timer"
                                                                      : "boot");

  prefs.begin("argentstars");
  int screenIdx = wrapIndex(prefs.getInt("screen", 0), SCREEN_COUNT);
#ifdef FORCE_SCREEN
  // Test hook: `pio run -e m5paper -t upload` with
  // PLATFORMIO_BUILD_FLAGS=-DFORCE_SCREEN=1 starts on that screen after a
  // cold boot / reset, without needing a swipe.
  if (!touchWake && cause != ESP_SLEEP_WAKEUP_TIMER) {
    screenIdx = wrapIndex(FORCE_SCREEN, SCREEN_COUNT);
    prefs.putInt("screen", screenIdx);
  }
#endif

  bool swiped = false;
  if (touchWake) {
    int dir = detectSwipe();
    if (dir != 0) {
      screenIdx = wrapIndex(screenIdx + dir, SCREEN_COUNT);
      prefs.putInt("screen", screenIdx);
      swiped = true;
    }
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
  if (!touchWake) due = true;  // timer wake or cold boot: always refresh

  int sleepFor;
  if (due) {
    int interval = screen.refresh(ctx);
    if (ctx.now == 0) ctx.now = rtcNow();
    if (ctx.now) prefs.putLong(deadlineKey(screenIdx).c_str(), ctx.now + interval);
    sleepFor = interval;
    drawScreen(screen, ctx);
  } else {
    sleepFor = (int)(deadline - ctx.now);
    if (swiped) drawScreen(screen, ctx);
    // else: a tap/missed gesture — the panel already shows this screen
  }
  prefs.end();
  goToSleep(sleepFor);
}

void loop() {
  // Never reached: deepSleep() resets on wake and setup() runs again
}
