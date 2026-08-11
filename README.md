# argentstars

M5Paper e-ink dashboard showing the current GitHub star count of
[software-mansion/argent](https://github.com/software-mansion/argent),
refreshed every hour.

On each wake the device connects to Wi-Fi, fetches
`https://api.github.com/repos/<repo>` (unauthenticated — 1 req/h is far below
GitHub's rate limit), draws the count on the e-ink panel, and goes into
RTC-timed deep sleep for an hour. On battery it fully powers down between
refreshes (the BM8563 RTC wakes it), so a charge lasts roughly 1–2 months.
If Wi-Fi or the API fails it keeps the last known value on screen, marks it
as stale, and retries after 10 minutes.

## Hardware

M5Paper (ESP32-D0WDQ6, 4.7" 960x540 e-ink, 16 MB flash, PSRAM).

## Setup

1. Install [PlatformIO](https://platformio.org/) (`pip install platformio`).
2. Copy `include/config.example.h` to `include/config.h` and fill in your
   Wi-Fi credentials (and optionally the repo / interval / timezone).
3. Build and flash:

   ```sh
   pio run -t upload
   ```

4. Watch logs:

   ```sh
   pio device monitor
   ```

## Notes

- TLS certificate validation is disabled (`setInsecure()`) — fine for a desk
  gadget, swap in GitHub's root CA if you care.
- The "Updated" timestamp comes from the API response's `Date` header shifted
  by `TZ_OFFSET_S`; the device keeps no clock of its own across power-off.
