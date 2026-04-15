#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include "data.h"
#include "hardware.h"
#include "wifi_time.h"
#include "todoist.h"
#include "ui.h"

void setup() {
    Serial.begin(115200);
    randomSeed(analogRead(34)); // seed from LDR noise
    initPraiseMessages();
    lastActivity = millis();

    // Init hardware and LVGL
    initHardware();
    initLVGL();

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

    // Fetch data and build UI
    fetchSections();
    fetchTasks();
    buildUI();
}

void loop() {
    lv_timer_handler();

    // Handle wake from sleep — refresh data and rebuild UI
    if (pendingWake) {
        pendingWake = false;
        fetchTasks();
        buildUI();
    }

    // Reset-tasks flow — runs the blocking HTTP cycle here (outside the
    // LVGL event loop) so we can safely rebuild the UI afterward. Shows
    // a full-screen "Resetting tasks..." overlay first so the user knows
    // the action is in flight and the UI is intentionally frozen.
    if (pendingReset) {
        Serial.println("[RESET] main loop entering reset branch");
        pendingReset = false;
        Serial.println("[RESET] showing overlay");
        showResetOverlay();
        Serial.println("[RESET] overlay shown, calling resetAllTasks");
        resetAllTasks(updateResetProgress);
        Serial.println("[RESET] resetAllTasks returned, refetching");
        tabTotalsInitialized = false;
        fetchSections();
        fetchTasks();
        activeTab = 0;
        Serial.println("[RESET] rebuilding UI");
        buildUI();
        lastRefresh = millis();
        Serial.println("[RESET] done");
    }

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

    // Process ONE queued reward-bank POST per loop (non-blocking)
    if (bankQueueCount > 0 && !pendingRefresh) {
        addBankedReward(bankQueue[0]);
        for (int i = 1; i < bankQueueCount; i++)
            bankQueue[i - 1] = bankQueue[i];
        bankQueueCount--;
        // Refresh so the new banked reward shows up in the bank tab
        lastRefresh = 0;
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
        analogWrite(21, 0);
        screenOff = true;
        screenDimmed = false;
    } else if (!screenDimmed && !screenOff && idle > DIM_TIMEOUT) {
        analogWrite(21, 30);
        screenDimmed = true;
    }

    delay(5);
}
