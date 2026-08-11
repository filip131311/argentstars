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
