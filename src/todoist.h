#pragma once
#include <Arduino.h>

void fetchSections();
void fetchTasks();
void sendComplete(const String& taskId);

// Optional progress callback fired while resetAllTasks runs. Called once
// with (0, total) after the task list is fetched, then once after each
// PATCH with (N, total). `total` is the number of tasks being reset.
typedef void (*ResetProgressCb)(int current, int total);
void resetAllTasks(ResetProgressCb progressCb = nullptr);

// Create a new task inside the "Reward Bank" section. Content should
// already include the "YYYY-MM-DD: " date prefix used for dedupe.
void addBankedReward(const String& content);
