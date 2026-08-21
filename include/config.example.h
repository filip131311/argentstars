// Copy this file to config.h and fill in your values.
#pragma once

#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASS "your-wifi-password"

// Repository to track, "owner/name"
#define GITHUB_REPO "software-mansion/argent"

// Seconds between refreshes
#define REFRESH_INTERVAL_S 3600
// Retry sooner when Wi-Fi or the API call fails
#define RETRY_INTERVAL_S 600

// Offset applied to the UTC time from the API response when shown on screen
#define TZ_OFFSET_S 7200  // UTC+2 (CEST)

// Optional: GitHub personal access token for a dedicated 5000 req/h quota.
// NOTE: only set this after enabling real TLS validation - the firmware
// currently uses setInsecure(), which would expose the token to MITM.
#define GITHUB_TOKEN ""

// Countdown screen (wheel up/down from the stars screen): days left until
// this date, shown under the label. Refreshed at local midnight.
#define COUNTDOWN_LABEL "Dni do odejścia Kacpra:"
#define COUNTDOWN_YEAR 2026
#define COUNTDOWN_MONTH 10
#define COUNTDOWN_DAY 1
