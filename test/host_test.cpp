// Host-side unit test for the pure helpers in include/timeutil.h.
// Build & run:  c++ -std=c++11 -Iinclude test/host_test.cpp -o /tmp/t && /tmp/t
#include <cstdio>
#include <cstdlib>
#include "timeutil.h"

static int fails = 0;
#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++fails;                                                        \
    }                                                                 \
  } while (0)

int main() {
  const long TZ = 7200;  // CEST
  // Known anchors
  CHECK(daysFromCivil(1970, 1, 1) == 0);
  CHECK(daysFromCivil(2000, 3, 1) == 11017);
  CHECK(daysFromCivil(2026, 10, 1) == 20727);

  // 2026-08-21 10:00 UTC = 1787392800 ... compute from daysFromCivil instead
  time_t aug21_10utc = (time_t)daysFromCivil(2026, 8, 21) * 86400 + 10 * 3600;
  CHECK(daysUntil(aug21_10utc, TZ, 2026, 10, 1) == 41);

  // Local midnight boundary: 2026-09-30 21:59:59 UTC is 23:59:59 CEST -> 1 day
  time_t sep30_2159 = (time_t)daysFromCivil(2026, 9, 30) * 86400 + 21 * 3600 + 59 * 60 + 59;
  CHECK(daysUntil(sep30_2159, TZ, 2026, 10, 1) == 1);
  // one second later it is Oct 1st locally -> 0
  CHECK(daysUntil(sep30_2159 + 1, TZ, 2026, 10, 1) == 0);
  // the day after -> -1
  CHECK(daysUntil(sep30_2159 + 1 + 86400, TZ, 2026, 10, 1) == -1);

  // secondsToLocalMidnight
  CHECK(secondsToLocalMidnight(sep30_2159, TZ) == 1);
  CHECK(secondsToLocalMidnight(sep30_2159 + 1, TZ) == 86400);
  CHECK(secondsToLocalMidnight(aug21_10utc, TZ) == 12 * 3600);

  // swipe classification
  CHECK(classifySwipe(-200, 10, 80) == +1);
  CHECK(classifySwipe(200, -10, 80) == -1);
  CHECK(classifySwipe(30, 0, 80) == 0);      // too short: tap
  CHECK(classifySwipe(100, 90, 80) == 0);    // too diagonal
  CHECK(classifySwipe(-100, 40, 80) == +1);

  // wrapIndex cycles both ways
  CHECK(wrapIndex(2, 2) == 0);
  CHECK(wrapIndex(-1, 2) == 1);
  CHECK(wrapIndex(1, 2) == 1);

  if (fails) { std::printf("%d failure(s)\n", fails); return 1; }
  std::printf("all host tests passed\n");
  return 0;
}
