// Shared helpers for the full-screen pages.
//
// Extracted from page_news because page_riddle needs all three verbatim, and
// the button classifier in particular carries a fix that cost a real debugging
// session (see pc_button_classify). Two copies of that means the next fix
// lands in one of them and silently not the other. (Eng review 11A.)

#ifndef PAGE_COMMON_H
#define PAGE_COMMON_H

#include <stddef.h>
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------- buttons ----

// What a press MEANS, so pages stop switching on raw event codes.
//
// The raw codes are regular: each button owns seven consecutive codes from its
// base -- click, double-click, press-down, bounce-up, repeat, long-press-once,
// long-press-hold -- with bases Up=0, Function=7, Down=14, Boot=21. Boot only
// implements click, double-click and long-hold; the rest are commented out in
// button_bsp.c.
typedef enum {
    PC_BTN_TIMEOUT  = -1,   // nothing arrived before the timeout
    PC_BTN_IGNORE   = 0,    // a real event, but not a decision
    PC_BTN_UP,              // Up click
    PC_BTN_DOWN,            // Down click
    PC_BTN_SELECT,          // Function click
    PC_BTN_BACK,            // Function or Boot double-click
    PC_BTN_POWER,           // Boot click
    PC_BTN_REFRESH,         // Function long-press-once (full panel refresh)
    PC_BTN_SETTINGS,        // Boot long-press-hold
} pc_button_t;

// Maps one raw code to its meaning. Pure; safe to unit-test.
//
// WHY THIS EXISTS AT ALL: one physical press emits SEVERAL codes. Pressing
// Function once fires 9 (press-down), 7 (click) and 10 (bounce-up), in that
// order. A page with a catch-all "any other button" branch therefore acts two
// or three times per press. That shipped in the news reader on 2026-08-26 and
// presented as "the feed toggle immediately jumps back" -- it was toggling
// twice. Everything that is not a decision maps to PC_BTN_IGNORE, on purpose.
pc_button_t pc_button_classify(int code);

// Blocks until a MEANINGFUL press arrives, discarding PC_BTN_IGNORE events.
// Returns PC_BTN_TIMEOUT if `timeout` elapses first; pass portMAX_DELAY to
// wait forever. This is the loop every page was writing by hand.
pc_button_t pc_button_wait(TickType_t timeout);

// ------------------------------------------------------------- fetching ----

typedef enum {
    PC_FETCH_OK = 0,
    PC_FETCH_TRUNCATED,     // body exceeded `cap`; buf holds a valid prefix
    PC_FETCH_SHORT,         // fewer bytes arrived than Content-Length promised
    PC_FETCH_TRANSPORT,     // DNS / TLS / connection failure
    PC_FETCH_HTTP_STATUS,   // reached the server, got a non-200
} pc_fetch_status_t;

// GETs `url` over HTTPS into `buf` (caller-allocated, `cap` bytes) and
// NUL-terminates it. Writes the byte count to `out_len` when non-NULL.
//
// TRUNCATED and SHORT are reported rather than swallowed. The original
// page_news handler dropped overflow bytes and returned ESP_OK, so a partial
// download looked identical to a complete one -- harmless for a news feed that
// just loses tail headlines, destructive for a riddle batch that would
// overwrite a good queue with a broken one. Callers decide which they are.
pc_fetch_status_t pc_fetch_url(const char *url, char *buf, size_t cap,
                               size_t *out_len);

// Human-readable form of a status, for logs and on-screen messages.
const char *pc_fetch_strerror(pc_fetch_status_t s);

// -------------------------------------------------------------- drawing ----

// Pushes the current Image_Mono to the panel (partial refresh, whole area).
void pc_refresh(void);

// Full-screen two-line status card. `l2` may be NULL.
void pc_draw_message(const char *l1, const char *l2);

#ifdef __cplusplus
}
#endif

#endif // PAGE_COMMON_H
