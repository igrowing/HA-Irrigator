#ifndef STATE_MANAGER_H
#define STATE_MANAGER_H

#include <Arduino.h>

// --- System State Enum ---
enum SystemState {
  STATE_INITIAL = 0,
  STATE_RUNNING,
  STATE_PAUSED,
  STATE_ENDED
};

static const char *SYSTEM_STATE_STRING[] = {
    "Initial", "Running", "Paused", "Ended"
};

// --- State Variables ---
extern SystemState currentState;
extern unsigned long runStartTime;
extern unsigned long pausedAtTime;
extern unsigned long totalPausedTime;

// --- State Functions ---
void changeStateAndReport(SystemState next);

#endif // STATE_MANAGER_H
