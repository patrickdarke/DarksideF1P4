// millis() as a timestamp in which 0 means "not set".
#pragma once
#include <Arduino.h>

// Never in the future. `millis() | 1`, the usual way to keep a stamp off 0, is
// 1 ms ahead whenever millis() is even, and an unsigned "how long ago" taken
// in that same millisecond (`millis() - stamp > period`) then reads 49.7 days:
// the test fires at once, on about every second try.
static inline uint32_t stampNow() {
  uint32_t t = millis();
  return t ? t : 0xFFFFFFFFu;
}
