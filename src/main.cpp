#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>
#include <lvgl.h>
#include "secrets.h"
#include "emoji_art.h"

// ===== HARDWARE =====

// Touch (HSPI bus)
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

// ===== DATA =====

struct Task {
    String id;
    String content;
    String sectionId;
    String dueDate;
    bool completed;
};

struct Section {
    String id;
    String name;
};

static const int MAX_TASKS = 20;
static const int MAX_SECTIONS = 4;
Task allTasks[MAX_TASKS];
int allTaskCount = 0;
Section sections[MAX_SECTIONS];
int sectionCount = 0;  // excludes Rewards section
int activeTab = 0;

// Rewards (fetched from Todoist "Rewards" section)
static const int MAX_REWARDS = 15;
String rewards[MAX_REWARDS];
int rewardCount = 0;
String rewardsSectionId = "";

// Completion queue
static const int MAX_QUEUE = 10;
String completeQueue[MAX_QUEUE];
int queueCount = 0;

// Timing
unsigned long lastRefresh = 0;
unsigned long lastComplete = 0;
unsigned long lastActivity = 0;
bool screenDimmed = false;
bool screenOff = false;
const unsigned long DIM_TIMEOUT = 300000;   // 5 min
const unsigned long OFF_TIMEOUT = 600000;   // 10 min
bool pendingRefresh = false;
const unsigned long REFRESH_INTERVAL = 60000;
const unsigned long COMPLETE_DEBOUNCE = 2000;

// LVGL widgets
lv_obj_t* tabview = NULL;
lv_obj_t* tabPages[MAX_SECTIONS];
lv_obj_t* tabCounterLabels[MAX_SECTIONS];
lv_obj_t* tabProgressBars[MAX_SECTIONS];
int tabTotalCounts[MAX_SECTIONS];
int tabCompletedCounts[MAX_SECTIONS];
int tabOriginalTotals[MAX_SECTIONS];  // persists across rebuilds
bool tabTotalsInitialized = false;
String lastKnownDate = "";

// Rainbow colors for progress bar based on completion %
uint32_t getProgressColor(int completed, int total) {
    if (total == 0) return 0xE91E8C;
    int pct = (completed * 100) / total;
    if (pct < 20)  return 0xE91E8C;  // pink
    if (pct < 40)  return 0xFF6B6B;  // coral
    if (pct < 60)  return 0xFFA500;  // orange
    if (pct < 80)  return 0xFFD700;  // gold
    if (pct < 100) return 0x7CFC00;  // green
    return 0x4CAF50;                  // complete green
}

// Forward declarations
void buildUI();
void fetchTasks();
bool isWeeklySection(int tab);
static void animOpaCb(void* obj, int32_t v);

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
            // Wake up — restore backlight, refresh data
            analogWrite(21, 255);
            screenDimmed = false;
            screenOff = false;
            lastActivity = millis();
            // Swallow the touch so it doesn't trigger a checkbox
            data->state = LV_INDEV_STATE_RELEASED;
            // Refresh on wake
            fetchTasks();
            buildUI();
            return;
        }

        lastActivity = millis();
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ===== WIFI & TIME =====

void connectWiFi() {
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        configTime(TIMEZONE_OFFSET * 3600, DST_OFFSET * 3600, "pool.ntp.org");
        struct tm timeinfo;
        int ntpAttempts = 0;
        while (!getLocalTime(&timeinfo) && ntpAttempts < 10) {
            delay(500);
            ntpAttempts++;
        }
    }
}

String getTodayDate() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return "";
    char buf[11];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &timeinfo);
    return String(buf);
}

String getWeekEndDate() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) return "";
    time_t now = mktime(&timeinfo);
    now += 7 * 24 * 3600;
    struct tm* future = localtime(&now);
    char buf[11];
    strftime(buf, sizeof(buf), "%Y-%m-%d", future);
    return String(buf);
}

// ===== TODOIST API =====

void fetchSections() {
    if (WiFi.status() != WL_CONNECTED) return;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://api.todoist.com/api/v1/sections?project_id=" TODOIST_PROJECT_ID);
    http.addHeader("Authorization", "Bearer " TODOIST_API_TOKEN);
    int code = http.GET();
    if (code == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
            JsonArray arr = doc["results"].as<JsonArray>();
            sectionCount = 0;
            rewardsSectionId = "";
            for (JsonObject obj : arr) {
                String name = obj["name"].as<String>();
                String id   = obj["id"].as<String>();
                // Skip Rewards section from tabs
                if (name == "Rewards") {
                    rewardsSectionId = id;
                    continue;
                }
                if (sectionCount >= MAX_SECTIONS) break;
                sections[sectionCount].id   = id;
                sections[sectionCount].name = name;
                sectionCount++;
            }
        }
    }
    http.end();
}

void fetchTasks() {
    if (WiFi.status() != WL_CONNECTED) return;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://api.todoist.com/api/v1/tasks?project_id=" TODOIST_PROJECT_ID);
    http.addHeader("Authorization", "Bearer " TODOIST_API_TOKEN);
    int code = http.GET();
    if (code == 200) {
        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
            JsonArray arr = doc["results"].as<JsonArray>();
            allTaskCount = 0;
            rewardCount = 0;
            for (JsonObject obj : arr) {
                String sid = obj["section_id"].as<String>();
                if (sid == "null") sid = "";

                // Rewards go to separate array
                if (sid == rewardsSectionId && rewardsSectionId.length() > 0) {
                    if (rewardCount < MAX_REWARDS) {
                        rewards[rewardCount++] = obj["content"].as<String>();
                    }
                    continue;
                }

                if (allTaskCount >= MAX_TASKS) break;
                allTasks[allTaskCount].id      = obj["id"].as<String>();
                allTasks[allTaskCount].content  = obj["content"].as<String>();
                allTasks[allTaskCount].sectionId = sid;
                if (obj["due"].is<JsonObject>()) {
                    allTasks[allTaskCount].dueDate = obj["due"]["date"].as<String>();
                } else {
                    allTasks[allTaskCount].dueDate = "";
                }
                allTasks[allTaskCount].completed = false;
                allTaskCount++;
            }
        }
    }
    http.end();
    lastRefresh = millis();
}

void sendComplete(const String& taskId) {
    if (WiFi.status() != WL_CONNECTED) return;
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://api.todoist.com/api/v1/tasks/" + taskId + "/close");
    http.addHeader("Authorization", "Bearer " TODOIST_API_TOKEN);
    http.POST("");
    http.end();
}

// Reset all tasks — set proper recurrence and reschedule to today
// Helper to reset a single task with retry
bool resetSingleTask(const String& tid, const String& dueString, const String& today) {
    for (int attempt = 0; attempt < 3; attempt++) {
        WiFiClientSecure c;
        c.setInsecure();
        c.setTimeout(5000);
        HTTPClient h;
        h.begin(c, "https://api.todoist.com/api/v1/tasks/" + tid);
        h.addHeader("Authorization", "Bearer " TODOIST_API_TOKEN);
        h.addHeader("Content-Type", "application/json");
        String body = "{\"due_date\":\"" + today + "\",\"due_string\":\"" + dueString + "\",\"due_lang\":\"en\"}";
        int code = h.POST(body);
        h.end();
        if (code == 200) return true;
        delay(200);  // brief pause before retry
    }
    return false;
}

void resetAllTasks() {
    if (WiFi.status() != WL_CONNECTED) return;

    // Build list of weekly section IDs
    String weeklySectionIds[MAX_SECTIONS];
    int weeklyCount = 0;
    for (int i = 0; i < sectionCount; i++) {
        if (isWeeklySection(i)) {
            weeklySectionIds[weeklyCount++] = sections[i].id;
        }
    }

    String today = getTodayDate();

    // Fetch current tasks
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://api.todoist.com/api/v1/tasks?project_id=" TODOIST_PROJECT_ID);
    http.addHeader("Authorization", "Bearer " TODOIST_API_TOKEN);
    int code = http.GET();
    if (code == 200) {
        // Store task info before closing connection
        struct ResetInfo { String id; String dueString; };
        ResetInfo toReset[MAX_TASKS];
        int resetCount = 0;

        JsonDocument doc;
        if (!deserializeJson(doc, http.getString())) {
            JsonArray arr = doc["results"].as<JsonArray>();
            for (JsonObject obj : arr) {
                String sid = obj["section_id"].as<String>();
                if (sid == "null") sid = "";
                String tid = obj["id"].as<String>();

                if (sid == rewardsSectionId || sid.length() == 0) continue;

                bool isWeekly = false;
                for (int w = 0; w < weeklyCount; w++) {
                    if (sid == weeklySectionIds[w]) { isWeekly = true; break; }
                }

                if (resetCount < MAX_TASKS) {
                    toReset[resetCount].id = tid;
                    toReset[resetCount].dueString = isWeekly ? "every monday" : "every day";
                    resetCount++;
                }
            }
        }
        http.end();

        // Now reset each task with retries
        for (int i = 0; i < resetCount; i++) {
            resetSingleTask(toReset[i].id, toReset[i].dueString, today);
            delay(100);  // pace requests
        }
    } else {
        http.end();
    }
}

// Check if a tab/section is "weekly" based on its name
bool isWeeklySection(int tab) {
    if (tab < 0 || tab >= sectionCount) return false;
    String name = sections[tab].name;
    name.toLowerCase();
    return name.indexOf("week") >= 0 || name.indexOf("chore") >= 0;
}

// ===== ENCOURAGEMENT TOAST =====

static const char* encourageMessages[] = {
    LV_SYMBOL_OK " Nice one!",
    LV_SYMBOL_OK " Keep going!",
    LV_SYMBOL_OK " Great!",
    LV_SYMBOL_OK " Awesome!",
    LV_SYMBOL_OK " Yes!",
    LV_SYMBOL_OK " Way to go!",
};
static const int ENCOURAGE_COUNT = 6;
static int encourageIdx = 0;

static void toastDeleteCb(lv_anim_t* a) {
    lv_obj_delete((lv_obj_t*)a->var);
}

void showToast() {
    // Container with solid background
    lv_obj_t* toastBg = lv_obj_create(lv_screen_active());
    lv_obj_set_size(toastBg, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(toastBg, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(toastBg, lv_color_hex(0x2A1040), 0);
    lv_obj_set_style_bg_opa(toastBg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(toastBg, lv_color_hex(0xE91E8C), 0);
    lv_obj_set_style_border_width(toastBg, 1, 0);
    lv_obj_set_style_radius(toastBg, 8, 0);
    lv_obj_set_style_pad_hor(toastBg, 16, 0);
    lv_obj_set_style_pad_ver(toastBg, 6, 0);
    lv_obj_remove_flag(toastBg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(toastBg, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t* toast = lv_label_create(toastBg);
    lv_label_set_text(toast, encourageMessages[encourageIdx % ENCOURAGE_COUNT]);
    encourageIdx++;
    lv_obj_set_style_text_font(toast, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(toast, lv_color_hex(0xE91E8C), 0);

    // Fade out animation on the container
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, toastBg);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, 1500);
    lv_anim_set_delay(&a, 800);
    lv_anim_set_exec_cb(&a, animOpaCb);
    lv_anim_set_deleted_cb(&a, toastDeleteCb);
    lv_anim_start(&a);
}

// ===== CHECKBOX EVENT =====

// Row tap handler — toggles the checkbox inside
static void rowClickCb(lv_event_t* e) {
    lv_obj_t* row = (lv_obj_t*)lv_event_get_target(e);
    lv_obj_t* cb = lv_obj_get_child(row, 0);
    if (!cb) return;

    if (lv_obj_has_state(cb, LV_STATE_CHECKED)) {
        lv_obj_remove_state(cb, LV_STATE_CHECKED);
    } else {
        lv_obj_add_state(cb, LV_STATE_CHECKED);
    }
    lv_obj_send_event(cb, LV_EVENT_VALUE_CHANGED, NULL);
}

static void checkboxEventCb(lv_event_t* e) {
    lv_obj_t* cb = (lv_obj_t*)lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);

    if (idx < 0 || idx >= allTaskCount) return;

    bool checked = lv_obj_has_state(cb, LV_STATE_CHECKED);

    if (checked && !allTasks[idx].completed) {
        // Checking off
        allTasks[idx].completed = true;

        if (queueCount < MAX_QUEUE) {
            completeQueue[queueCount++] = allTasks[idx].id;
        }
        lastComplete = millis();
        pendingRefresh = true;

        lv_obj_set_style_text_color(cb, lv_color_hex(0x555555), LV_PART_MAIN);
        lv_obj_set_style_text_decor(cb, LV_TEXT_DECOR_STRIKETHROUGH, LV_PART_MAIN);

        // Update counter and progress bar
        for (int t = 0; t < sectionCount; t++) {
            if (allTasks[idx].sectionId == sections[t].id) {
                tabCompletedCounts[t]++;
                if (tabCounterLabels[t]) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%d/%d", tabCompletedCounts[t], tabTotalCounts[t]);
                    lv_label_set_text(tabCounterLabels[t], buf);
                }
                if (tabProgressBars[t]) {
                    lv_bar_set_value(tabProgressBars[t], tabCompletedCounts[t], LV_ANIM_ON);
                    lv_obj_set_style_bg_color(tabProgressBars[t],
                        lv_color_hex(getProgressColor(tabCompletedCounts[t], tabTotalCounts[t])),
                        LV_PART_INDICATOR);
                }
                break;
            }
        }
        showToast();

    } else if (!checked && allTasks[idx].completed) {
        // Unchecking — remove from queue if still there
        allTasks[idx].completed = false;

        for (int q = 0; q < queueCount; q++) {
            if (completeQueue[q] == allTasks[idx].id) {
                for (int j = q + 1; j < queueCount; j++)
                    completeQueue[j - 1] = completeQueue[j];
                queueCount--;
                break;
            }
        }

        // Reset text style
        lv_obj_set_style_text_color(cb, lv_color_hex(0xEEEEEE), LV_PART_MAIN);
        lv_obj_set_style_text_decor(cb, LV_TEXT_DECOR_NONE, LV_PART_MAIN);

        // Update counter and progress bar
        for (int t = 0; t < sectionCount; t++) {
            if (allTasks[idx].sectionId == sections[t].id) {
                tabCompletedCounts[t]--;
                if (tabCompletedCounts[t] < 0) tabCompletedCounts[t] = 0;
                if (tabCounterLabels[t]) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), "%d/%d", tabCompletedCounts[t], tabTotalCounts[t]);
                    lv_label_set_text(tabCounterLabels[t], buf);
                }
                if (tabProgressBars[t]) {
                    lv_bar_set_value(tabProgressBars[t], tabCompletedCounts[t], LV_ANIM_ON);
                    lv_obj_set_style_bg_color(tabProgressBars[t],
                        lv_color_hex(getProgressColor(tabCompletedCounts[t], tabTotalCounts[t])),
                        LV_PART_INDICATOR);
                }
                break;
            }
        }

        // Reset debounce timer
        lastComplete = millis();
    }
}

// ===== CELEBRATION =====

// Praise messages
// Build praise messages with user's name
static String praiseStrings[8];
static const char* praiseMessages[8];
static bool praiseInitialized = false;

void initPraiseMessages() {
    if (praiseInitialized) return;
    const char* templates[] = {
        "Great job, %s!",
        "You're a star, %s!",
        "Super work, %s!",
        "Way to go, %s!",
        "Fantastic, %s!",
        "So proud of you, %s!",
        "Incredible, %s!",
        "You're awesome, %s!",
    };
    for (int i = 0; i < 8; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), templates[i], USER_NAME);
        praiseStrings[i] = String(buf);
        praiseMessages[i] = praiseStrings[i].c_str();
    }
    praiseInitialized = true;
}
static const int PRAISE_COUNT = 8;

// Rewards are fetched from Todoist "Rewards" section


// Animation callback for opacity (toast fade)
static void animOpaCb(void* obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t*)obj, v, 0);
}

// Get a deterministic reward index for today (same all day, changes daily)
int getDailyRewardIndex() {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo) || rewardCount == 0) return 0;
    // Use day-of-year as seed for consistent daily pick
    int dayOfYear = timeinfo.tm_yday;
    return dayOfYear % rewardCount;
}

// ===== BUILD UI =====

void buildUI() {
    // Save active tab before rebuilding
    int savedTab = activeTab;

    // Clear screen
    lv_obj_clean(lv_screen_active());

    // Apply pink/purple theme
    lv_color_t primary = lv_color_hex(0xE91E8C);
    lv_color_t secondary = lv_color_hex(0x9C27B0);
    lv_theme_t* th = lv_theme_default_init(
        lv_display_get_default(), primary, secondary, true, &lv_font_montserrat_16);
    lv_display_set_theme(lv_display_get_default(), th);

    // Create tabview
    tabview = lv_tabview_create(lv_screen_active());
    lv_tabview_set_tab_bar_size(tabview, 34);

    // Create tabs from sections + settings
    for (int i = 0; i < sectionCount; i++) {
        tabPages[i] = lv_tabview_add_tab(tabview, sections[i].name.c_str());
    }

    // Settings tab (gear icon)
    lv_obj_t* settingsPage = lv_tabview_add_tab(tabview, LV_SYMBOL_SETTINGS);

    // Resize tab buttons and set smaller font
    lv_obj_t* tabBar = lv_tabview_get_tab_bar(tabview);
    lv_obj_remove_flag(tabBar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(tabBar, &lv_font_montserrat_14, 0);
    uint32_t childCount = lv_obj_get_child_count(tabBar);
    for (uint32_t i = 0; i < childCount; i++) {
        lv_obj_t* btn = lv_obj_get_child(tabBar, i);
        if (i < (uint32_t)sectionCount) {
            lv_obj_set_flex_grow(btn, 3);
        } else {
            lv_obj_set_flex_grow(btn, 1);
        }
    }

    // Settings page content
    lv_obj_set_flex_flow(settingsPage, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(settingsPage, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(settingsPage, 30, 0);
    lv_obj_set_style_pad_row(settingsPage, 15, 0);

    // Reset Tasks button
    lv_obj_t* resetBtn = lv_btn_create(settingsPage);
    lv_obj_set_size(resetBtn, 200, 45);
    lv_obj_set_style_bg_color(resetBtn, lv_color_hex(0xE91E8C), 0);
    lv_obj_set_style_bg_color(resetBtn, lv_color_hex(0xFF69B4), LV_STATE_PRESSED);
    lv_obj_set_style_radius(resetBtn, 8, 0);
    lv_obj_add_event_cb(resetBtn, [](lv_event_t* e) {
        resetAllTasks();
        tabTotalsInitialized = false;  // reset totals on explicit reset
        fetchSections();
        fetchTasks();
        activeTab = 0;
        buildUI();
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* resetLabel = lv_label_create(resetBtn);
    lv_label_set_text(resetLabel, LV_SYMBOL_LOOP " Reset Tasks");
    lv_obj_center(resetLabel);

    // WiFi info
    lv_obj_t* wifiLabel = lv_label_create(settingsPage);
    String wifiInfo = String(LV_SYMBOL_WIFI " ") + WiFi.localIP().toString();
    lv_label_set_text(wifiLabel, wifiInfo.c_str());
    lv_obj_set_style_text_font(wifiLabel, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wifiLabel, lv_color_hex(0x888888), 0);

    // Restore active tab
    if (savedTab > 0 && savedTab <= sectionCount) {
        lv_tabview_set_active(tabview, savedTab, LV_ANIM_OFF);
        activeTab = savedTab;
    }

    // Track tab changes
    lv_obj_add_event_cb(tabview, [](lv_event_t* e) {
        lv_obj_t* tv = (lv_obj_t*)lv_event_get_target(e);
        activeTab = lv_tabview_get_tab_active(tv);
    }, LV_EVENT_VALUE_CHANGED, NULL);

    String today = getTodayDate();
    String weekEnd = getWeekEndDate();

    // Dark pastel rainbow palette
    static const uint32_t rowColors[] = {
        0x2D1B3D,  // deep plum
        0x1B2D3D,  // midnight blue
        0x1B3D2D,  // forest teal
        0x2D3D1B,  // olive dark
        0x3D2D1B,  // warm brown
        0x3D1B2D,  // berry
        0x1B3D3D,  // dark cyan
        0x3D1B1B,  // dark rose
        0x2B1B3D,  // indigo
        0x1B3D1B,  // dark green
    };
    static const int ROW_COLOR_COUNT = 10;

    for (int tab = 0; tab < sectionCount; tab++) {
        lv_obj_t* tabPage = tabPages[tab];

        // Count visible tasks for this tab
        int visibleCount = 0;
        for (int i = 0; i < allTaskCount; i++) {
            if (allTasks[i].sectionId != sections[tab].id) continue;
            String due = allTasks[i].dueDate;
            if (due.length() > 0) {
                if (!isWeeklySection(tab) && due > today) continue;
                if (isWeeklySection(tab) && due > weekEnd) continue;
            }
            visibleCount++;
        }

        bool needsScroll = visibleCount >= 5;

        lv_obj_set_flex_flow(tabPage, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_all(tabPage, 0, 0);
        lv_obj_set_style_pad_gap(tabPage, 0, 0);
        lv_obj_set_scrollbar_mode(tabPage, LV_SCROLLBAR_MODE_OFF);

        // Task list
        lv_obj_t* list = lv_obj_create(tabPage);
        lv_obj_set_flex_grow(list, 1);
        lv_obj_set_height(list, lv_pct(100));
        lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list, 3, 0);
        lv_obj_set_style_pad_all(list, 4, 0);
        lv_obj_set_style_pad_top(list, 5, 0);
        lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(list, 0, 0);
        lv_obj_set_style_radius(list, 0, 0);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);

        // Scroll buttons only if needed
        if (needsScroll) {
            lv_obj_t* scrollCol = lv_obj_create(tabPage);
            lv_obj_set_size(scrollCol, 28, lv_pct(100));
            lv_obj_set_flex_flow(scrollCol, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_all(scrollCol, 1, 0);
            lv_obj_set_style_pad_row(scrollCol, 1, 0);
            lv_obj_set_style_bg_opa(scrollCol, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(scrollCol, 0, 0);
            lv_obj_set_scrollbar_mode(scrollCol, LV_SCROLLBAR_MODE_OFF);

            lv_obj_t* upBtn = lv_btn_create(scrollCol);
            lv_obj_set_width(upBtn, 26);
            lv_obj_set_flex_grow(upBtn, 1);
            lv_obj_set_style_bg_color(upBtn, lv_color_hex(0x2A1040), 0);
            lv_obj_set_style_bg_color(upBtn, lv_color_hex(0xE91E8C), LV_STATE_PRESSED);
            lv_obj_set_style_radius(upBtn, 4, 0);
            lv_obj_t* upLabel = lv_label_create(upBtn);
            lv_label_set_text(upLabel, LV_SYMBOL_UP);
            lv_obj_center(upLabel);

            lv_obj_t* downBtn = lv_btn_create(scrollCol);
            lv_obj_set_width(downBtn, 26);
            lv_obj_set_flex_grow(downBtn, 1);
            lv_obj_set_style_bg_color(downBtn, lv_color_hex(0x2A1040), 0);
            lv_obj_set_style_bg_color(downBtn, lv_color_hex(0xE91E8C), LV_STATE_PRESSED);
            lv_obj_set_style_radius(downBtn, 4, 0);
            lv_obj_t* downLabel = lv_label_create(downBtn);
            lv_label_set_text(downLabel, LV_SYMBOL_DOWN);
            lv_obj_center(downLabel);

            lv_obj_add_event_cb(upBtn, [](lv_event_t* e) {
                lv_obj_t* l = (lv_obj_t*)lv_event_get_user_data(e);
                lv_obj_scroll_by(l, 0, 60, LV_ANIM_ON);
            }, LV_EVENT_CLICKED, list);

            lv_obj_add_event_cb(downBtn, [](lv_event_t* e) {
                lv_obj_t* l = (lv_obj_t*)lv_event_get_user_data(e);
                lv_obj_scroll_by(l, 0, -60, LV_ANIM_ON);
            }, LV_EVENT_CLICKED, list);
        }

        // Progress row: bar + counter
        // Only set original totals on first load or after fresh fetch
        if (!tabTotalsInitialized || visibleCount > tabOriginalTotals[tab]) {
            tabOriginalTotals[tab] = visibleCount;
        }
        int origTotal = tabOriginalTotals[tab];
        int completed = origTotal - visibleCount;  // tasks already removed = completed
        tabTotalCounts[tab] = origTotal;
        tabCompletedCounts[tab] = completed;
        tabCounterLabels[tab] = NULL;
        tabProgressBars[tab] = NULL;
        if (origTotal > 0 && visibleCount > 0) {
            lv_obj_t* progRow = lv_obj_create(list);
            lv_obj_set_size(progRow, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(progRow, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(progRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_all(progRow, 0, 0);
            lv_obj_set_style_pad_column(progRow, 6, 0);
            lv_obj_set_style_bg_opa(progRow, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(progRow, 0, 0);
            lv_obj_set_scrollbar_mode(progRow, LV_SCROLLBAR_MODE_OFF);

            // Progress bar
            lv_obj_t* bar = lv_bar_create(progRow);
            lv_obj_set_flex_grow(bar, 1);
            lv_obj_set_height(bar, 10);
            lv_bar_set_range(bar, 0, origTotal);
            lv_bar_set_value(bar, completed, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0x1A1A2E), LV_PART_MAIN);
            lv_obj_set_style_radius(bar, 5, LV_PART_MAIN);
            lv_obj_set_style_bg_color(bar,
                lv_color_hex(getProgressColor(completed, origTotal)),
                LV_PART_INDICATOR);
            lv_obj_set_style_radius(bar, 5, LV_PART_INDICATOR);
            tabProgressBars[tab] = bar;

            // Counter text
            lv_obj_t* counter = lv_label_create(progRow);
            char counterBuf[16];
            snprintf(counterBuf, sizeof(counterBuf), "%d/%d", completed, origTotal);
            lv_label_set_text(counter, counterBuf);
            lv_obj_set_style_text_font(counter, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(counter, lv_color_hex(0x888888), 0);
            tabCounterLabels[tab] = counter;
        }

        int rowIdx = 0;
        for (int i = 0; i < allTaskCount; i++) {
            if (allTasks[i].sectionId != sections[tab].id) continue;

            // Date filter
            String due = allTasks[i].dueDate;
            if (due.length() > 0) {
                if (!isWeeklySection(tab) && due > today) continue;
                if (isWeeklySection(tab) && due > weekEnd) continue;
            }

            // Row container with rainbow color
            lv_obj_t* row = lv_obj_create(list);
            lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(row, lv_color_hex(rowColors[rowIdx % ROW_COLOR_COUNT]), 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(row, 5, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 6, 0);
            lv_obj_set_style_pad_left(row, 8, 0);
            lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, rowClickCb, LV_EVENT_CLICKED, NULL);
            rowIdx++;

            // Checkbox inside the colored row
            lv_obj_t* cb = lv_checkbox_create(row);
            lv_checkbox_set_text(cb, allTasks[i].content.c_str());
            lv_obj_set_style_text_font(cb, &lv_font_montserrat_16, LV_PART_MAIN);
            lv_obj_set_style_text_color(cb, lv_color_hex(0xEEEEEE), LV_PART_MAIN);
            lv_obj_set_style_pad_ver(cb, 4, LV_PART_MAIN);

            // Checkbox indicator
            lv_obj_set_style_border_width(cb, 2, LV_PART_INDICATOR);
            lv_obj_set_style_border_color(cb, lv_color_hex(0xBBBBBB), LV_PART_INDICATOR);
            lv_obj_set_style_radius(cb, 4, LV_PART_INDICATOR);
            lv_obj_set_style_pad_all(cb, 3, LV_PART_INDICATOR);
            lv_obj_set_style_bg_color(cb, lv_color_hex(0xE91E8C), LV_PART_INDICATOR | LV_STATE_CHECKED);

            lv_obj_add_event_cb(cb, checkboxEventCb, LV_EVENT_VALUE_CHANGED, (void*)(intptr_t)i);

        }

        // If no tasks in this tab
        bool hasTasks = false;
        for (int i = 0; i < allTaskCount; i++) {
            if (allTasks[i].sectionId == sections[tab].id) {
                String due = allTasks[i].dueDate;
                if (due.length() > 0) {
                    if (tab == 0 && due > today) continue;
                    if (tab != 0 && due > weekEnd) continue;
                }
                hasTasks = true;
                break;
            }
        }
        if (!hasTasks) {
            lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_top(list, 20, 0);

            // Icon row
            lv_obj_t* iconRow = lv_obj_create(list);
            lv_obj_set_size(iconRow, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(iconRow, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(iconRow, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(iconRow, 12, 0);
            lv_obj_set_style_bg_opa(iconRow, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(iconRow, 0, 0);
            lv_obj_set_style_pad_all(iconRow, 0, 0);
            lv_obj_set_scrollbar_mode(iconRow, LV_SCROLLBAR_MODE_OFF);

            if (isWeeklySection(tab)) {
                // Weekly: trophy + money + trophy
                lv_image_set_src(lv_image_create(iconRow), &img_trophy_32);
                lv_image_set_src(lv_image_create(iconRow), &img_money_32);
                lv_image_set_src(lv_image_create(iconRow), &img_trophy_32);
            } else {
                // Daily: heart + party + heart
                lv_image_set_src(lv_image_create(iconRow), &img_heart_32);
                lv_image_set_src(lv_image_create(iconRow), &img_party_32);
                lv_image_set_src(lv_image_create(iconRow), &img_heart_32);
            }

            // Heading
            lv_obj_t* doneLabel = lv_label_create(list);
            if (isWeeklySection(tab)) {
                lv_label_set_text(doneLabel, "This week is complete!");
            } else {
                // Check section name for appropriate message
                String secName = sections[tab].name;
                secName.toLowerCase();
                if (secName.indexOf("morning") >= 0) {
                    lv_label_set_text(doneLabel, "Morning done!");
                } else if (secName.indexOf("night") >= 0) {
                    char nightBuf[48];
                    snprintf(nightBuf, sizeof(nightBuf), "Goodnight, %s!", USER_NAME);
                    lv_label_set_text(doneLabel, nightBuf);
                } else {
                    lv_label_set_text(doneLabel, "All done!");
                }
            }
            lv_obj_set_style_text_font(doneLabel, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(doneLabel, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_text_align(doneLabel, LV_TEXT_ALIGN_CENTER, 0);

            // Personalized praise
            struct tm timeinfo;
            int praiseIdx = 0;
            if (getLocalTime(&timeinfo)) {
                praiseIdx = timeinfo.tm_yday % PRAISE_COUNT;
            }
            lv_obj_t* label = lv_label_create(list);
            lv_label_set_text(label, praiseMessages[praiseIdx]);
            lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(label, lv_color_hex(0xE91E8C), 0);
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

            // Divider
            lv_obj_t* div = lv_obj_create(list);
            lv_obj_set_size(div, 180, 2);
            lv_obj_set_style_bg_color(div, lv_color_hex(0x9C27B0), 0);
            lv_obj_set_style_border_width(div, 0, 0);
            lv_obj_set_style_radius(div, 0, 0);
            lv_obj_set_style_pad_all(div, 0, 0);
            lv_obj_set_style_min_height(div, 2, 0);
            lv_obj_set_style_max_height(div, 2, 0);

            // Reward — daily reward only shows when ALL daily sections are empty
            if (!isWeeklySection(tab) && rewardCount > 0) {
                // Check if all non-weekly sections have no tasks today
                bool allDailySectionsDone = true;
                for (int s = 0; s < sectionCount; s++) {
                    if (isWeeklySection(s)) continue;
                    for (int t2 = 0; t2 < allTaskCount; t2++) {
                        if (allTasks[t2].sectionId != sections[s].id) continue;
                        String d = allTasks[t2].dueDate;
                        if (d.length() > 0 && d > today) continue;
                        allDailySectionsDone = false;
                        break;
                    }
                    if (!allDailySectionsDone) break;
                }

                if (allDailySectionsDone) {
                    lv_obj_t* rewardTitle = lv_label_create(list);
                    lv_label_set_text(rewardTitle, "Today's reward:");
                    lv_obj_set_style_text_font(rewardTitle, &lv_font_montserrat_14, 0);
                    lv_obj_set_style_text_color(rewardTitle, lv_color_hex(0xBBBBBB), 0);
                    lv_obj_set_style_text_align(rewardTitle, LV_TEXT_ALIGN_CENTER, 0);

                    int rewardIdx = getDailyRewardIndex();
                    String rewardText = rewards[rewardIdx];
                    if (rewardText.startsWith("* ")) rewardText = rewardText.substring(2);

                    lv_obj_t* rewardLabel = lv_label_create(list);
                    lv_label_set_text(rewardLabel, rewardText.c_str());
                    lv_obj_set_style_text_font(rewardLabel, &lv_font_montserrat_16, 0);
                    lv_obj_set_style_text_color(rewardLabel, lv_color_hex(0xFFFFFF), 0);
                    lv_obj_set_style_text_align(rewardLabel, LV_TEXT_ALIGN_CENTER, 0);
                    lv_label_set_long_mode(rewardLabel, LV_LABEL_LONG_WRAP);
                    lv_obj_set_width(rewardLabel, 260);
                } else {
                    lv_obj_t* hint = lv_label_create(list);
                    lv_label_set_text(hint, "Complete all daily tasks\nto unlock your reward!");
                    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, 0);
                    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
                    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
                }
            } else if (isWeeklySection(tab)) {
                lv_obj_t* rewardTitle = lv_label_create(list);
                lv_label_set_text(rewardTitle, "This week's reward:");
                lv_obj_set_style_text_font(rewardTitle, &lv_font_montserrat_14, 0);
                lv_obj_set_style_text_color(rewardTitle, lv_color_hex(0xBBBBBB), 0);
                lv_obj_set_style_text_align(rewardTitle, LV_TEXT_ALIGN_CENTER, 0);

                lv_obj_t* rewardLabel = lv_label_create(list);
                lv_label_set_text(rewardLabel, WEEKLY_REWARD);
                lv_obj_set_style_text_font(rewardLabel, &lv_font_montserrat_20, 0);
                lv_obj_set_style_text_color(rewardLabel, lv_color_hex(0x4CAF50), 0);
                lv_obj_set_style_text_align(rewardLabel, LV_TEXT_ALIGN_CENTER, 0);
            }
        }
    }
    tabTotalsInitialized = true;
}

// ===== SETUP & LOOP =====

void setup() {
    Serial.begin(115200);
    randomSeed(analogRead(34)); // seed from LDR noise
    initPraiseMessages();
    lastActivity = millis();

    // Backlight
    pinMode(21, OUTPUT);
    digitalWrite(21, HIGH);

    // Init TFT
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    // Init touch
    touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    ts.begin(touchSPI);
    ts.setRotation(1);

    // Init LVGL
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

    // Show loading screen
    lv_obj_t* label = lv_label_create(lv_screen_active());
    lv_label_set_text(label, "Connecting...");
    lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
    lv_obj_center(label);
    lv_timer_handler();

    // Connect WiFi
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) {
        lv_label_set_text(label, "WiFi Failed!");
        lv_obj_set_style_text_color(label, lv_color_hex(0xFF0000), 0);
        return;
    }

    lv_label_set_text(label, "Loading tasks...");
    lv_timer_handler();

    // Fetch data
    fetchSections();
    fetchTasks();

    // Build the UI
    buildUI();
}

void loop() {
    lv_timer_handler();

    // After 2s idle, rebuild UI with local state (removes checked tasks)
    if (pendingRefresh && millis() - lastComplete > COMPLETE_DEBOUNCE) {
        pendingRefresh = false;

        // Remove locally completed tasks
        int newCount = 0;
        for (int i = 0; i < allTaskCount; i++) {
            if (!allTasks[i].completed) {
                allTasks[newCount++] = allTasks[i];
            }
        }
        allTaskCount = newCount;

        // Rebuild UI immediately
        buildUI();
        lastRefresh = millis();
    }

    // Process ONE queued completion per loop (non-blocking)
    if (queueCount > 0 && !pendingRefresh) {
        sendComplete(completeQueue[0]);
        for (int i = 1; i < queueCount; i++)
            completeQueue[i - 1] = completeQueue[i];
        queueCount--;
    }

    // Check for day change — reset totals at midnight
    String currentDate = getTodayDate();
    if (currentDate.length() > 0 && lastKnownDate.length() > 0 && currentDate != lastKnownDate) {
        tabTotalsInitialized = false;
        lastKnownDate = currentDate;
        fetchTasks();
        buildUI();
    } else {
        if (lastKnownDate.length() == 0) lastKnownDate = currentDate;
    }

    // Periodic refresh
    if (millis() - lastRefresh > REFRESH_INTERVAL) {
        fetchTasks();
        buildUI();
    }

    // Screen dim/off based on inactivity
    unsigned long idle = millis() - lastActivity;
    if (!screenOff && idle > OFF_TIMEOUT) {
        // Turn screen off completely
        analogWrite(21, 0);
        screenOff = true;
        screenDimmed = false;
    } else if (!screenDimmed && !screenOff && idle > DIM_TIMEOUT) {
        // Dim screen
        analogWrite(21, 30);
        screenDimmed = true;
    }

    delay(5);
}
