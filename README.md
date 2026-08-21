# argentstars

M5Paper e-ink dashboard with several screens:

1. **GitHub stars** — current star count of
   [software-mansion/argent](https://github.com/software-mansion/argent),
   refreshed every hour.
2. **Countdown** — days left until a configurable date
   (`COUNTDOWN_*` in `config.h`), refreshed at local midnight.

Use the side wheel to move between screens: down = next, up = previous (they
form a cycle), push = refresh the current screen now. The screen you leave it
on stays until the next press and keeps its own refresh cadence (hourly for
stars, once a day for the countdown).

Between refreshes the device light-sleeps and wakes either from the timer or
from the wheel. On wake it connects to Wi-Fi only when the shown screen's
refresh is due: the stars screen fetches
`https://api.github.com/repos/<repo>` (conditional ETag request, with an HTML
scrape fallback when the shared-IP API quota is exhausted); the countdown only
needs the BM8563 RTC, which is set from the HTTP `Date` header of every fetch.
If Wi-Fi or the API fails it keeps the last known value on screen, marks it as
stale, and retries after 10 minutes.

The firmware stays resident in light sleep rather than powering off: on this
hardware a deep-sleep reboot intermittently browns out during the e-paper
re-init (losing the wake-up cause), and the touch panel never reports touches,
so the wheel is the input. Expect days–weeks on a charge rather than months.

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

## Development

- Pure date helpers live in `include/timeutil.h` and have a host test:
  `c++ -std=c++11 -Iinclude test/host_test.cpp -o /tmp/t && /tmp/t`.
- To start on a given screen after a reset without swiping (handy for checking
  a screen's rendering): `PLATFORMIO_BUILD_FLAGS=-DFORCE_SCREEN=1 pio run -t upload`.
- `cfg.output_power = false` keeps the EXT 5 V boost off; with it on, the
  boot-time surge tripped the brownout detector on a laptop USB port.
- Adding a screen: append a `{name, refresh, render}` entry to `SCREENS` in
  `src/main.cpp`. `refresh()` may use the network and returns the seconds until
  its next refresh; `render()` draws from cached data only.

## Notes

- TLS certificate validation is disabled (`setInsecure()`) — fine for a desk
  gadget, swap in GitHub's root CA if you care.
- All times are derived from the RTC, which is set from the HTTP `Date`
  header and shifted by the fixed `TZ_OFFSET_S` (no DST handling).
