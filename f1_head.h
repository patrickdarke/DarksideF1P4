// Driver portraits for the car screen: the small PNG each row of /drivers
// links to, fetched once per driver and kept decoded (RGB565 + alpha, the
// layout LVGL draws directly).
#pragma once
#include <stdint.h>

// Pixels of a loaded portrait (3 bytes each), or nullptr. Hold appLock.
const uint8_t *headGet(uint8_t num, uint16_t *w, uint16_t *h);
// True if this driver's portrait has not been loaded or tried lately.
bool headWanted(uint8_t num);
// Network task only: download and decode. Uses the API body buffer.
void headLoad(uint8_t num, const char *url);
