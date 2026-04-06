#include "todoist.h"
#include "data.h"
#include "wifi_time.h"
#include "secrets.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

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

// Helper to reset a single task with retry
static bool resetSingleTask(const String& tid, const String& dueString, const String& today) {
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
