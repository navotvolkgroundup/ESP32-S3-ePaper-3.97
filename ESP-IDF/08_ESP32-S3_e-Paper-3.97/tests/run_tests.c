// Host tests for the Morning Riddle decision core.
//
//   cc -std=c99 -Wall -Wextra -I main/page_riddle \
//      tests/run_tests.c main/page_riddle/riddle_decide.c -o /tmp/rt && /tmp/rt
//
// or just: make test
//
// No framework and no fixtures on purpose. The value here is that these run in
// milliseconds on a Mac, against a board that has been reachable for about 11
// minutes of the last three hours. The DST block below is the reason this file
// exists: that bug is invisible until 2026-10-25 and then the riddle silently
// arrives an hour after the kids have left.

#include "riddle_decide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0;
#define CHECK(cond) do { checks++; if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
    return 1; } } while (0)

// A UTC instant, spelled out, so no epoch magic numbers appear below.
static time_t utc_at(int y, int mo, int d, int h, int mi)
{
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d;
    t.tm_hour = h; t.tm_min = mi;
    return timegm(&t);
}

// ------------------------------------------------------------------- DST ---
static int test_dst(void)
{
    int morning = -1;

    // Friday 2026-10-23, still IDT (UTC+3). 00:00Z is 03:00 local, before the
    // 06:30 slot, so the next wake is that morning: 06:30 IDT == 03:30Z.
    time_t got = riddle_next_wake(utc_at(2026, 10, 23, 0, 0), RIDDLE_TZ, &morning);
    CHECK(got == utc_at(2026, 10, 23, 3, 30));
    CHECK(morning == 1);

    // Monday 2026-10-26, IDT has ended (M10.5.0 -> last Sunday, the 25th), so
    // Israel is back on UTC+2. The SAME 06:30 wall time is now 04:30Z.
    got = riddle_next_wake(utc_at(2026, 10, 26, 0, 0), RIDDLE_TZ, &morning);
    CHECK(got == utc_at(2026, 10, 26, 4, 30));
    CHECK(morning == 1);

    // The point, stated as an assertion: a schedule pinned to a fixed UTC
    // offset would fire at the same instant on both days. It must not.
    CHECK(utc_at(2026, 10, 23, 3, 30) != utc_at(2026, 10, 26, 3, 30));

    // Spring forward, for symmetry: M3.4.4/26 puts the change at 02:00 on the
    // Friday after the 4th Thursday of March 2026 (the 27th).
    got = riddle_next_wake(utc_at(2026, 3, 26, 0, 0), RIDDLE_TZ, &morning);
    CHECK(got == utc_at(2026, 3, 26, 4, 30));      // still IST, UTC+2
    got = riddle_next_wake(utc_at(2026, 3, 30, 0, 0), RIDDLE_TZ, &morning);
    CHECK(got == utc_at(2026, 3, 30, 3, 30));      // IDT, UTC+3
    return 0;
}

// -------------------------------------------------------------- schedule ---
static int test_schedule(void)
{
    int morning = -1;

    // Mid-summer (IDT, UTC+3): 05:00Z is 08:00 local, past 06:30, so 16:00.
    time_t got = riddle_next_wake(utc_at(2026, 7, 1, 5, 0), RIDDLE_TZ, &morning);
    CHECK(got == utc_at(2026, 7, 1, 13, 0));       // 16:00 IDT
    CHECK(morning == 0);

    // After the afternoon slot, roll to tomorrow morning.
    got = riddle_next_wake(utc_at(2026, 7, 1, 14, 0), RIDDLE_TZ, &morning);
    CHECK(got == utc_at(2026, 7, 2, 3, 30));
    CHECK(morning == 1);

    // Exactly on the morning instant: strictly-after, so we get 16:00, not the
    // same second again. An alarm that re-arms for now would spin.
    got = riddle_next_wake(utc_at(2026, 7, 1, 3, 30), RIDDLE_TZ, &morning);
    CHECK(got == utc_at(2026, 7, 1, 13, 0));

    // Year boundary: the day key must keep increasing across it.
    int32_t d31 = riddle_local_day(utc_at(2026, 12, 31, 12, 0), RIDDLE_TZ);
    int32_t d01 = riddle_local_day(utc_at(2027, 1, 1, 12, 0), RIDDLE_TZ);
    CHECK(d01 == d31 + 1);

    // 23:00Z in winter is already the next local day (UTC+2).
    int32_t a = riddle_local_day(utc_at(2026, 12, 1, 21, 0), RIDDLE_TZ);
    int32_t b = riddle_local_day(utc_at(2026, 12, 1, 23, 0), RIDDLE_TZ);
    CHECK(b == a + 1);

    // TZ is a process global; the core must put it back as it found it.
    setenv("TZ", "UTC", 1); tzset();
    (void)riddle_next_wake(utc_at(2026, 7, 1, 5, 0), RIDDLE_TZ, NULL);
    CHECK(strcmp(getenv("TZ"), "UTC") == 0);
    return 0;
}

// --------------------------------------------------------- state machine ---
static riddle_input_t IN(int reason, int32_t today, int guess, uint16_t n)
{
    riddle_input_t in;
    in.reason = (uint8_t)reason; in.today = today;
    in.guess = (int8_t)guess;    in.batch_n = n;
    return in;
}

static int test_state(void)
{
    riddle_nvs_t st;
    memset(&st, 0, sizeof st);
    st.state = RS_IDLE; st.guess = RIDDLE_NO_GUESS;

    // First morning shows riddle 0, not 1.
    CHECK(riddle_decide(&(riddle_input_t){WAKE_MORNING, RIDDLE_NO_GUESS, 30, 100},
                        &st) == ACT_SHOW_QUESTION);
    CHECK(st.idx == 0 && st.state == RS_QUESTION_SHOWN && st.day == 100);

    // A second 06:30 on the same day redraws without consuming a riddle.
    riddle_input_t again = IN(WAKE_MORNING, 100, RIDDLE_NO_GUESS, 30);
    CHECK(riddle_decide(&again, &st) == ACT_SHOW_QUESTION);
    CHECK(st.idx == 0);

    // A guess: records, gives feedback, and bumps the streak once.
    riddle_input_t g = IN(WAKE_GUESS, 100, 2, 30);
    CHECK(riddle_decide(&g, &st) == ACT_SHOW_RESULT);
    CHECK(st.state == RS_GUESSED && st.guess == 2 && st.streak == 1);

    // Extra presses are ignored -- one physical press emits three button
    // codes, which is the bug that shipped in page_news earlier today.
    CHECK(riddle_decide(&g, &st) == ACT_NONE);
    CHECK(st.streak == 1);

    // 16:00 reveals, knowing a guess was made.
    riddle_input_t pm = IN(WAKE_AFTERNOON, 100, RIDDLE_NO_GUESS, 30);
    CHECK(riddle_decide(&pm, &st) == ACT_SHOW_ANSWER);
    CHECK(st.state == RS_ANSWER_SHOWN);
    CHECK(riddle_decide(&pm, &st) == ACT_NONE);        // idempotent

    // A guess arriving after the reveal changes nothing.
    CHECK(riddle_decide(&g, &st) == ACT_NONE);
    CHECK(st.guess == 2);

    // Next day: advances, and the streak survives because yesterday counted.
    riddle_input_t d2 = IN(WAKE_MORNING, 101, RIDDLE_NO_GUESS, 30);
    CHECK(riddle_decide(&d2, &st) == ACT_SHOW_QUESTION);
    CHECK(st.idx == 1 && st.streak == 1 && st.guess == RIDDLE_NO_GUESS);

    // Skip a day without guessing -> streak resets.
    riddle_input_t d4 = IN(WAKE_MORNING, 103, RIDDLE_NO_GUESS, 30);
    CHECK(riddle_decide(&d4, &st) == ACT_SHOW_QUESTION);
    CHECK(st.streak == 0 && st.idx == 2);
    return 0;
}

static int test_state_edges(void)
{
    riddle_nvs_t st;

    // Participation, not accuracy: a WRONG guess still keeps the run alive.
    memset(&st, 0, sizeof st);
    st.state = RS_QUESTION_SHOWN; st.day = 200; st.last_played_day = 199;
    st.streak = 5; st.guess = RIDDLE_NO_GUESS;
    riddle_input_t wrong = IN(WAKE_GUESS, 200, 0, 30);   // whatever the answer is
    CHECK(riddle_decide(&wrong, &st) == ACT_SHOW_RESULT);
    CHECK(st.streak == 6);

    // Queue wraps at the end rather than going blank (CEO 7A).
    memset(&st, 0, sizeof st);
    st.state = RS_QUESTION_SHOWN; st.idx = 29; st.day = 300;
    riddle_input_t nxt = IN(WAKE_MORNING, 301, RIDDLE_NO_GUESS, 30);
    CHECK(riddle_decide(&nxt, &st) == ACT_SHOW_QUESTION);
    CHECK(st.idx == 0);

    // An empty batch must not divide by zero.
    memset(&st, 0, sizeof st);
    st.state = RS_QUESTION_SHOWN; st.idx = 3; st.day = 300;
    riddle_input_t empty = IN(WAKE_MORNING, 301, RIDDLE_NO_GUESS, 0);
    CHECK(riddle_decide(&empty, &st) == ACT_SHOW_QUESTION);
    CHECK(st.idx == 3);

    // 16:00 with no morning at all (board was off): show the riddle, not an
    // orphan answer to a question nobody saw.
    memset(&st, 0, sizeof st);
    st.state = RS_IDLE; st.guess = RIDDLE_NO_GUESS;
    riddle_input_t orphan = IN(WAKE_AFTERNOON, 400, RIDDLE_NO_GUESS, 30);
    CHECK(riddle_decide(&orphan, &st) == ACT_SHOW_QUESTION);
    CHECK(st.state == RS_QUESTION_SHOWN && st.day == 400);

    // Yesterday's screen still up when the afternoon fires: same rule.
    memset(&st, 0, sizeof st);
    st.state = RS_ANSWER_SHOWN; st.day = 399; st.idx = 4;
    riddle_input_t stale = IN(WAKE_AFTERNOON, 400, RIDDLE_NO_GUESS, 30);
    CHECK(riddle_decide(&stale, &st) == ACT_SHOW_QUESTION);
    CHECK(st.idx == 5 && st.day == 400);

    // Reveal-early from the question, then again -> no-op.
    memset(&st, 0, sizeof st);
    st.state = RS_QUESTION_SHOWN; st.day = 500;
    riddle_input_t rv = IN(WAKE_REVEAL, 500, RIDDLE_NO_GUESS, 30);
    CHECK(riddle_decide(&rv, &st) == ACT_SHOW_ANSWER);
    CHECK(riddle_decide(&rv, &st) == ACT_NONE);

    // The menu is read-only: no advance, no reveal, no streak change.
    memset(&st, 0, sizeof st);
    st.state = RS_QUESTION_SHOWN; st.idx = 7; st.day = 600;
    st.streak = 3; st.last_played_day = 599;
    riddle_nvs_t before = st;
    riddle_input_t menu = IN(WAKE_MENU, 601, RIDDLE_NO_GUESS, 30);
    CHECK(riddle_decide(&menu, &st) == ACT_SHOW_QUESTION);
    CHECK(memcmp(&before, &st, sizeof st) == 0);

    // A guess against yesterday's screen is refused.
    memset(&st, 0, sizeof st);
    st.state = RS_QUESTION_SHOWN; st.day = 700; st.streak = 2;
    riddle_input_t late = IN(WAKE_GUESS, 701, 1, 30);
    CHECK(riddle_decide(&late, &st) == ACT_NONE);
    CHECK(st.streak == 2);

    // CONTRACT, load-bearing: WAKE_MENU does NOT advance a stale day. It is
    // deliberately read-only, so on a day the 06:30 wake never ran it shows
    // yesterday's riddle and then refuses guesses against it. Callers must
    // detect the stale day themselves and pass WAKE_MORNING instead -- which
    // is what page_riddle_show() does, and it matters because ambient mode
    // ships defaulting OFF, making the menu tile the only way in.
    // Do not "fix" this by auto-advancing here; that would let opening the
    // tile consume a riddle and silently break the ritual.
    memset(&st, 0, sizeof st);
    st.state = RS_QUESTION_SHOWN; st.day = 800; st.idx = 9;
    riddle_input_t stale_menu = IN(WAKE_MENU, 801, RIDDLE_NO_GUESS, 30);
    CHECK(riddle_decide(&stale_menu, &st) == ACT_SHOW_QUESTION);
    CHECK(st.day == 800 && st.idx == 9);          // untouched, on purpose
    riddle_input_t refused = IN(WAKE_GUESS, 801, 1, 30);
    CHECK(riddle_decide(&refused, &st) == ACT_NONE);
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "dst",          test_dst },
        { "schedule",     test_schedule },
        { "state",        test_state },
        { "state_edges",  test_state_edges },
    };
    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        if (tests[i].fn()) {
            fprintf(stderr, "\n%s FAILED\n", tests[i].name);
            return 1;
        }
        printf("  ok  %s\n", tests[i].name);
    }
    printf("PASS: %d checks\n", checks);
    return 0;
}
