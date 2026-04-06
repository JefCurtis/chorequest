#pragma once
#include <Arduino.h>

void fetchSections();
void fetchTasks();
void sendComplete(const String& taskId);
void resetAllTasks();
