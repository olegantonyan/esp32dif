#pragma once

#include "oled.h"

#define STATUS_OLED_FRONT_BIG_ROWS 0x55

void status_oled_back(oled_lines_t lines, int rows);
void status_oled_front(oled_lines_t lines, int rows);
