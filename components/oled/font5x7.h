#pragma once

#include <stdint.h>

#define FONT5X7_FIRST 0x20
#define FONT5X7_LAST 0x7E
#define FONT5X7_CYR_COUNT 66
#define FONT5X7_ICON_COUNT 3

extern const uint8_t font5x7[FONT5X7_LAST - FONT5X7_FIRST + 1][5];
extern const uint8_t font5x7_cyr[FONT5X7_CYR_COUNT][5];
extern const uint8_t font5x7_icons[FONT5X7_ICON_COUNT][5];
