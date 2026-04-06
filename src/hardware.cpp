#include "hardware.h"
#include "data.h"
#include <SPI.h>

// ===== HARDWARE PINS =====

#define TOUCH_CS   33
#define TOUCH_IRQ  36
#define TOUCH_MOSI 32
#define TOUCH_MISO 39
#define TOUCH_CLK  25

SPIClass touchSPI(HSPI);
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
TFT_eSPI tft = TFT_eSPI();

// LVGL display buffer
static uint8_t lvBuf[320 * 20 * 2];  // 20 rows, RGB565

// ===== LVGL DISPLAY DRIVER =====

static void lvFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t*)px_map, w * h, true);
    tft.endWrite();
    lv_display_flush_ready(disp);
}

// ===== LVGL TOUCH DRIVER =====

static void lvTouchCb(lv_indev_t* indev, lv_indev_data_t* data) {
    if (ts.touched()) {
        TS_Point p = ts.getPoint();
        data->point.x = constrain(map(p.x, 200, 3800, 0, 320), 0, 319);
        data->point.y = constrain(map(p.y, 200, 3800, 0, 240), 0, 239);

        if (screenOff || screenDimmed) {
            // Wake up — restore backlight, set flag for main loop to refresh
            analogWrite(21, 255);
            screenDimmed = false;
            screenOff = false;
            lastActivity = millis();
            // Swallow the touch so it doesn't trigger a checkbox
            data->state = LV_INDEV_STATE_RELEASED;
            pendingWake = true;
            return;
        }

        lastActivity = millis();
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ===== INIT =====

void initHardware() {
    // Backlight
    pinMode(21, OUTPUT);
    digitalWrite(21, HIGH);

    // Init TFT
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    // Init touch on separate HSPI bus
    touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    ts.begin(touchSPI);
    ts.setRotation(1);
}

void initLVGL() {
    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return millis(); });

    // Create LVGL display
    lv_display_t* disp = lv_display_create(320, 240);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, lvBuf, NULL, sizeof(lvBuf), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, lvFlushCb);

    // Create LVGL touch input
    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, lvTouchCb);
}
