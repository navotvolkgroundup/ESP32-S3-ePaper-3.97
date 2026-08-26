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
#include "wake_log.h"
#include "kids.h"
#include "weather.h"

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

// -------------------------------------------------------------- wake ring ---
static wake_rec_t REC(uint32_t when, uint8_t outcome, uint8_t flags)
{
    wake_rec_t r;
    memset(&r, 0, sizeof r);
    r.when = when; r.outcome = outcome; r.flags = flags; r.battery = -1;
    return r;
}

static int test_wake_ring(void)
{
    wake_ring_t ring;
    wake_rec_t out[WAKE_LOG_N];
    memset(&ring, 0, sizeof ring);

    CHECK(wake_ring_read(&ring, out, WAKE_LOG_N) == 0);      // empty

    // Newest first, which is the property the screen depends on.
    for (uint32_t i = 1; i <= 3; i++) {
        wake_rec_t r = REC(i * 1000, WO_OK, 0);
        wake_ring_push(&ring, &r);
    }
    CHECK(wake_ring_read(&ring, out, WAKE_LOG_N) == 3);
    CHECK(out[0].when == 3000 && out[1].when == 2000 && out[2].when == 1000);

    // Overfill by five: count saturates and the oldest five are gone.
    memset(&ring, 0, sizeof ring);
    for (uint32_t i = 1; i <= WAKE_LOG_N + 5; i++) {
        wake_rec_t r = REC(i, WO_OK, 0);
        wake_ring_push(&ring, &r);
    }
    int n = wake_ring_read(&ring, out, WAKE_LOG_N);
    CHECK(n == WAKE_LOG_N);
    CHECK(out[0].when == (uint32_t)(WAKE_LOG_N + 5));        // newest
    CHECK(out[n - 1].when == 6);                             // oldest surviving
    for (int i = 1; i < n; i++) CHECK(out[i].when < out[i - 1].when);

    // A short read must still give the newest, not the oldest.
    CHECK(wake_ring_read(&ring, out, 3) == 3);
    CHECK(out[0].when == (uint32_t)(WAKE_LOG_N + 5));

    // A corrupt blob resets rather than indexing off the end.
    memset(&ring, 0, sizeof ring);
    ring.head = 200; ring.count = 200;
    CHECK(!wake_ring_valid(&ring));
    CHECK(wake_ring_read(&ring, out, WAKE_LOG_N) == 0);
    wake_rec_t r = REC(42, WO_OK, 0);
    wake_ring_push(&ring, &r);                               // must not crash
    CHECK(wake_ring_valid(&ring));
    CHECK(wake_ring_read(&ring, out, WAKE_LOG_N) == 1 && out[0].when == 42);
    return 0;
}

static int test_wake_participation(void)
{
    wake_ring_t ring;
    memset(&ring, 0, sizeof ring);
    const uint32_t DAY = 86400;

    // Three consecutive days, guessed on each. Two wakes per day, as the real
    // device produces, so the counter must not double-count a day.
    for (uint32_t d = 10; d <= 12; d++) {
        wake_rec_t am = REC(d * DAY + 6 * 3600, WO_OK, WF_GUESSED);
        wake_rec_t pm = REC(d * DAY + 16 * 3600, WO_OK, WF_GUESSED);
        wake_ring_push(&ring, &am);
        wake_ring_push(&ring, &pm);
    }
    CHECK(wake_ring_recent_guesses(&ring) == 3);

    // A day with no guess ends the run, counted from the newest end.
    memset(&ring, 0, sizeof ring);
    wake_rec_t d10 = REC(10 * DAY, WO_OK, WF_GUESSED);
    wake_rec_t d11 = REC(11 * DAY, WO_OK, 0);            // nobody played
    wake_rec_t d12 = REC(12 * DAY, WO_OK, WF_GUESSED);
    wake_ring_push(&ring, &d10);
    wake_ring_push(&ring, &d11);
    wake_ring_push(&ring, &d12);
    CHECK(wake_ring_recent_guesses(&ring) == 1);

    // The alarm-unset outcome has to be nameable; it is the one failure that
    // is otherwise invisible, so a wrong label defeats the whole screen.
    CHECK(strcmp(wake_outcome_name(WO_ALARM_UNVERIFIED), "ALARM UNSET") == 0);
    CHECK(strcmp(wake_outcome_name(WO_OK), "ok") == 0);
    CHECK(strcmp(wake_outcome_name(200), "?") == 0);
    return 0;
}

// ------------------------------------------------------------------- kids ---
static kids_t KIDS(int n)
{
    kids_t k;
    memset(&k, 0, sizeof k);
    k.count = (uint8_t)n;
    // Deliberately synthetic placeholders, not names. These tests care about
    // selection, not rendering, and this file is in a PUBLIC fork -- real
    // given names here would read as a hint about the actual kids even though
    // the data itself never leaves device NVS.
    const char *names[] = { "AaaA", "BbbB", "CccC", "DddD" };
    const uint8_t mon[]  = { 3, 11, 0, 7 };
    const uint8_t day[]  = { 14, 2, 0, 30 };
    for (int i = 0; i < n && i < KIDS_MAX; i++) {
        snprintf(k.kid[i].name, KID_NAME_MAX, "%s", names[i]);
        k.kid[i].birth_month = mon[i];
        k.kid[i].birth_day   = day[i];
    }
    return k;
}

static int test_kids_empty(void)
{
    // THE load-bearing case: with no data, both features must simply sleep.
    // This is what makes 16A work -- the build ships complete and the kids'
    // details become a config edit rather than a reflash.
    kids_t k;
    memset(&k, 0, sizeof k);
    CHECK(kids_valid(&k));                       // empty is legal, not corrupt
    CHECK(kids_birthday_on(&k, 3, 14) == -1);
    for (int32_t d = 0; d < 100; d++) CHECK(kids_pick_callout(&k, d) == -1);
    CHECK(kids_birthday_on(NULL, 3, 14) == -1);
    CHECK(kids_pick_callout(NULL, 5) == -1);
    return 0;
}

static int test_kids_birthday(void)
{
    kids_t k = KIDS(4);
    CHECK(kids_birthday_on(&k, 3, 14) == 0);
    CHECK(kids_birthday_on(&k, 11, 2) == 1);
    CHECK(kids_birthday_on(&k, 7, 30) == 3);
    CHECK(kids_birthday_on(&k, 3, 15) == -1);    // one day off
    CHECK(kids_birthday_on(&k, 14, 3) == -1);    // month/day transposed

    // A half-filled record (name, no date) never matches a birthday, but must
    // not disqualify the whole blob -- the name is still good for callouts.
    CHECK(k.kid[2].birth_month == 0);
    CHECK(kids_valid(&k));
    CHECK(kids_birthday_on(&k, 0, 0) == -1);

    // Out-of-range input is rejected rather than matched against zeros.
    CHECK(kids_birthday_on(&k, 13, 1) == -1);
    CHECK(kids_birthday_on(&k, 2, 32) == -1);

    // A corrupt blob is treated as absent.
    kids_t bad = KIDS(2);
    bad.count = KIDS_MAX + 3;
    CHECK(!kids_valid(&bad));
    CHECK(kids_birthday_on(&bad, 3, 14) == -1);
    bad = KIDS(2); bad.kid[1].birth_month = 13;
    CHECK(!kids_valid(&bad));
    return 0;
}

static int test_kids_callout(void)
{
    kids_t k = KIDS(2);

    // Deterministic per day. The morning draw, an early reveal and the 16:00
    // draw are three separate calls on one day; if they disagreed, the screen
    // would greet a different kid each time.
    for (int32_t d = 0; d < 500; d++)
        CHECK(kids_pick_callout(&k, d) == kids_pick_callout(&k, d));

    // Fires sometimes, not always, and never names a kid who does not exist.
    int fired = 0;
    for (int32_t d = 0; d < 600; d++) {
        int i = kids_pick_callout(&k, d);
        CHECK(i >= -1 && i < k.count);
        if (i >= 0) fired++;
    }
    CHECK(fired > 0 && fired < 600);

    // Roughly one day in KIDS_CALLOUT_ONE_IN. Loose bounds on purpose: this
    // asserts "a small surprise, not furniture", not an exact distribution.
    CHECK(fired > 600 / (KIDS_CALLOUT_ONE_IN * 2));
    CHECK(fired < 600 / KIDS_CALLOUT_ONE_IN * 2);

    // Both kids get picked over time; it must not lock onto one.
    int seen[KIDS_MAX] = {0};
    for (int32_t d = 0; d < 600; d++) {
        int i = kids_pick_callout(&k, d);
        if (i >= 0) seen[i]++;
    }
    CHECK(seen[0] > 0 && seen[1] > 0);

    // One kid: still valid, always index 0 when it fires.
    kids_t solo = KIDS(1);
    for (int32_t d = 0; d < 200; d++) {
        int i = kids_pick_callout(&solo, d);
        CHECK(i == -1 || i == 0);
    }
    return 0;
}

// ---------------------------------------------------------------- weather ---
//
// The fixture is a REAL open-meteo response captured on 2026-08-26, not a
// hand-written one. Hand-written fixtures encode what you believe the API
// returns; captured ones encode what it actually returns.
static const char *OM_REAL =
"{\"latitude\":32.0625,\"longitude\":34.8125,\"generationtime_ms\":0.089,"
"\"utc_offset_seconds\":10800,\"timezone\":\"Asia/Jerusalem\","
"\"timezone_abbreviation\":\"GMT+3\",\"elevation\":16.0,"
"\"current_units\":{\"time\":\"iso8601\",\"interval\":\"seconds\","
"\"temperature_2m\":\"°C\",\"weather_code\":\"wmo code\"},"
"\"current\":{\"time\":\"2026-08-26T23:00\",\"interval\":900,"
"\"temperature_2m\":26.6,\"weather_code\":0},"
"\"daily_units\":{\"time\":\"iso8601\",\"temperature_2m_max\":\"°C\","
"\"temperature_2m_min\":\"°C\",\"weather_code\":\"wmo code\"},"
"\"daily\":{\"time\":[\"2026-08-26\"],\"temperature_2m_max\":[31.9],"
"\"temperature_2m_min\":[23.7],\"weather_code\":[2]}}";

static int test_weather_parse(void)
{
    weather_t w;
    memset(&w, 0xAA, sizeof w);
    CHECK(weather_parse(OM_REAL, &w));
    CHECK(w.temp_x10 == 266);          // 26.6C
    CHECK(w.hi_x10   == 319);
    CHECK(w.lo_x10   == 237);
    CHECK(w.wmo      == 0);
    CHECK(w.fetched_at == 0);          // caller stamps this, not the parser

    // Garbage must not clobber a good cached reading.
    weather_t good = w;
    CHECK(!weather_parse("{ not json", &good));
    CHECK(good.temp_x10 == 266);
    CHECK(!weather_parse("{}", &good));
    CHECK(good.temp_x10 == 266);
    CHECK(!weather_parse(NULL, &good));
    CHECK(good.temp_x10 == 266);

    // A current block with no daily block is still usable.
    weather_t p;
    memset(&p, 0, sizeof p);
    CHECK(weather_parse("{\"current\":{\"temperature_2m\":-3.55,"
                        "\"weather_code\":71}}", &p));
    CHECK(p.wmo == 71);
    // Rounding half away from zero: a truncating cast would give -35 and put
    // the board a whole tenth warm on a freezing morning.
    CHECK(p.lo_x10 == 0 && p.hi_x10 == 0);
    CHECK(p.temp_x10 == -36);

    // open-meteo emits null in a daily array when a value is unavailable.
    weather_t n;
    memset(&n, 0, sizeof n);
    CHECK(weather_parse("{\"current\":{\"temperature_2m\":5,\"weather_code\":3},"
                        "\"daily\":{\"temperature_2m_max\":[null],"
                        "\"temperature_2m_min\":[]}}", &n));
    CHECK(n.temp_x10 == 50 && n.hi_x10 == 0 && n.lo_x10 == 0);
    return 0;
}

static int test_weather_icons(void)
{
    // Every icon named here must exist in page_weather/Weather_img, including
    // leiyu, which only exists because the lieyu transposition was fixed.
    CHECK(strcmp(wmo_icon(0),  "qin")    == 0);
    CHECK(strcmp(wmo_icon(2),  "duoyun") == 0);
    CHECK(strcmp(wmo_icon(3),  "yin")    == 0);
    CHECK(strcmp(wmo_icon(48), "wumai")  == 0);
    CHECK(strcmp(wmo_icon(61), "xiaoyu") == 0);
    CHECK(strcmp(wmo_icon(63), "zhongyu")== 0);
    CHECK(strcmp(wmo_icon(65), "dayu")   == 0);
    CHECK(strcmp(wmo_icon(82), "baoyu")  == 0);
    CHECK(strcmp(wmo_icon(75), "xiaxue") == 0);
    CHECK(strcmp(wmo_icon(95), "leiyu")  == 0);
    CHECK(strcmp(wmo_icon(99), "leiyu")  == 0);

    // Unknown codes fall back to a plausible cloud, never to nothing. WMO adds
    // codes and this board is not casually reflashable.
    CHECK(strcmp(wmo_icon(7),     "duoyun") == 0);
    CHECK(strcmp(wmo_icon(65535), "duoyun") == 0);
    for (uint32_t c = 0; c <= 120; c++) CHECK(wmo_icon((uint16_t)c) != NULL);
    CHECK(strcmp(wmo_label(0), "clear") == 0);
    CHECK(strcmp(wmo_label(7), "?")     == 0);
    return 0;
}

static int test_weather_staleness(void)
{
    weather_t w;
    memset(&w, 0, sizeof w);

    CHECK(weather_is_stale(&w, 1000));            // never fetched
    CHECK(weather_is_stale(NULL, 1000));

    // The two intervals that actually occur, and the threshold sits between.
    w.fetched_at = 100000;
    CHECK(!weather_is_stale(&w, 100000 + 1));
    CHECK(!weather_is_stale(&w, 100000 + (uint32_t)(9.5 * 3600)));  // 06:30 -> 16:00
    CHECK(weather_is_stale(&w, 100000 + 24 * 3600));                // next morning, fetch failed
    CHECK(!weather_is_stale(&w, 100000 + WEATHER_STALE_SECS));      // boundary is inclusive
    CHECK(weather_is_stale(&w, 100000 + WEATHER_STALE_SECS + 1));

    // A clock that moved backwards means one of the two values is wrong and
    // there is no telling which, so it must not be presented as current. Note
    // this case is ALSO satisfied by unsigned underflow in the subtraction, so
    // this assertion does not distinguish the explicit guard from its absence;
    // the guard is kept for intent, not behaviour. See weather.c.
    CHECK(weather_is_stale(&w, 99999));
    CHECK(weather_is_stale(&w, 0));
    return 0;
}

int main(void)
{
    struct { const char *name; int (*fn)(void); } tests[] = {
        { "dst",             test_dst },
        { "schedule",        test_schedule },
        { "state",           test_state },
        { "state_edges",     test_state_edges },
        { "wake_ring",       test_wake_ring },
        { "wake_participation", test_wake_participation },
        { "kids_empty",      test_kids_empty },
        { "kids_birthday",   test_kids_birthday },
        { "kids_callout",    test_kids_callout },
        { "weather_parse",     test_weather_parse },
        { "weather_icons",     test_weather_icons },
        { "weather_staleness", test_weather_staleness },
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
