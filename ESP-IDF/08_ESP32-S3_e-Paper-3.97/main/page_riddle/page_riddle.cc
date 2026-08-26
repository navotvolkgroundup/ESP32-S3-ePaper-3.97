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
#include "kids.h"
#include "page_common.h"
#include "epaper_port.h"
#include "GUI_Paint.h"
#include "pcf85063_bsp.h"
#include "sdcard_bsp.h"
#include "axp_prot.h"
#include "sched.h"

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

// A PUBLIC release asset, so the firmware carries no credential of any kind.
//
// This originally pointed at the private notes repo, which was wrong: release
// assets on a private repo need auth just as much as a raw file does, so the
// fetch would have 404'd and the only symptom would have been a queue that
// never refreshed. The whole point of 8A was to avoid a token in flash.
//
// Publishing the batch is safe because it holds no personal data. The schema
// has a `by` field for crediting an author, and entries using it must carry a
// nickname -- names and birthdays live in device NVS and never leave the house.
//
// Verified fetchable unauthenticated: HTTP 200, 6790 bytes, sha256 matching
// the local file.
#define RIDDLE_URL    "https://github.com/navotvolkgroundup/ESP32-S3-ePaper-3.97" \
                      "/releases/download/riddles-latest/riddles.json"
#define PATH_SPIFFS   "/spiffs/riddles.json"
#define PATH_TMP      "/spiffs/riddles.tmp"
#define PATH_SD       "/sdcard/riddles.json"

#define NVS_NS        "riddle"
#define NVS_KEY_STATE "state"
#define NVS_KEY_LOG   "wakelog"
#define NVS_KEY_KIDS  "kids"
#define PATH_KIDS_SD  "/sdcard/kids.json"

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

// ------------------------------------------------------------------ kids ----
//
// Names and birthdays live ONLY here, in device NVS, imported from an SD card
// that never leaves the house. They are deliberately absent from riddles.json,
// which is published to a public GitHub release -- children's names and
// birthdays have no business on a public URL. (CEO review 8A.)
//
// An empty blob is the normal, shipping state: both features simply never
// fire, which is what lets the build be complete while the answer is still
// outstanding. (CEO review 16A.)

static kids_t s_kids;

static void kids_load(void)
{
    memset(&s_kids, 0, sizeof s_kids);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof s_kids;
    kids_t tmp;
    if (nvs_get_blob(h, NVS_KEY_KIDS, &tmp, &len) == ESP_OK &&
        len == sizeof s_kids && kids_valid(&tmp))
        s_kids = tmp;
    nvs_close(h);
}

static void kids_store(const kids_t *k)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, NVS_KEY_KIDS, k, sizeof *k);
    nvs_commit(h);
    nvs_close(h);
}

// Imports /sdcard/kids.json if present, then keeps it in NVS so the card can
// be taken out again. Shape:
//   {"kids":[{"name":"...","month":3,"day":14}, ...]}
static void kids_import_from_sd(void)
{
    char buf[1024];
    FILE *f = fopen(PATH_KIDS_SD, "r");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;

    cJSON *root = cJSON_Parse(buf);
    if (!root) { ESP_LOGW(TAG, "kids.json did not parse; ignoring it"); return; }
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "kids");
    if (!cJSON_IsArray(arr)) { cJSON_Delete(root); return; }

    kids_t k;
    memset(&k, 0, sizeof k);
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (k.count >= KIDS_MAX) break;
        const cJSON *nm = cJSON_GetObjectItemCaseSensitive(it, "name");
        if (!cJSON_IsString(nm) || !nm->valuestring[0]) continue;
        kid_t *kid = &k.kid[k.count];
        snprintf(kid->name, KID_NAME_MAX, "%s", nm->valuestring);
        const cJSON *mo = cJSON_GetObjectItemCaseSensitive(it, "month");
        const cJSON *dy = cJSON_GetObjectItemCaseSensitive(it, "day");
        if (cJSON_IsNumber(mo)) kid->birth_month = (uint8_t)mo->valueint;
        if (cJSON_IsNumber(dy)) kid->birth_day   = (uint8_t)dy->valueint;
        k.count++;
    }
    cJSON_Delete(root);

    if (!kids_valid(&k)) {
        ESP_LOGW(TAG, "kids.json is out of range; ignoring it");
        return;
    }
    if (memcmp(&k, &s_kids, sizeof k) != 0) {
        s_kids = k;
        kids_store(&k);
        ESP_LOGI(TAG, "imported %d kid(s) from the SD card", k.count);
    }
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

// LAYOUT GEOMETRY -- verified against the hardware 2026-08-26 (eng review D15).
//
// The three choices are stacked in physical button order so a child can map
// them without being told, which is the difference between a game and a
// decoration. Three facts pin the layout, and only the last needed eyes:
//
//   1. Button ORDER: Up=GPIO4, Function=GPIO5, Down=GPIO6. Three sequential
//      pins, so they are one physical group in that sequence.
//   2. Canvas +y is visually DOWN. The vendor's menu increments the selection
//      on Down while y_positions runs 57->243->429->615, which only makes
//      sense if Down moves the highlight downward on the panel as held.
//   3. The buttons run down the LEFT edge, top to bottom Up/Function/Down.
//      Not derivable from code: the vendor never places button hints
//      spatially, only as prose ("Up/Down: pick time zone" at x=10).
//
// So the marker glyph goes on the LEFT, beside its button, and the Hebrew
// (RTL, so right-aligned anyway) runs away from it.
//
// The old name for this flag was CHOICES_RIGHT_ALIGNED, which described the
// TEXT alignment and read as though it described the marker side. It misled
// me into recommending the exact wrong flip once. Named for the physical fact
// now, because that is what a reader is actually checking against.
#define BUTTONS_ON_LEFT_EDGE 1

// Width the marker reserves. riddle_gen.py must budget choices against
// (usable width - this), or a long choice can run under the glyph.
#define MARKER_GUTTER 34

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

// Birthday takeover.
//
// This is a banner across the top rather than a full-screen replacement, and
// that is a deliberate narrowing of the accepted item. A full takeover would
// mean the day's riddle is never shown, which then leaves the 16:00 wake
// revealing the answer to a question nobody read -- and it costs the kid their
// riddle on the one morning they are most likely to walk over. The banner
// still makes the screen unmistakably about them, which was the point.
//
// Returns the y to continue drawing below, or `y` unchanged when nobody has a
// birthday today (the normal case, and the only case until kids.json exists).
static int draw_birthday_banner(int y, int month, int day)
{
    int who = kids_birthday_on(&s_kids, month, day);
    if (who < 0) return y;

    Paint_DrawRectangle(MARGIN_X, y, CANVAS_W - MARGIN_X, y + 2 * HE_H + 16,
                        BLACK, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
    he_draw_line_rtl(CANVAS_W - MARGIN_X - 12, y + 8, "יום הולדת שמח");
    he_draw_line_rtl(CANVAS_W - MARGIN_X - 12, y + 8 + HE_H, s_kids.kid[who].name);
    return y + 2 * HE_H + 32;
}

// "<name>, this one is for you." Fires about one day in three, deterministically
// per day so the morning screen and the afternoon screen agree on who.
static int draw_callout(int y, int32_t today)
{
    int who = kids_pick_callout(&s_kids, today);
    if (who < 0) return y;

    char line[KID_NAME_MAX + 24];
    snprintf(line, sizeof line, "%s, זאת בשבילך", s_kids.kid[who].name);
    he_draw_line_rtl(CANVAS_W - MARGIN_X, y, line);
    return y + HE_H;
}

// Draws one choice row with its button marker on the button side.
static void draw_choice(int y, int i, const char *text, bool boxed)
{
    if (boxed)
        Paint_DrawRectangle(MARGIN_X, y - 6, CANVAS_W - MARGIN_X, y + HE_H + 6,
                            BLACK, DOT_PIXEL_2X2, DRAW_FILL_EMPTY);
#if BUTTONS_ON_LEFT_EDGE
    Paint_DrawString_EN(MARGIN_X + 6, y + 8, kMarks[i], &Font24, WHITE, BLACK);
    he_draw_line_rtl(CANVAS_W - MARGIN_X - 10, y, text);
#else
    Paint_DrawString_EN(CANVAS_W - MARGIN_X - 26, y + 8, kMarks[i], &Font24,
                        WHITE, BLACK);
    he_draw_line_rtl(CANVAS_W - MARGIN_X - MARKER_GUTTER - 6, y, text);
#endif
}

static void draw_question(const riddle_nvs_t *st, const riddle_t *r)
{
    canvas_begin();
    draw_header(st, r);

    int top = Q_TOP;
    xSemaphoreTake(rtc_mutex, portMAX_DELAY);
    Time_data t = PCF85063_GetTime();
    xSemaphoreGive(rtc_mutex);
    top = draw_birthday_banner(top, t.months, t.days);
    top = draw_callout(top, st->day);

    int y = he_draw_wrapped(top, MARGIN_X, CANVAS_W - MARGIN_X, BODY_BOTTOM,
                            r->q, 5);
    if (y < 0) y = top + 5 * (HE_H - 6);         // clamped; drew what it could

    if (r->has_choices) {
        y += 40;
        for (int i = 0; i < 3; i++) {
            if (y + CHOICE_H > BODY_BOTTOM) break;
            draw_choice(y, i, r->choices[i], false);
            y += CHOICE_H + CHOICE_GAP;
        }
    }
    // Ambient state, small, bottom-left. The toggle is otherwise invisible,
    // and an invisible switch that decides whether the board powers itself
    // off is the kind of thing you discover the hard way.
    Paint_DrawString_EN(MARGIN_X, BODY_BOTTOM - 22,
                        sched_ambient_enabled()
                            ? "auto: on  (Boot: off, hold: log)"
                            : "auto: off (Boot: on, hold: log)",
                        &Font12, WHITE, BLACK);

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
    kids_load();
    kids_import_from_sd();      // no card, or no file, is the normal case
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

        // Boot long-hold opens the wake log. It is the last unused control on
        // this page, and the log is the only way to tell a working device from
        // one whose alarm never armed -- both look like a riddle on a wall.
        if (b == PC_BTN_SETTINGS) {
            page_riddle_diagnostics();
            riddle_input_t back = make_input(WAKE_MENU, RIDDLE_NO_GUESS);
            render(riddle_decide(&back, &st), &st);
            continue;
        }

        // Boot-click toggles ambient mode. It is the only physical control not
        // already spoken for -- Up/Function/Down are the three guesses and
        // Function-double is back -- and 14A requires this to ship OFF, so
        // there has to be some way to turn it on without a reflash.
        if (b == PC_BTN_POWER) {
            bool on = !sched_ambient_enabled();
            sched_set_ambient_enabled(on);
            ESP_LOGI(TAG, "ambient mode %s", on ? "ENABLED" : "disabled");
            if (on) {
                // Arm immediately, and say plainly whether it took. Finding
                // out at 06:30 tomorrow that the alarm never verified is the
                // expensive way to learn it.
                pc_draw_message(sched_arm_next()
                                    ? "Auto mode ON. Next wake armed."
                                    : "Auto mode ON but the ALARM DID NOT ARM.",
                                "Double-click Function to go back.");
            } else {
                pc_draw_message("Auto mode OFF. The board will stay awake.",
                                "Double-click Function to go back.");
            }
            continue;
        }

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
