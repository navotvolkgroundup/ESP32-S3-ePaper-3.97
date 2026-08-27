// Scheduled-wake plumbing. See sched.h for why this owns arming only.

#include "sched.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "riddle_decide.h"
#include "pcf85063_bsp.h"
#include "axp_prot.h"

static const char *TAG = "sched";

#define NVS_NS          "sched"
#define KEY_AMBIENT     "ambient"
#define KEY_SLOT        "slot"

// --------------------------------------------------------------- clock ----

time_t sched_now_utc(void)
{
    Time_data t = PCF85063_GetTime();

    // The RTC holds local wall time. tm_isdst = -1 asks the C library which
    // offset applies on that date, which is the entire reason the schedule is
    // stored in UTC rather than against a fixed offset: the vendor's timezone
    // table has no DST rules, so a fixed +3 silently becomes an hour wrong
    // when IDT ends (9A).
    struct tm lt;
    memset(&lt, 0, sizeof lt);
    lt.tm_year  = t.years + 100;         // Time_data years are since 2000
    lt.tm_mon   = t.months - 1;
    lt.tm_mday  = t.days;
    lt.tm_hour  = t.hours;
    lt.tm_min   = t.minutes;
    lt.tm_sec   = t.seconds;
    lt.tm_isdst = -1;

    char saved[64];
    const char *cur = getenv("TZ");
    snprintf(saved, sizeof saved, "%s", cur ? cur : "");
    setenv("TZ", RIDDLE_TZ, 1);
    tzset();
    time_t utc = mktime(&lt);
    if (saved[0]) setenv("TZ", saved, 1); else unsetenv("TZ");
    tzset();

    return utc;
}

// ----------------------------------------------------------------- nvs ----

static int32_t nvs_get_or(const char *key, int32_t dflt)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return dflt;
    int32_t v = dflt;
    nvs_get_i32(h, key, &v);
    nvs_close(h);
    return v;
}

static void nvs_put(const char *key, int32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed writing %s", key);
        return;
    }
    if (nvs_set_i32(h, key, v) == ESP_OK) nvs_commit(h);
    nvs_close(h);
}

bool sched_ambient_enabled(void)      { return nvs_get_or(KEY_AMBIENT, 0) != 0; }
void sched_set_ambient_enabled(bool on) { nvs_put(KEY_AMBIENT, on ? 1 : 0); }

uint8_t sched_wake_slot(void)
{
    // Default to the morning: if the record is missing the sensible guess is
    // "show the riddle", never "reveal an answer nobody has seen".
    int32_t v = nvs_get_or(KEY_SLOT, WAKE_MORNING);
    return (v == WAKE_AFTERNOON) ? WAKE_AFTERNOON : WAKE_MORNING;
}

// ---------------------------------------------------------------- arming ----

bool sched_arm_next(void)
{
    time_t now = sched_now_utc();
    int is_morning = 1;
    time_t next = riddle_next_wake(now, RIDDLE_TZ, &is_morning);
    if (next == (time_t)-1) {
        ESP_LOGE(TAG, "could not compute the next wake");
        return false;
    }

    // Back to local, because the RTC alarm matches on local wall clock.
    char saved[64];
    const char *cur = getenv("TZ");
    snprintf(saved, sizeof saved, "%s", cur ? cur : "");
    setenv("TZ", RIDDLE_TZ, 1);
    tzset();
    struct tm lt;
    localtime_r(&next, &lt);
    if (saved[0]) setenv("TZ", saved, 1); else unsetenv("TZ");
    tzset();

    ESP_LOGI(TAG, "next wake %02d:%02d local (%s), in %lld s",
             lt.tm_hour, lt.tm_min, is_morning ? "riddle" : "answer",
             (long long)(next - now));

    if (!PCF85063_alarm_daily(lt.tm_hour, lt.tm_min)) {
        ESP_LOGE(TAG, "alarm did NOT verify -- refusing to consider power-off");
        return false;
    }

    // Record BEFORE returning true, so the boot that follows knows which slot
    // it is servicing without having to re-derive it from a clock that may
    // have moved past the boundary during the ~4 s cold boot.
    nvs_put(KEY_SLOT, is_morning ? WAKE_MORNING : WAKE_AFTERNOON);
    save_mode_enable_to_nvs(SCHED_MODE_RIDDLE);
    return true;
}

// ------------------------------------------------------------- power off ----

bool sched_power_off_if_safe(void)
{
    if (!sched_ambient_enabled()) {
        ESP_LOGI(TAG, "ambient mode is off; staying awake");
        return false;
    }
    // A cabled board must stay reachable. Powering down here is what would
    // make the board un-flashable, and it is the one failure that makes every
    // other bug expensive to fix.
    if (get_usb_connected()) {
        ESP_LOGW(TAG, "USB attached; staying awake so the board stays flashable");
        return false;
    }
    if (!sched_arm_next()) {
        ESP_LOGE(TAG, "no verified alarm; staying awake rather than never "
                      "waking again");
        return false;
    }
    ESP_LOGI(TAG, "alarm verified, powering off");
    axp_pwr_off();
    return true;                 // not reached; axp_pwr_off cuts the rail
}
