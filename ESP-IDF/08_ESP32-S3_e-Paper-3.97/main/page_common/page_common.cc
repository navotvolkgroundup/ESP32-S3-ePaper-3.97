// Shared helpers for the full-screen pages. See page_common.h for the why.

#include <string.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"

#include "page_common.h"
#include "epaper_port.h"
#include "GUI_Paint.h"
#include "button_bsp.h"

static const char *TAG = "page_common";

extern uint8_t *Image_Mono;

#define MARGIN_X 10

// ------------------------------------------------------------- buttons ----

// Base code of each button; each owns seven consecutive codes from its base.
enum { BTN_UP_BASE = 0, BTN_FN_BASE = 7, BTN_DOWN_BASE = 14, BTN_BOOT_BASE = 21 };
// Offsets within a button's block.
enum { EV_CLICK = 0, EV_DOUBLE = 1, EV_PRESS = 2, EV_BOUNCE = 3,
       EV_REPEAT = 4, EV_LONG_ONCE = 5, EV_LONG_HOLD = 6 };

pc_button_t pc_button_classify(int code)
{
    if (code < 0) return PC_BTN_TIMEOUT;

    switch (code) {
    case BTN_UP_BASE   + EV_CLICK:     return PC_BTN_UP;
    case BTN_DOWN_BASE + EV_CLICK:     return PC_BTN_DOWN;
    case BTN_FN_BASE   + EV_CLICK:     return PC_BTN_SELECT;
    case BTN_FN_BASE   + EV_DOUBLE:    return PC_BTN_BACK;
    case BTN_BOOT_BASE + EV_DOUBLE:    return PC_BTN_BACK;
    case BTN_BOOT_BASE + EV_CLICK:     return PC_BTN_POWER;
    case BTN_FN_BASE   + EV_LONG_ONCE: return PC_BTN_REFRESH;
    // BOOT'S LONG PRESS IS 26, NOT 27. Boot is numbered base+0..6 like the
    // others, but button_bsp.c's LONG_PRESS_HOLD arm for Boot_id sets bit 23
    // -- the same bit as PRESS_DOWN -- instead of 27. So base+EV_LONG_HOLD was
    // dead and the wake log could not be opened at all.
    //
    // 23 is NOT the fix, tempting as it looks: it fires on every Boot
    // PRESS_DOWN, so mapping it here would open the wake log on every ordinary
    // Boot click, before the click itself was even classified. 26 is
    // LONG_PRESS_START for Boot and nothing else, so it fires once per long
    // press and is unambiguous.
    case BTN_BOOT_BASE + EV_LONG_ONCE: return PC_BTN_SETTINGS;
    default:                           return PC_BTN_IGNORE;
    }
}

pc_button_t pc_button_wait(TickType_t timeout)
{
    // Discarding IGNORE inside the loop is the point: callers that write their
    // own `while (1) { code = wait(); ... }` are the ones that grow catch-all
    // branches and fire multiple times per press.
    for (;;) {
        int code = wait_key_event_and_return_code(timeout);
        pc_button_t b = pc_button_classify(code);
        // Raw code AND its meaning, on every real event. Two bugs this session
        // came from inferring codes out of button_bsp.c rather than watching
        // what the hardware actually emits, and one of them (the wake log) is
        // on a page that draws without logging, so serial could not see it at
        // all. Cheap: a handful of lines per press, only when a button moves.
        if (code >= 0) ESP_LOGI(TAG, "button raw=%d -> %d", code, (int)b);
        if (b != PC_BTN_IGNORE) return b;
        // A non-decision event still consumed the timeout window. With
        // portMAX_DELAY that is harmless; with a finite timeout the caller
        // gets a slightly longer wait than asked for, which no page cares
        // about and which is cheaper than plumbing a deadline through.
    }
}

// ------------------------------------------------------------- fetching ----

// Per-request state. The original lived in `static` locals inside the event
// handler, which works only because exactly one fetch is ever in flight.
// Carrying it through user_data removes that assumption before a second page
// starts fetching.
typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    bool   overflowed;
} fetch_ctx_t;

static esp_err_t fetch_event_handler(esp_http_client_event_t *evt)
{
    fetch_ctx_t *ctx = (fetch_ctx_t *)evt->user_data;
    if (!ctx) return ESP_FAIL;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        ctx->len = 0;
        ctx->overflowed = false;
        if (ctx->cap) ctx->buf[0] = '\0';
        break;
    case HTTP_EVENT_ON_DATA: {
        size_t room = (ctx->cap > ctx->len + 1) ? ctx->cap - ctx->len - 1 : 0;
        size_t take = ((size_t)evt->data_len < room) ? (size_t)evt->data_len : room;
        if (take) {
            memcpy(ctx->buf + ctx->len, evt->data, take);
            ctx->len += take;
            ctx->buf[ctx->len] = '\0';
        }
        if (take < (size_t)evt->data_len) ctx->overflowed = true;
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

pc_fetch_status_t pc_fetch_url(const char *url, char *buf, size_t cap,
                               size_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!url || !buf || cap == 0) return PC_FETCH_TRANSPORT;
    buf[0] = '\0';

    fetch_ctx_t ctx = { buf, cap, 0, false };

    esp_http_client_config_t config = {};
    config.url               = url;
    config.event_handler     = fetch_event_handler;
    config.user_data         = &ctx;
    config.crt_bundle_attach = esp_crt_bundle_attach;   // feeds 301 off http
    config.timeout_ms        = 20000;
    config.user_agent        = "esp32-eink/1.0";
    // The default header buffer is 512 bytes, and that is not enough for a
    // GitHub release download. The first response is a 302 whose Location is a
    // signed objects.githubusercontent.com URL -- an X-Amz-Signature query
    // string that runs well past 512 on its own -- so esp_http_client logs
    // "Out of buffer" and the perform fails with ESP_FAIL. It presents as
    // "could not reach the server", which sends you looking at WiFi.
    // Found on hardware 2026-08-27; the host tests cannot see this.
    config.buffer_size       = 4096;
    config.buffer_size_tx    = 2048;   // the redirected GET line is long too

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return PC_FETCH_TRANSPORT;

    esp_err_t err  = esp_http_client_perform(client);
    int status     = esp_http_client_get_status_code(client);
    int64_t declared = esp_http_client_get_content_length(client);
    esp_http_client_cleanup(client);

    if (out_len) *out_len = ctx.len;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: transport: %s", url, esp_err_to_name(err));
        return PC_FETCH_TRANSPORT;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "%s: HTTP %d", url, status);
        return PC_FETCH_HTTP_STATUS;
    }
    if (ctx.overflowed) {
        ESP_LOGW(TAG, "%s: body exceeded the %u-byte buffer, kept %u",
                 url, (unsigned)cap, (unsigned)ctx.len);
        return PC_FETCH_TRUNCATED;
    }
    // Chunked responses report -1 here, so a missing length means "cannot
    // verify", not "mismatch". Only a positive, disagreeing value is a fault.
    if (declared > 0 && (size_t)declared != ctx.len) {
        ESP_LOGW(TAG, "%s: Content-Length %lld but received %u",
                 url, (long long)declared, (unsigned)ctx.len);
        return PC_FETCH_SHORT;
    }
    ESP_LOGI(TAG, "%s: fetched %u bytes", url, (unsigned)ctx.len);
    return PC_FETCH_OK;
}

const char *pc_fetch_strerror(pc_fetch_status_t s)
{
    switch (s) {
    case PC_FETCH_OK:          return "ok";
    case PC_FETCH_TRUNCATED:   return "response too large for the buffer";
    case PC_FETCH_SHORT:       return "connection dropped mid-body";
    case PC_FETCH_TRANSPORT:   return "could not reach the server";
    case PC_FETCH_HTTP_STATUS: return "server refused the request";
    default:                   return "unknown";
    }
}

// -------------------------------------------------------------- drawing ----

void pc_refresh(void)
{
    EPD_Display_Partial(Image_Mono, 0, 0, EPD_WIDTH, EPD_HEIGHT);
}

void pc_draw_message(const char *l1, const char *l2)
{
    Paint_NewImage(Image_Mono, EPD_WIDTH, EPD_HEIGHT, 270, WHITE);
    Paint_SetScale(2);
    Paint_SelectImage(Image_Mono);
    Paint_Clear(WHITE);
    Paint_DrawString_EN(MARGIN_X, 60, l1, &Font16, WHITE, BLACK);
    if (l2) Paint_DrawString_EN(MARGIN_X, 100, l2, &Font16, WHITE, BLACK);
    pc_refresh();
}
