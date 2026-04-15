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
            rewardBankSectionId = "";
            for (JsonObject obj : arr) {
                String name = obj["name"].as<String>();
                String id   = obj["id"].as<String>();
                // Skip Rewards section from tabs
                if (name == "Rewards") {
                    rewardsSectionId = id;
                    continue;
                }
                // Skip Reward Bank from regular tabs too — it gets its own
                // special-case render path in ui.cpp.
                if (name == "Reward Bank") {
                    rewardBankSectionId = id;
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
            bankedCount = 0;
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

                // Reward Bank entries go to their own array. Content is
                // stored with the "YYYY-MM-DD: " prefix intact; the UI
                // strips it on display.
                if (sid == rewardBankSectionId && rewardBankSectionId.length() > 0) {
                    if (bankedCount < MAX_BANKED) {
                        bankedRewards[bankedCount].id      = obj["id"].as<String>();
                        bankedRewards[bankedCount].content = obj["content"].as<String>();
                        bankedCount++;
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

void addBankedReward(const String& content) {
    if (WiFi.status() != WL_CONNECTED) return;
    if (rewardBankSectionId.length() == 0) return;  // no Reward Bank section set up

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://api.todoist.com/api/v1/tasks");
    http.addHeader("Authorization", "Bearer " TODOIST_API_TOKEN);
    http.addHeader("Content-Type", "application/json");

    // Escape any double quotes in content to keep the JSON valid.
    String safe = content;
    safe.replace("\"", "\\\"");

    String body = "{\"project_id\":\"" TODOIST_PROJECT_ID "\","
                  "\"section_id\":\"" + rewardBankSectionId + "\","
                  "\"content\":\"" + safe + "\"}";
    http.POST(body);
    http.end();
}

void resetAllTasks(ResetProgressCb progressCb) {
    Serial.println("[RESET] resetAllTasks entry");
    Serial.printf("[RESET] free heap at start: %u\n", ESP.getFreeHeap());
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[RESET] WiFi not connected, aborting");
        return;
    }

    // Build list of weekly section IDs
    String weeklySectionIds[MAX_SECTIONS];
    int weeklyCount = 0;
    for (int i = 0; i < sectionCount; i++) {
        if (isWeeklySection(i)) {
            weeklySectionIds[weeklyCount++] = sections[i].id;
        }
    }
    Serial.printf("[RESET] weeklyCount=%d, sectionCount=%d\n", weeklyCount, sectionCount);

    String today = getTodayDate();
    Serial.print("[RESET] today=");
    Serial.println(today);

    // Collect the list of tasks to reset. Scope the fetch client tightly
    // so its SSL context is released before we start the PATCH phase —
    // the ESP32's mbedTLS allocator is very sensitive to fragmentation
    // and cannot establish a second SSL session if a previous one is
    // still alive in a nearby scope.
    struct ResetInfo { String id; String dueString; };
    ResetInfo toReset[MAX_TASKS];
    int resetCount = 0;
    int totalTasks = 0;

    {
        WiFiClientSecure fetchClient;
        fetchClient.setInsecure();
        HTTPClient http;
        http.begin(fetchClient, "https://api.todoist.com/api/v1/tasks?project_id=" TODOIST_PROJECT_ID);
        http.addHeader("Authorization", "Bearer " TODOIST_API_TOKEN);
        int code = http.GET();
        Serial.printf("[RESET] GET tasks returned %d\n", code);
        if (code != 200) {
            http.end();
            Serial.println("[RESET] resetAllTasks aborted due to non-200 GET");
            return;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, http.getString());
        http.end();
        if (err) {
            Serial.println("[RESET] JSON parse failed");
            return;
        }

        JsonArray arr = doc["results"].as<JsonArray>();
        for (JsonObject obj : arr) {
            totalTasks++;
            String sid = obj["section_id"].as<String>();
            if (sid == "null") sid = "";
            String tid = obj["id"].as<String>();

            // Skip rewards, banked rewards, and unsectioned tasks
            if (sid == rewardsSectionId || sid == rewardBankSectionId || sid.length() == 0) continue;

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
        // fetchClient destroyed here as it goes out of scope
    }

    Serial.printf("[RESET] total tasks=%d, toReset=%d\n", totalTasks, resetCount);
    if (progressCb) progressCb(0, resetCount);
    delay(200);  // let RTOS clean up the released SSL context
    Serial.printf("[RESET] free heap after GET scope: %u\n", ESP.getFreeHeap());

    // PATCH phase — reuse a single WiFiClientSecure and HTTPClient across
    // all tasks with keep-alive so we only establish SSL once. This is
    // critical on ESP32: creating one SSL context per task fragments the
    // heap and fails after the first success.
    WiFiClientSecure patchClient;
    patchClient.setInsecure();
    patchClient.setTimeout(5000);

    HTTPClient http;
    http.setReuse(true);
    http.setTimeout(5000);

    int successCount = 0;
    for (int i = 0; i < resetCount; i++) {
        http.begin(patchClient, "https://api.todoist.com/api/v1/tasks/" + toReset[i].id);
        http.addHeader("Authorization", "Bearer " TODOIST_API_TOKEN);
        http.addHeader("Content-Type", "application/json");
        String body = "{\"due_date\":\"" + today + "\",\"due_string\":\"" + toReset[i].dueString + "\",\"due_lang\":\"en\"}";
        int code = http.POST(body);
        http.end();  // with setReuse(true) this doesn't drop the underlying TCP/SSL session
        bool ok = (code == 200);
        if (ok) successCount++;
        Serial.printf("[RESET] task %d/%d id=%s code=%d ok=%d heap=%u\n",
                      i + 1, resetCount, toReset[i].id.c_str(), code, ok, ESP.getFreeHeap());
        if (progressCb) progressCb(i + 1, resetCount);
        delay(50);  // brief pacing
    }

    Serial.printf("[RESET] resetAllTasks done — %d/%d succeeded\n", successCount, resetCount);
}
