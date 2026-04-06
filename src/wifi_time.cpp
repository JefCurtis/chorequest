#include "wifi_time.h"
#include <WiFi.h>
#include <time.h>
#include "secrets.h"

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
