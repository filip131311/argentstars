// Pure date/gesture helpers with no Arduino dependencies so they can be
// unit-tested on the host (see test/host_test.cpp).
#pragma once

#include <stdint.h>
#include <time.h>

// Days since the Unix epoch for a civil date (proleptic Gregorian).
// Howard Hinnant's days_from_civil.
static inline long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

// Local calendar day (days since epoch) for a UTC instant shifted by tzOffset.
static inline long localDay(time_t utc, long tzOffsetS) {
  long long t = (long long)utc + tzOffsetS;
  // floor division, also correct for negative values
  long long d = t / 86400;
  if (t % 86400 < 0) --d;
  return (long)d;
}

// Whole calendar days from "today" (local) until the target date.
// Today -> 0, tomorrow -> 1, yesterday -> -1.
static inline long daysUntil(time_t nowUtc, long tzOffsetS, int y, int m,
                             int d) {
  return daysFromCivil(y, (unsigned)m, (unsigned)d) - localDay(nowUtc, tzOffsetS);
}

// Seconds from nowUtc until the next local midnight (1..86400).
static inline long secondsToLocalMidnight(time_t nowUtc, long tzOffsetS) {
  long long t = (long long)nowUtc + tzOffsetS;
  long long sod = t % 86400;
  if (sod < 0) sod += 86400;
  return (long)(86400 - sod);
}

// Classify a touch movement as a horizontal swipe.
// Returns +1 for a swipe to the left (finger moves towards smaller x,
// i.e. "next page"), -1 for a swipe to the right ("previous page"),
// 0 when it is a tap or a mostly-vertical movement.
static inline int classifySwipe(int dx, int dy, int minDistance) {
  int adx = dx < 0 ? -dx : dx;
  int ady = dy < 0 ? -dy : dy;
  if (adx < minDistance || adx < ady * 2) return 0;
  return dx < 0 ? +1 : -1;
}

// Wrap a screen index into [0, count).
static inline int wrapIndex(int idx, int count) {
  if (count <= 0) return 0;
  idx %= count;
  if (idx < 0) idx += count;
  return idx;
}
