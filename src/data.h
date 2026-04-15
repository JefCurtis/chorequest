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

struct BankedReward {
    String id;
    String content;   // display text, with date prefix stripped
};

// A reward in the pool with a weight derived from its Todoist priority.
// Higher weight = more likely to be picked on any given day.
//   Todoist p1 (red, urgent)  -> weight 5
//   Todoist p2 (orange, high) -> weight 3
//   Todoist p3 (blue, medium) -> weight 2
//   Todoist p4 (default)      -> weight 1
struct Reward {
    String content;
    int weight;
};

// ===== CONSTANTS =====

static const int MAX_TASKS = 25;
static const int MAX_SECTIONS = 4;
static const int MAX_REWARDS = 30;
static const int MAX_BANKED = 30;
static const int MAX_QUEUE = 10;
static const int MAX_BANK_QUEUE = 5;

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

extern Reward rewards[MAX_REWARDS];
extern int rewardCount;
extern String rewardsSectionId;

extern BankedReward bankedRewards[MAX_BANKED];
extern int bankedCount;
extern String rewardBankSectionId;

extern String completeQueue[MAX_QUEUE];
extern int queueCount;

// Queue of reward text strings to POST to the Reward Bank section.
// Entries already include the "YYYY-MM-DD: " date prefix for dedupe.
extern String bankQueue[MAX_BANK_QUEUE];
extern int bankQueueCount;

extern unsigned long lastRefresh;
extern unsigned long lastComplete;
extern unsigned long lastActivity;
extern bool screenDimmed;
extern bool screenOff;
extern bool pendingRefresh;
extern bool pendingWake;
extern bool pendingReset;
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
