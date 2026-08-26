// Morning Riddle: batch handling and the screen.
//
// The decision logic is NOT here -- it lives in riddle_decide.c, which is
// IDF-free so it can be tested on a Mac. This file is the I/O half: load a
// batch, draw a screen, persist state. Keep it that way; anything that decides
// something belongs next door where `make test` can reach it.
//
// Delivery order on a scheduled wake:
//
//   /sdcard/riddles.json  (if newer)  ──┐
//                                       ├──▶ parse ──▶ validate ──▶ SPIFFS
//   HTTPS release asset (if queue low) ─┘                (tmp + rename)
//                                                              │
//   /spiffs/riddles.json  ◀─────────────────────────────────────┘
//
// Every failure along that path leaves the previous queue untouched and shows
// yesterday's riddle. A blank wall reads as a broken device; a repeat is a
// mild disappointment. (CEO review 4A, 5A, 7A.)

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"
#include "nvs.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"   // must precede task.h; the header enforces it
#include "freertos/task.h"       // uxTaskGetStackHighWaterMark

#include "page_riddle.h"
#include "riddle_decide.h"
#include "wake_log.h"
#include "page_common.h"
#include "epaper_port.h"
#include "GUI_Paint.h"
#include "pcf85063_bsp.h"
#include "sdcard_bsp.h"
#include "axp_prot.h"

static const char *TAG = "riddle";

// Canvas is 480x800 after the 270-degree rotation every page uses.
#define CANVAS_W        480
#define CANVAS_H        800
#define MARGIN_X        14
#define BODY_BOTTOM     (CANVAS_H - 8)

#include "../page_common/hebrew.inc"

// A 30-riddle batch is about 6KB. 32KB is 5x headroom, and unlike page_news's
// 384KB it means an absurd response fails fast instead of being parsed. (E8.)
#define RIDDLE_MAX_SIZE (32 * 1024)
#define MAX_RIDDLES     40
#define Q_MAX           200
#define A_MAX           64
#define BY_MAX          16
#define LOW_WATER       7      // refetch when fewer than this remain unseen

// Published as a release asset, deliberately not from the private repo: a
// private raw URL needs a PAT in flash, and a public repo would expose the
// kids' names in `by`. Nicknames only, birthdays stay in NVS. (CEO review 8A.)
#define RIDDLE_URL    "https://github.com/navotvolkgroundup/esp32-s3-epaper-397" \
                      "/releases/download/riddles-latest/riddles.json"
#define PATH_SPIFFS   "/spiffs/riddles.json"
#define PATH_TMP      "/spiffs/riddles.tmp"
#define PATH_SD       "/sdcard/riddles.json"

#define NVS_NS        "riddle"
#define NVS_KEY_STATE "state"
#define NVS_KEY_LOG   "wakelog"

typedef struct {
    char q[Q_MAX];
    char a[A_MAX];
    char choices[3][A_MAX];
    char by[BY_MAX];
    bool weekend;
    bool has_choices;
} riddle_t;

extern uint8_t *Image_Mono;
extern bool wifi_enable;
extern SemaphoreHandle_t rtc_mutex;

static riddle_t *s_batch;          // PSRAM, MAX_RIDDLES entries
static int       s_count;

// ------------------------------------------------------------------- nvs ----

static void state_load(riddle_nvs_t *st)
{
    memset(st, 0, sizeof *st);
    st->state = RS_IDLE;
    st->guess = RIDDLE_NO_GUESS;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof *st;
    riddle_nvs_t tmp;
    if (nvs_get_blob(h, NVS_KEY_STATE, &tmp, &len) == ESP_OK && len == sizeof *st)
        *st = tmp;
    nvs_close(h);
}

static void log_outcome(uint8_t o);      // defined with the wake log

static void state_save(const riddle_nvs_t *st)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed; today's progress will be lost");
        log_outcome(WO_NVS_FAILED);
        return;
    }
    esp_err_t e = nvs_set_blob(h, NVS_KEY_STATE, st, sizeof *st);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "nvs commit: %s", esp_err_to_name(e));
        log_outcome(WO_NVS_FAILED);
    }
}

// -------------------------------------------------------------- wake log ----
//
// One record per wake, filled as the wake proceeds and committed once at the
// end. Committing once matters: NVS is flash, and this device writes twice a
// day for years.

static wake_ring_t s_ring;
static wake_rec_t  s_cur;
static bool        s_cur_open;

static void log_begin(int reason)
{
    memset(&s_cur, 0, sizeof s_cur);
    s_cur.reason  = (uint8_t)reason;
    s_cur.outcome = WO_OK;
    s_cur.battery = (int8_t)get_battery_power();     // -1 if the gauge is mute
    if (get_usb_connected()) s_cur.flags |= WF_USB;
    s_cur_open = true;
}

// First non-OK outcome wins: the earliest fault is the one that explains the
// rest, and a later WO_OK must not paper over it.
static void log_outcome(uint8_t o)
{
    if (s_cur_open && s_cur.outcome == WO_OK) s_cur.outcome = o;
}

static void log_flag(uint8_t f) { if (s_cur_open) s_cur.flags |= f; }

static void log_commit(uint32_t when, uint16_t idx)
{
    if (!s_cur_open) return;
    s_cur.when = when;
    s_cur.idx  = idx;
    // Stack headroom, so the 32KB choice for the ambient task (eng review E5)
    // becomes a measurement after a week instead of staying a guess.
    s_cur.stack_free = (uint16_t)(uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));

    wake_ring_push(&s_ring, &s_cur);
    s_cur_open = false;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_LOG, &s_ring, sizeof s_ring);
    nvs_commit(h);
    nvs_close(h);
}

static void log_load(void)
{
    memset(&s_ring, 0, sizeof s_ring);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof s_ring;
    wake_ring_t tmp;
    // A size mismatch means the struct changed under an old blob. Start fresh
    // rather than reinterpreting stale bytes through a new layout.
    if (nvs_get_blob(h, NVS_KEY_LOG, &tmp, &len) == ESP_OK &&
        len == sizeof s_ring && wake_ring_valid(&tmp))
        s_ring = tmp;
    nvs_close(h);
}

// ----------------------------------------------------------------- parse ----

// Copies a cJSON string into a fixed buffer. Returns false if the field is
// missing, not a string, or empty -- the caller drops the whole item, because
// half a riddle is worse than one fewer riddle.
static bool take_str(const cJSON *o, const char *key, char *dst, size_t cap,
                     bool required)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (!cJSON_IsString(v) || !v->valuestring || !v->valuestring[0]) {
        dst[0] = '\0';
        return !required;
    }
    snprintf(dst, cap, "%s", v->valuestring);
    return true;
}

// Parses a batch document into s_batch. Returns the count, or -1 if the
// document itself is unusable. Individual bad items are skipped, not fatal.
static int parse_batch(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGE(TAG, "batch is not valid JSON");
        return -1;
    }
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "riddles");
    if (!cJSON_IsArray(arr)) {
        ESP_LOGE(TAG, "batch has no riddles array");
        cJSON_Delete(root);
        return -1;
    }

    int n = 0, skipped = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (n >= MAX_RIDDLES) break;
        if (!cJSON_IsObject(it)) { skipped++; continue; }

        // Only one renderer exists. An unknown type is skipped rather than
        // drawn as a riddle -- the field is there so a future content type
        // needs no schema migration on a hard-to-reflash device. (CEO 2.)
        const cJSON *ty = cJSON_GetObjectItemCaseSensitive(it, "type");
        if (cJSON_IsString(ty) && strcmp(ty->valuestring, "riddle") != 0) {
            skipped++;
            continue;
        }

        riddle_t r;
        memset(&r, 0, sizeof r);
        if (!take_str(it, "q", r.q, sizeof r.q, true) ||
            !take_str(it, "a", r.a, sizeof r.a, true)) { skipped++; continue; }
        take_str(it, "by", r.by, sizeof r.by, false);

        const cJSON *ch = cJSON_GetObjectItemCaseSensitive(it, "choices");
        if (cJSON_IsArray(ch) && cJSON_GetArraySize(ch) == 3) {
            bool ok = true, holds_answer = false;
            for (int i = 0; i < 3; i++) {
                const cJSON *c = cJSON_GetArrayItem(ch, i);
                if (!cJSON_IsString(c) || !c->valuestring[0]) { ok = false; break; }
                snprintf(r.choices[i], sizeof r.choices[i], "%s", c->valuestring);
                if (strcmp(r.choices[i], r.a) == 0) holds_answer = true;
            }
            // Choices that do not contain the answer make the game
            // unwinnable. Fall back to a plain reveal rather than showing
            // three wrong options.
            r.has_choices = ok && holds_answer;
        }

        const cJSON *wk = cJSON_GetObjectItemCaseSensitive(it, "weekend");
        r.weekend = cJSON_IsTrue(wk);

        s_batch[n++] = r;
    }
    cJSON_Delete(root);
    if (skipped) ESP_LOGW(TAG, "skipped %d malformed item(s)", skipped);
    ESP_LOGI(TAG, "batch holds %d riddle(s)", n);
    return n;
}

// ------------------------------------------------------------------ load ----

static int read_file(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(buf, 1, cap - 1, f);
    int leftover = fgetc(f) != EOF;      // did it fit?
    fclose(f);
    buf[n] = '\0';
    if (leftover) {
        ESP_LOGW(TAG, "%s is larger than %u bytes; ignoring it", path,
                 (unsigned)cap);
        return -1;
    }
    return (int)n;
}

// Writes to a temp path and renames only after the content has parsed. A
// truncated download or a power cut mid-write therefore cannot destroy a good
// queue -- the rename is the commit. (CEO review 4A.)
static bool store_batch(const char *json)
{
    FILE *f = fopen(PATH_TMP, "w");
    if (!f) { ESP_LOGE(TAG, "cannot open %s", PATH_TMP); return false; }
    size_t len = strlen(json);
    bool ok = fwrite(json, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok) { ESP_LOGE(TAG, "write failed"); remove(PATH_TMP); return false; }

    remove(PATH_SPIFFS);                 // SPIFFS rename needs a free target
    if (rename(PATH_TMP, PATH_SPIFFS) != 0) {
        ESP_LOGE(TAG, "rename failed; queue may be missing");
        return false;
    }
    return true;
}

// Tries, in order: the SD card, then the network if the queue is running low,
// then whatever is already in SPIFFS. Returns the riddle count.
static int load_batch(int remaining)
{
    char *buf = (char *)heap_caps_malloc(RIDDLE_MAX_SIZE, MALLOC_CAP_SPIRAM);
    if (!buf) { ESP_LOGE(TAG, "no PSRAM for the batch"); return 0; }
    int n = 0;

    // 1. SD card. The manual path that survives a broken pipeline: repo
    //    renamed, cert bundle stale, auth changed. Put the card in. (CEO 5A.)
    if (read_file(PATH_SD, buf, RIDDLE_MAX_SIZE) > 0) {
        int parsed = parse_batch(buf);
        if (parsed > 0) {
            ESP_LOGI(TAG, "imported %d riddle(s) from the SD card", parsed);
            log_outcome(WO_SD_IMPORT);
            log_flag(WF_FETCHED);
            store_batch(buf);
            heap_caps_free(buf);
            s_count = parsed;
            return parsed;
        }
        ESP_LOGW(TAG, "SD card batch did not parse; ignoring it");
        log_outcome(WO_PARSE_FAILED);
    }

    // 2. Network, only when the queue is actually running low.
    if (remaining < LOW_WATER && wifi_enable) {
        esp_netif_t *nif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip;
        if (nif && esp_netif_get_ip_info(nif, &ip) == ESP_OK && ip.ip.addr) {
            size_t got = 0;
            pc_fetch_status_t s = pc_fetch_url(RIDDLE_URL, buf,
                                               RIDDLE_MAX_SIZE, &got);
            if (s == PC_FETCH_OK) {
                int parsed = parse_batch(buf);
                // Parse BEFORE storing. A batch that does not parse must not
                // replace one that does.
                if (parsed > 0) {
                    log_flag(WF_FETCHED);
                    store_batch(buf);
                    heap_caps_free(buf);
                    s_count = parsed;
                    return parsed;
                }
                ESP_LOGW(TAG, "fetched batch did not parse; keeping the old one");
                log_outcome(WO_PARSE_FAILED);
            } else {
                ESP_LOGW(TAG, "fetch: %s", pc_fetch_strerror(s));
                log_outcome((s == PC_FETCH_TRUNCATED || s == PC_FETCH_SHORT)
                            ? WO_FETCH_PARTIAL : WO_FETCH_FAILED);
            }
        }
    }

    // 3. Whatever we already had.
    if (read_file(PATH_SPIFFS, buf, RIDDLE_MAX_SIZE) > 0) n = parse_batch(buf);
    heap_caps_free(buf);
    s_count = (n > 0) ? n : 0;
    return s_count;
}

// ---------------------------------------------------------------- screen ----

// LAYOUT ASSUMPTION, NOT YET VERIFIED ON HARDWARE.
//
// The three choices are stacked in physical button order so a child can map
// them without being told, which is the difference between a game and a
// decoration. That requires knowing which edge the buttons sit on and in what
// order. Eng review decision D15 says: look at the board, then pin this.
//
// Until then: Up above Function above Down, choices right-aligned toward the
// right edge. If the buttons run down the LEFT edge, flip CHOICES_RIGHT_ALIGNED
// and nothing else needs to change.
#define CHOICES_RIGHT_ALIGNED 1

#define HDR_Y        10
#define Q_TOP        70
#define CHOICE_H     72
#define CHOICE_GAP   10

static const char *kMarks[3] = { "^", "*", "v" };   // Up, Function, Down

static void canvas_begin(void)
{
    Paint_NewImage(Image_Mono, EPD_WIDTH, EPD_HEIGHT, 270, WHITE);
    Paint_SetScale(2);
    Paint_SelectImage(Image_Mono);
    Paint_Clear(WHITE);
}

static void draw_header(const riddle_nvs_t *st, const riddle_t *r)
{
    char stamp[40];
    xSemaphoreTake(rtc_mutex, portMAX_DELAY);
    Time_data t = PCF85063_GetTime();
    xSemaphoreGive(rtc_mutex);
    snprintf(stamp, sizeof stamp, "%02d/%02d", t.days, t.months);
    Paint_DrawString_EN(MARGIN_X, HDR_Y, stamp, &Font16, WHITE, BLACK);

    if (st->streak > 1) {
        char s[24];
        snprintf(s, sizeof s, "%u days", (unsigned)st->streak);
        Paint_DrawString_EN(CANVAS_W - MARGIN_X - 90, HDR_Y, s, &Font16,
                            WHITE, BLACK);
    }
    if (r && r->weekend)
        Paint_DrawString_EN(CANVAS_W / 2 - 16, HDR_Y, "**", &Font16, WHITE, BLACK);

    Paint_DrawLine(MARGIN_X, HDR_Y + 30, CANVAS_W - MARGIN_X, HDR_Y + 30,
                   BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
}

// Draws one choice row with its button marker on the button side.
static void draw_choice(int y, int i, const char *text, bool boxed)
{
    if (boxed)
        Paint_DrawRectangle(MARGIN_X, y - 6, CANVAS_W - MARGIN_X, y + HE_H + 6,
                            BLACK, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
#if CHOICES_RIGHT_ALIGNED
    Paint_DrawString_EN(MARGIN_X + 6, y + 8, kMarks[i], &Font24, WHITE, BLACK);
    he_draw_line_rtl(CANVAS_W - MARGIN_X - 10, y, text);
#else
    Paint_DrawString_EN(CANVAS_W - MARGIN_X - 26, y + 8, kMarks[i], &Font24,
                        WHITE, BLACK);
    he_draw_line_rtl(CANVAS_W - MARGIN_X - 40, y, text);
#endif
}

static void draw_question(const riddle_nvs_t *st, const riddle_t *r)
{
    canvas_begin();
    draw_header(st, r);

    int y = he_draw_wrapped(Q_TOP, MARGIN_X, CANVAS_W - MARGIN_X, BODY_BOTTOM,
                            r->q, 5);
    if (y < 0) y = Q_TOP + 5 * (HE_H - 6);       // clamped; drew what it could

    if (r->has_choices) {
        y += 40;
        for (int i = 0; i < 3; i++) {
            if (y + CHOICE_H > BODY_BOTTOM) break;
            draw_choice(y, i, r->choices[i], false);
            y += CHOICE_H + CHOICE_GAP;
        }
    }
    if (r->by[0]) {
        char credit[BY_MAX + 8];
        snprintf(credit, sizeof credit, "-- %s", r->by);
        he_draw_line_rtl(CANVAS_W - MARGIN_X, BODY_BOTTOM - HE_H, credit);
    }
    pc_refresh();
}

static void draw_answer(const riddle_nvs_t *st, const riddle_t *r)
{
    canvas_begin();
    draw_header(st, r);

    // The question stays, smaller, so the answer has something to answer.
    int y = he_draw_wrapped(Q_TOP, MARGIN_X, CANVAS_W - MARGIN_X, BODY_BOTTOM,
                            r->q, 3);
    if (y < 0) y = Q_TOP + 3 * (HE_H - 6);

    y += 50;
    Paint_DrawLine(MARGIN_X, y, CANVAS_W - MARGIN_X, y, BLACK,
                   DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    y += 30;
    he_draw_line_rtl(CANVAS_W - MARGIN_X, y, r->a);

    // Outcome, only if a guess was actually made.
    if (st->guess >= 0 && st->guess < 3 && r->has_choices) {
        bool right = strcmp(r->choices[st->guess], r->a) == 0;
        y += HE_H + 24;
        he_draw_line_rtl(CANVAS_W - MARGIN_X, y, right ? "כל הכבוד!" : "כמעט!");
    }
    pc_refresh();
}

static void draw_result(const riddle_nvs_t *st, const riddle_t *r)
{
    bool right = (st->guess >= 0 && st->guess < 3 && r->has_choices &&
                  strcmp(r->choices[st->guess], r->a) == 0);
    canvas_begin();
    draw_header(st, r);

    int y = 200;
    he_draw_line_rtl(CANVAS_W - MARGIN_X, y, right ? "כל הכבוד!" : "לא בדיוק");
    y += HE_H + 40;
    // Never the answer here: the whole product is the gap between morning and
    // afternoon, and spending it on a wrong guess throws the gap away.
    he_draw_line_rtl(CANVAS_W - MARGIN_X, y, "התשובה תופיע בארבע");
    pc_refresh();
}

// ----------------------------------------------------------------- entry ----

// How many riddles remain unseen, for the low-water refetch check.
static int remaining_of(const riddle_nvs_t *st)
{
    if (s_count <= 0) return 0;
    int used = (st->state == RS_IDLE) ? 0 : (st->idx + 1);
    int left = s_count - used;
    return left < 0 ? 0 : left;
}

static void render(riddle_action_e act, const riddle_nvs_t *st)
{
    if (s_count <= 0 || st->idx >= s_count) {
        pc_draw_message("No riddles yet.",
                        "Add riddles.json, or check the network.");
        return;
    }
    const riddle_t *r = &s_batch[st->idx];
    switch (act) {
    case ACT_SHOW_QUESTION: draw_question(st, r); break;
    case ACT_SHOW_ANSWER:   draw_answer(st, r);   break;
    case ACT_SHOW_RESULT:   draw_result(st, r);   break;
    case ACT_NONE:          break;
    }
}

static bool ensure_batch(const riddle_nvs_t *st)
{
    if (!s_batch) {
        s_batch = (riddle_t *)heap_caps_calloc(MAX_RIDDLES, sizeof(riddle_t),
                                               MALLOC_CAP_SPIRAM);
        if (!s_batch) { ESP_LOGE(TAG, "no PSRAM for the batch table"); return false; }
    }
    if (s_count == 0) load_batch(remaining_of(st));
    return s_count > 0;
}

// The current instant, as UTC seconds, from the RTC.
static time_t now_utc(void)
{
    xSemaphoreTake(rtc_mutex, portMAX_DELAY);
    Time_data t = PCF85063_GetTime();
    xSemaphoreGive(rtc_mutex);

    // The RTC holds local wall time. riddle_local_day() wants a UTC instant,
    // so this is the one place the two representations meet. Building the tm
    // with tm_isdst = -1 lets the C library resolve which offset applies --
    // the same reason the schedule is stored in UTC. (CEO 9A.)
    struct tm lt;
    memset(&lt, 0, sizeof lt);
    lt.tm_year = t.years + 100;      // Time_data years are since 2000
    lt.tm_mon  = t.months - 1;
    lt.tm_mday = t.days;
    lt.tm_hour = t.hours;
    lt.tm_min  = t.minutes;
    lt.tm_sec  = t.seconds;
    lt.tm_isdst = -1;

    char saved[64];
    const char *cur = getenv("TZ");
    snprintf(saved, sizeof saved, "%s", cur ? cur : "");
    setenv("TZ", RIDDLE_TZ, 1); tzset();
    time_t utc = mktime(&lt);
    if (saved[0]) setenv("TZ", saved, 1); else unsetenv("TZ");
    tzset();

    return utc;
}

// Local civil day right now.
static int32_t today_now(void)
{
    return riddle_local_day(now_utc(), RIDDLE_TZ);
}

static riddle_input_t make_input(int reason, int guess)
{
    riddle_input_t in;
    in.reason  = (uint8_t)reason;
    in.guess   = (int8_t)guess;
    in.batch_n = (uint16_t)s_count;
    in.today   = today_now();
    return in;
}

// ----------------------------------------------------------- diagnostics ----
//
// Font12 is 8x21, so 480px gives 56 columns and 14 rows fit comfortably. This
// screen is for the adult holding the board, not the kid looking at the wall,
// so it is dense ASCII rather than anything pretty.

static const char *reason_abbrev(uint8_t r)
{
    switch (r) {
    case WAKE_MORNING:   return "AM";
    case WAKE_AFTERNOON: return "PM";
    case WAKE_GUESS:     return "GS";
    case WAKE_REVEAL:    return "RV";
    case WAKE_MENU:      return "MN";
    default:             return "??";
    }
}

void page_riddle_diagnostics(void)
{
    log_load();

    wake_rec_t recs[WAKE_LOG_N];
    int n = wake_ring_read(&s_ring, recs, WAKE_LOG_N);

    canvas_begin();
    Paint_DrawString_EN(MARGIN_X, 8, "WAKE LOG", &Font16, WHITE, BLACK);

    char line[96];
    snprintf(line, sizeof line, "%d entries   %d-day guess run",
             n, wake_ring_recent_guesses(&s_ring));
    Paint_DrawString_EN(MARGIN_X, 40, line, &Font12, WHITE, BLACK);
    Paint_DrawLine(MARGIN_X, 64, CANVAS_W - MARGIN_X, 64, BLACK,
                   DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    Paint_DrawString_EN(MARGIN_X, 72, "DATE  TIME  WK OUTCOME       G BAT STACK",
                        &Font12, WHITE, BLACK);

    int y = 96;
    if (n == 0) {
        Paint_DrawString_EN(MARGIN_X, y, "No wakes recorded yet.", &Font12,
                            WHITE, BLACK);
    }
    for (int i = 0; i < n && y + 21 < BODY_BOTTOM; i++) {
        const wake_rec_t *r = &recs[i];
        char when[16] = "--/-- --:--";
        if (r->when) {
            // Shown in local time, because that is what you compare against
            // "did it appear before the kids left?".
            char saved[64];
            const char *cur = getenv("TZ");
            snprintf(saved, sizeof saved, "%s", cur ? cur : "");
            setenv("TZ", RIDDLE_TZ, 1); tzset();
            time_t t = (time_t)r->when;
            struct tm lt;
            localtime_r(&t, &lt);
            if (saved[0]) setenv("TZ", saved, 1); else unsetenv("TZ");
            tzset();
            // The %% 100 is not paranoia about the clock, it is for the
            // compiler: -Werror=format-truncation assumes a full-range int
            // needs 11 characters per %02d, so without a visible bound it
            // rejects any buffer smaller than 48. Clamping says "two digits".
            snprintf(when, sizeof when, "%02d/%02d %02d:%02d",
                     lt.tm_mday % 100, (lt.tm_mon + 1) % 100,
                     lt.tm_hour % 100, lt.tm_min % 100);
        }

        const char *g = !(r->flags & WF_GUESSED) ? "-"
                      : (r->flags & WF_CORRECT)  ? "+" : "x";
        char bat[8];
        if (r->battery < 0) snprintf(bat, sizeof bat, " --");
        else                snprintf(bat, sizeof bat, "%3d", r->battery);

        char stk[8] = "    -";
        if (r->stack_free) snprintf(stk, sizeof stk, "%4uB", (unsigned)r->stack_free);

        snprintf(line, sizeof line, "%-11s %-2s %-13s %s %s%% %s",
                 when, reason_abbrev(r->reason),
                 wake_outcome_name(r->outcome), g, bat, stk);
        Paint_DrawString_EN(MARGIN_X, y, line, &Font12, WHITE, BLACK);
        y += 22;
    }

    Paint_DrawString_EN(MARGIN_X, BODY_BOTTOM - 24,
                        "Double-click Function to go back.", &Font12,
                        WHITE, BLACK);
    pc_refresh();

    EPD_Sleep();
    while (pc_button_wait(portMAX_DELAY) != PC_BTN_BACK) { }
    EPD_Init();
    pc_refresh();
}

void page_riddle_ambient(int reason)
{
    log_load();
    log_begin(reason);

    riddle_nvs_t st;
    state_load(&st);
    if (!ensure_batch(&st)) {
        log_outcome(WO_NO_BATCH);
        pc_draw_message("No riddles yet.",
                        "Add riddles.json, or check the network.");
        log_commit((uint32_t)now_utc(), 0);
        return;
    }
    riddle_input_t in = make_input(reason, RIDDLE_NO_GUESS);
    riddle_action_e act = riddle_decide(&in, &st);
    // Commit BEFORE drawing. A power cut between the two then repeats a day
    // rather than skipping one, and a repeat is the cheaper mistake.
    state_save(&st);
    render(act, &st);

    if (st.state == RS_GUESSED || st.state == RS_ANSWER_SHOWN) {
        if (st.guess >= 0) {
            log_flag(WF_GUESSED);
            const riddle_t *r = &s_batch[st.idx];
            if (r->has_choices && strcmp(r->choices[st.guess], r->a) == 0)
                log_flag(WF_CORRECT);
        }
    }
    log_commit((uint32_t)now_utc(), st.idx);
}

void page_riddle_show(void)
{
    riddle_nvs_t st;
    state_load(&st);
    if (!ensure_batch(&st)) {
        pc_draw_message("No riddles yet. Put riddles.json on the SD card,",
                        "or connect WiFi. Double-click Function to go back.");
        EPD_Sleep();
        while (pc_button_wait(portMAX_DELAY) != PC_BTN_BACK) { }
        EPD_Init();
        pc_refresh();
        return;
    }

    // WAKE_MENU is read-only, so it will happily show a stale screen. That is
    // right when the 06:30 wake already ran, and wrong when it did not --
    // and with ambient mode defaulting OFF (CEO 14A) the tile is at first the
    // ONLY way in. If today has not started yet, start it here, which is the
    // same rule the afternoon wake uses when the morning never happened.
    // Otherwise the tile shows yesterday's riddle and refuses every guess.
    riddle_input_t in = make_input(
        (st.state == RS_IDLE || st.day != today_now()) ? WAKE_MORNING : WAKE_MENU,
        RIDDLE_NO_GUESS);
    riddle_action_e first = riddle_decide(&in, &st);
    if (in.reason == WAKE_MORNING) state_save(&st);
    render(first, &st);

    for (;;) {
        EPD_Sleep();
        pc_button_t b = pc_button_wait(portMAX_DELAY);
        EPD_Init();

        if (b == PC_BTN_BACK) { pc_refresh(); return; }

        int guess = (b == PC_BTN_UP) ? 0 : (b == PC_BTN_SELECT) ? 1
                  : (b == PC_BTN_DOWN) ? 2 : RIDDLE_NO_GUESS;
        if (guess == RIDDLE_NO_GUESS) { pc_refresh(); continue; }

        // With no choices to pick from, Function means "just tell me".
        const riddle_t *r = &s_batch[st.idx];
        int reason = r->has_choices ? WAKE_GUESS : WAKE_REVEAL;
        if (!r->has_choices && b != PC_BTN_SELECT) { pc_refresh(); continue; }

        riddle_input_t gi = make_input(reason, guess);
        riddle_action_e act = riddle_decide(&gi, &st);
        state_save(&st);
        if (act != ACT_NONE && st.state == RS_GUESSED) {
            // The menu is a real way to play, so it feeds the same counter
            // the ambient path does -- otherwise the participation number
            // undercounts exactly while ambient mode is still switched off.
            log_load();
            log_begin(WAKE_GUESS);
            log_flag(WF_GUESSED);
            if (r->has_choices && strcmp(r->choices[guess], r->a) == 0)
                log_flag(WF_CORRECT);
            log_commit((uint32_t)now_utc(), st.idx);
        }
        if (act == ACT_NONE) pc_refresh(); else render(act, &st);
    }
}
