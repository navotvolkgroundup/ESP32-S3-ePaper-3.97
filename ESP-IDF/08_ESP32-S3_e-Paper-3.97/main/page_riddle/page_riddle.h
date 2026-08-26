#ifndef PAGE_RIDDLE_H
#define PAGE_RIDDLE_H

#ifdef __cplusplus
extern "C" {
#endif

// Menu tile. Read-only with respect to the daily state: opening it never
// consumes a riddle, breaks a streak, or reveals an answer.
void page_riddle_show(void);

// Ambient entry point, for the scheduled-wake dispatcher in app_main.
// `reason` is a wake_reason_e from riddle_decide.h. Draws, persists, returns;
// the caller owns powering the board back off.
void page_riddle_ambient(int reason);

// Wake log screen: the last 14 wakes with outcome, battery and stack headroom.
// The board is off ~99.98% of the time and ESP_LOG dies with the rail, so this
// is the only way to tell a working device from one whose alarm never armed.
void page_riddle_diagnostics(void);

#ifdef __cplusplus
}
#endif

#endif // PAGE_RIDDLE_H
