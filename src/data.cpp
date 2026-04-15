#include "data.h"

// ===== GLOBAL STATE =====

Task allTasks[MAX_TASKS];
int allTaskCount = 0;
Section sections[MAX_SECTIONS];
int sectionCount = 0;
int activeTab = 0;

String rewards[MAX_REWARDS];
int rewardCount = 0;
String rewardsSectionId = "";

BankedReward bankedRewards[MAX_BANKED];
int bankedCount = 0;
String rewardBankSectionId = "";

String completeQueue[MAX_QUEUE];
int queueCount = 0;

String bankQueue[MAX_BANK_QUEUE];
int bankQueueCount = 0;

unsigned long lastRefresh = 0;
unsigned long lastComplete = 0;
unsigned long lastActivity = 0;
bool screenDimmed = false;
bool screenOff = false;
bool pendingRefresh = false;
bool pendingWake = false;
bool pendingReset = false;
String lastKnownDate = "";

const unsigned long DIM_TIMEOUT = 300000;
const unsigned long OFF_TIMEOUT = 600000;
const unsigned long REFRESH_INTERVAL = 60000;
const unsigned long COMPLETE_DEBOUNCE = 2000;

// ===== LVGL WIDGETS =====

lv_obj_t* tabview = NULL;
lv_obj_t* tabPages[MAX_SECTIONS];
lv_obj_t* tabCounterLabels[MAX_SECTIONS];
lv_obj_t* tabProgressBars[MAX_SECTIONS];
int tabTotalCounts[MAX_SECTIONS];
int tabCompletedCounts[MAX_SECTIONS];
int tabOriginalTotals[MAX_SECTIONS];
bool tabTotalsInitialized = false;

// ===== UTILITIES =====

bool isWeeklySection(int tab) {
    if (tab < 0 || tab >= sectionCount) return false;
    String name = sections[tab].name;
    name.toLowerCase();
    return name.indexOf("week") >= 0 || name.indexOf("chore") >= 0;
}

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
