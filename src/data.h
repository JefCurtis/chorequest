#pragma once
#include <Arduino.h>
#include <lvgl.h>

// ===== DATA STRUCTURES =====

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

// ===== CONSTANTS =====

static const int MAX_TASKS = 20;
static const int MAX_SECTIONS = 4;
static const int MAX_REWARDS = 15;
static const int MAX_QUEUE = 10;

extern const unsigned long DIM_TIMEOUT;
extern const unsigned long OFF_TIMEOUT;
extern const unsigned long REFRESH_INTERVAL;
extern const unsigned long COMPLETE_DEBOUNCE;

// ===== GLOBAL STATE =====

extern Task allTasks[MAX_TASKS];
extern int allTaskCount;
extern Section sections[MAX_SECTIONS];
extern int sectionCount;
extern int activeTab;

extern String rewards[MAX_REWARDS];
extern int rewardCount;
extern String rewardsSectionId;

extern String completeQueue[MAX_QUEUE];
extern int queueCount;

extern unsigned long lastRefresh;
extern unsigned long lastComplete;
extern unsigned long lastActivity;
extern bool screenDimmed;
extern bool screenOff;
extern bool pendingRefresh;
extern bool pendingWake;
extern String lastKnownDate;

// ===== LVGL WIDGETS =====

extern lv_obj_t* tabview;
extern lv_obj_t* tabPages[MAX_SECTIONS];
extern lv_obj_t* tabCounterLabels[MAX_SECTIONS];
extern lv_obj_t* tabProgressBars[MAX_SECTIONS];
extern int tabTotalCounts[MAX_SECTIONS];
extern int tabCompletedCounts[MAX_SECTIONS];
extern int tabOriginalTotals[MAX_SECTIONS];
extern bool tabTotalsInitialized;

// ===== UTILITIES =====

bool isWeeklySection(int tab);
uint32_t getProgressColor(int completed, int total);
