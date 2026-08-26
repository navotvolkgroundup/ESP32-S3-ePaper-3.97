// Scheduled-wake plumbing for the ambient pages.
//
// SCOPE, deliberately narrow (eng review D4). This owns ARMING only. Dispatch
// already exists: app_main has switched on the `mode` byte since the vendor
// wrote it -- 1 clock, 2 calendar, 3 weather -- and it does so after EPD_Init()
// and the Image_Mono allocation, which is exactly where an ambient draw needs
// to be. The riddle is a fourth branch there, not a second dispatcher. Two
// places deciding what happens on boot is how the vendor's own modes quietly
// stop working.
//
// The decision maths is NOT here either. riddle_next_wake() in riddle_decide.c
// is IDF-free and host-tested, including the October DST boundary. This file
// is the part that touches hardware: read the clock, arm the RTC, verify it,
// and decide whether powering off is safe.
//
// WHY ARMING NEEDS AN OWNER. The PCF85063 has exactly one alarm register set,
// and page_clock (modes 1 and 2) and page_weather (mode 3) already write it.
// Adding the riddle makes three claimants for one register, so whoever arms
// last silently wins. sched_arm_next() is the single place that resolves that.

#ifndef SCHED_H
#define SCHED_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// `mode` values, the vendor's dispatch key. 1-3 are theirs; 4 is ours.
#define SCHED_MODE_NONE    0
#define SCHED_MODE_RIDDLE  4

// Current instant as UTC seconds, derived from the PCF85063. The RTC holds
// LOCAL wall time (vendor convention), so this is the one place the two
// representations meet.
time_t sched_now_utc(void);

// Is ambient mode switched on? Defaults to FALSE, and that default is
// load-bearing: shipping it off means the first flash cannot leave the board
// powering itself down before anyone has confirmed it wakes again (14A).
bool sched_ambient_enabled(void);
void sched_set_ambient_enabled(bool on);

// Which slot woke us -- WAKE_MORNING or WAKE_AFTERNOON from riddle_decide.h.
// Recorded when the alarm was armed rather than inferred from the clock now,
// because "which side of 16:00 are we on" is ambiguous exactly at the boundary
// and after a slow boot.
uint8_t sched_wake_slot(void);

// Computes the next 06:30/16:00, arms the RTC, verifies the write, and records
// the slot and mode so the next boot dispatches correctly.
//
// Returns false if the alarm could not be verified after retries. A false
// return means DO NOT POWER OFF: the board would never wake, and the panel
// would keep holding its last image, so nothing would look wrong until someone
// noticed the joke had gone stale (3A).
bool sched_arm_next(void);

// Powers the board down, but only when that is safe:
//   - ambient mode is on, and
//   - the next alarm verified, and
//   - USB is NOT attached.
//
// The USB guard is why this exists rather than a bare axp_pwr_off() call. A
// cabled board must stay reachable; a board that switches itself off mid-flash
// is the trap this whole feature could have walked into. Note the guard lives
// HERE, on the arming side -- main.cc:471 is a different thing (charge without
// booting) and must be left alone.
//
// Returns only if it decided NOT to power off.
void sched_power_off_if_safe(void);

#ifdef __cplusplus
}
#endif

#endif // SCHED_H
