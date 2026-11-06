#pragma once

#include <stdint.h>

#define FONT5X7_FIRST 0x20
#define FONT5X7_LAST 0x7E

extern const uint8_t font5x7[FONT5X7_LAST - FONT5X7_FIRST + 1][5];
