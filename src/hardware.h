#pragma once
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>

extern TFT_eSPI tft;
extern XPT2046_Touchscreen ts;

void initHardware();
void initLVGL();
