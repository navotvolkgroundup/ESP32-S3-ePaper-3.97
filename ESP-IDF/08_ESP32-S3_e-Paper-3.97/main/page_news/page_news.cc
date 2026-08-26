// Techmeme RSS newspaper page.
//
// Fetches https://www.techmeme.com/feed.xml, pulls the <item><title> strings
// out of it, and lays them out as a newspaper page on the 3.97" panel.
//
// ponytail: no XML parser. RSS titles are plain text between two fixed tags,
// so a scan for "<item>" then "<title>" is the whole job. Add a real parser
// only if we ever need nested elements or attributes.

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_netif.h"

#include "page_news.h"
#include "epaper_port.h"
#include "GUI_Paint.h"
#include "button_bsp.h"
#include "pcf85063_bsp.h"

static const char *TAG = "news";

// Two sources. Function-press cycles between them; the choice is remembered
// for the session only.
#define FEED_EN_URL     "https://www.techmeme.com/feed.xml"      // ~21KB
#define FEED_HE_URL     "https://www.ynet.co.il/Integration/StoryRss2.xml"  // ~43KB
#define FEED_MAX_SIZE   (384 * 1024)  // headroom: Geektime's feed is ~300KB
#define MAX_HEADLINES   12
#define TITLE_MAX       200

// Canvas is 480x800 after the 270-degree rotation the other pages use.
#define CANVAS_W        480
#define CANVAS_H        800
#define MARGIN_X        10
#define BODY_TOP        62
#define BODY_BOTTOM     792

// Body font. The panel is ~236 DPI, so Font12 (8px glyphs) renders around 6pt
// and is genuinely hard to read at arm's length - it fits 7 stories nobody can
// read. Font16 halves the density but is legible, which is the point of a
// glanceable newspaper. Change these three together to retune.
#define BODY_FONT       Font16
#define GLYPH_W         16
#define LINE_H          30
#define WRAP_COLS       ((CANVAS_W - 2 * MARGIN_X) / GLYPH_W)   // 28

// Cap each story so one long headline cannot eat the page. Techmeme titles run
// to ~150 chars; 4 lines keeps the substance and drops the tail.
#define MAX_LINES       4

extern uint8_t *Image_Mono;
extern bool wifi_enable;
extern SemaphoreHandle_t rtc_mutex;

static void strip_source_suffix(char *s);   // defined with the render code

static bool  s_hebrew = false;          // which feed is showing
static char  s_titles[MAX_HEADLINES][TITLE_MAX];
static int   s_title_count = 0;

static void refresh_page_news(void)
{
    EPD_Display_Partial(Image_Mono, 0, 0, EPD_WIDTH, EPD_HEIGHT);
}

// ---------------------------------------------------------------- fetch ----

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static size_t total_len = 0;
    static char *buffer = NULL;

    switch (evt->event_id) {
    case HTTP_EVENT_ON_CONNECTED:
        total_len = 0;
        buffer = (char *)evt->user_data;
        break;
    case HTTP_EVENT_ON_DATA:
        if (!buffer) return ESP_FAIL;
        if (total_len + evt->data_len < FEED_MAX_SIZE - 1) {
            memcpy(buffer + total_len, evt->data, evt->data_len);
            total_len += evt->data_len;
            buffer[total_len] = '\0';
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        buffer = NULL;
        break;
    default:
        break;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------- parse ----

// Decodes the handful of XML entities Techmeme actually emits, in place, and
// drops anything non-ASCII. The bundled fonts are ASCII-only, so a stray UTF-8
// byte would render as garbage; dropping whole bytes keeps the string valid.
static void sanitize(char *s)
{
    // Replacements are strings, not chars, because &hellip; needs three.
    // Every entity here was observed in the live feed on 2026-08-25; the
    // counts were mdash 19, nbsp 36, hellip 7, apos 6, quot 4, ldquo/rdquo 6,
    // ntilde 3. Without them all of these collapse to the '-' fallback below.
    static const struct { const char *ent; const char *rep; } kEntities[] = {
        {"&amp;",    "&"},   {"&lt;",     "<"},   {"&gt;",     ">"},
        {"&quot;",   "\""},  {"&apos;",   "'"},   {"&#39;",    "'"},
        {"&#8217;",  "'"},   {"&#8216;",  "'"},   {"&#8220;",  "\""},
        {"&#8221;",  "\""},  {"&nbsp;",   " "},   {"&mdash;",  " - "},
        {"&ndash;",  "-"},   {"&hellip;", "..."}, {"&ldquo;",  "\""},
        {"&rdquo;",  "\""},  {"&lsquo;",  "'"},   {"&rsquo;",  "'"},
        {"&ntilde;", "n"},   {"&eacute;", "e"},   {"&uuml;",   "u"},
    };

    char *r = s, *w = s;
    while (*r) {
        if (*r == '&') {
            bool matched = false;
            for (size_t i = 0; i < sizeof(kEntities) / sizeof(kEntities[0]); i++) {
                size_t n = strlen(kEntities[i].ent);
                if (strncmp(r, kEntities[i].ent, n) == 0) {
                    // Replacements are never longer than the entity they
                    // replace, so writing in place can't overrun the read head.
                    for (const char *q = kEntities[i].rep; *q; q++) *w++ = *q;
                    r += n;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
            // Unknown entity: skip to ';' so we never print raw "&#8212;".
            char *semi = strchr(r, ';');
            if (semi && semi - r < 10) {
                *w++ = '-';
                r = semi + 1;
                continue;
            }
        }
        unsigned char c = (unsigned char)*r;
        if (c >= 0x80) {
            // Keep Hebrew (U+0590-U+05FF, UTF-8 0xD6 0x90 .. 0xD7 0xBF) because
            // we have glyphs for it; drop every other non-ASCII sequence whole,
            // never byte-by-byte, or the survivors become invalid UTF-8.
            int n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
            bool hebrew = (n == 2 && (c == 0xD6 || c == 0xD7));
            for (int i = 0; i < n && r[i]; i++) if (hebrew) *w++ = r[i];
            while (n-- && *r) r++;
            continue;
        }
        r++;
        *w++ = (c == '\n' || c == '\t' || c == '\r') ? ' ' : (char)c;
    }
    *w = '\0';
}

// Pulls <item><title>...</title> into s_titles. Skips the channel <title>,
// which appears before the first <item>.
static int parse_feed(const char *xml)
{
    s_title_count = 0;
    const char *p = xml;

    while (s_title_count < MAX_HEADLINES) {
        const char *item = strstr(p, "<item>");
        if (!item) break;

        const char *open = strstr(item, "<title>");
        if (!open) break;
        open += 7;

        const char *close = strstr(open, "</title>");
        if (!close) break;

        // Ynet wraps titles in CDATA; Techmeme does not. Unwrap before copying
        // or the markers render literally.
        if (strncmp(open, "<![CDATA[", 9) == 0) {
            const char *cd_end = strstr(open, "]]>");
            if (cd_end && cd_end < close) { open += 9; close = cd_end; }
        }

        size_t len = (size_t)(close - open);
        if (len >= TITLE_MAX) len = TITLE_MAX - 1;
        memcpy(s_titles[s_title_count], open, len);
        s_titles[s_title_count][len] = '\0';
        sanitize(s_titles[s_title_count]);
        if (!s_hebrew) strip_source_suffix(s_titles[s_title_count]);

        if (s_titles[s_title_count][0]) s_title_count++;
        p = close + 8;
    }

    ESP_LOGI(TAG, "parsed %d headlines", s_title_count);
    return s_title_count;
}

static bool fetch_feed(char *buf)
{
    buf[0] = '\0';

    esp_http_client_config_t config = {};
    config.url               = s_hebrew ? FEED_HE_URL : FEED_EN_URL;
    config.event_handler     = http_event_handler;
    config.user_data         = buf;
    config.crt_bundle_attach = esp_crt_bundle_attach;   // HTTPS: feed 301s off http
    config.timeout_ms        = 20000;
    config.user_agent        = "esp32-eink-news/1.0";

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP failed: %s", esp_err_to_name(err));
        return false;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "HTTP status %d", status);
        return false;
    }
    ESP_LOGI(TAG, "fetched %d bytes", (int)strlen(buf));
    return true;
}

// --------------------------------------------------------------- render ----

#include "hebrew.inc"

// Techmeme appends "(Author/Publication)" to every headline. It is the least
// useful text on a glanceable display and costs ~25 chars, so drop it. Scans
// from the end and balances parens so a headline containing "(EU)" mid-sentence
// is left alone.
static void strip_source_suffix(char *s)
{
    int len = (int)strlen(s);
    if (len < 4 || s[len - 1] != ')') return;

    int depth = 0;
    for (int i = len - 1; i >= 0; i--) {
        if (s[i] == ')') depth++;
        else if (s[i] == '(') {
            if (--depth == 0) {
                while (i > 0 && s[i - 1] == ' ') i--;
                if (i > 0) s[i] = '\0';       // never blank the whole title
                return;
            }
        }
    }
}

// Draws one headline word-wrapped at WRAP_COLS, capped at MAX_LINES with an
// ellipsis. Returns the y below it, or -1 if it would not fit, so the caller
// stops cleanly at the page bottom.
static int draw_wrapped(int y, const char *text)
{
    char line[WRAP_COLS + 4];
    const char *p = text;

    for (int n = 0; *p && n < MAX_LINES; n++) {
        // Take up to WRAP_COLS chars, then back off to the last space so we
        // break on a word boundary rather than mid-token.
        int take = 0;
        while (p[take] && take < WRAP_COLS) take++;
        if (p[take]) {
            int brk = take;
            while (brk > 0 && p[brk] != ' ') brk--;
            if (brk > 0) take = brk;
        }

        memcpy(line, p, take);
        line[take] = '\0';

        p += take;
        while (*p == ' ') p++;

        // More text than we can show: mark the cut on the last visible line.
        if (*p && n == MAX_LINES - 1) {
            int cut = take;
            if (cut > WRAP_COLS - 3) cut = WRAP_COLS - 3;
            memcpy(line + cut, "...", 4);
        }

        if (y + LINE_H > BODY_BOTTOM) return -1;
        Paint_DrawString_EN(MARGIN_X, y, line, &BODY_FONT, WHITE, BLACK);
        y += LINE_H;
    }
    return y;
}

static void draw_message(const char *l1, const char *l2)
{
    Paint_NewImage(Image_Mono, EPD_WIDTH, EPD_HEIGHT, 270, WHITE);
    Paint_SetScale(2);
    Paint_SelectImage(Image_Mono);
    Paint_Clear(WHITE);
    Paint_DrawString_EN(MARGIN_X, 60, l1, &Font16, WHITE, BLACK);
    if (l2) Paint_DrawString_EN(MARGIN_X, 100, l2, &Font16, WHITE, BLACK);
    refresh_page_news();
}

static void draw_page(void)
{
    Paint_NewImage(Image_Mono, EPD_WIDTH, EPD_HEIGHT, 270, WHITE);
    Paint_SetScale(2);
    Paint_SelectImage(Image_Mono);
    Paint_Clear(WHITE);

    // Masthead
    Paint_DrawString_EN(MARGIN_X, 8, s_hebrew ? "YNET" : "TECHMEME", &Font24, WHITE, BLACK);

    char stamp[32] = {0};
    xSemaphoreTake(rtc_mutex, portMAX_DELAY);
    Time_data t = PCF85063_GetTime();
    xSemaphoreGive(rtc_mutex);
    snprintf(stamp, sizeof(stamp), "%02d-%02d %02d:%02d",
             t.months, t.days, t.hours, t.minutes);
    Paint_DrawString_EN(CANVAS_W - MARGIN_X - (int)strlen(stamp) * GLYPH_W, 26,
                        stamp, &Font12, WHITE, BLACK);

    Paint_DrawLine(2, 54, CANVAS_W - 2, 54, BLACK, DOT_PIXEL_2X2, LINE_STYLE_SOLID);

    int y = BODY_TOP;
    int shown = 0;
    for (int i = 0; i < s_title_count; i++) {
        int next = s_hebrew
                 ? he_draw_wrapped(y, MARGIN_X, CANVAS_W - MARGIN_X, s_titles[i], MAX_LINES)
                 : draw_wrapped(y, s_titles[i]);
        if (next < 0) break;              // out of page
        y = next + 6;

        // Rule between stories, but not trailing off the last one.
        if (i + 1 < s_title_count && y + LINE_H + 8 < BODY_BOTTOM) {
            Paint_DrawLine(MARGIN_X, y, CANVAS_W - MARGIN_X, y,
                           BLACK, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
            y += 10;
        }
        shown++;
    }

    ESP_LOGI(TAG, "rendered %d/%d headlines", shown, s_title_count);
    refresh_page_news();
}

// ----------------------------------------------------------------- entry ---

void page_news_show(void)
{
    if (!wifi_enable) {
        ESP_LOGW(TAG, "WiFi is off");
        draw_message("WiFi is off. Enable it in Network,",
                     "then double-click Function to go back.");
        EPD_Sleep();
        while (1) {
            int button = wait_key_event_and_return_code(portMAX_DELAY);
            if (button == 8 || button == 22) { EPD_Init(); refresh_page_news(); return; }
        }
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        ESP_LOGW(TAG, "no IP address");
        draw_message("No IP address yet. Check the network,",
                     "then double-click Function to go back.");
        EPD_Sleep();
        while (1) {
            int button = wait_key_event_and_return_code(portMAX_DELAY);
            if (button == 8 || button == 22) { EPD_Init(); refresh_page_news(); return; }
        }
    }

    while (1) {
        draw_message(s_hebrew ? "Fetching Ynet..." : "Fetching Techmeme...", NULL);

        char *buf = (char *)heap_caps_malloc(FEED_MAX_SIZE, MALLOC_CAP_SPIRAM);
        if (!buf) {
            ESP_LOGE(TAG, "PSRAM allocation failed");
            draw_message("Out of memory.",
                         "Double-click Function to go back.");
        } else {
            bool ok = fetch_feed(buf);
            int n = ok ? parse_feed(buf) : 0;
            heap_caps_free(buf);

            if (!ok) {
                draw_message("Could not reach the feed.",
                             "Function: other feed   Double-click: back");
            } else if (n == 0) {
                draw_message("Feed had no headlines.",
                             "Function: retry   Double-click: back");
            } else {
                draw_page();
            }
        }

        EPD_Sleep();

        // Dispatch on SPECIFIC codes. One physical press emits several events -
        // Function alone fires 9 (Press), 7 (Click) and 10 (Bounce up) - so a
        // catch-all "anything else" branch toggles the feed two or three times
        // per press and lands you back where you started. Everything not named
        // here is deliberately ignored.
        //
        //   7 = Function click        8 = Function double-click
        //   0 = Up click             14 = Down click
        //  21 = Boot click           22 = Boot double-click
        while (1) {
            int button = wait_key_event_and_return_code(portMAX_DELAY);

            if (button == 8 || button == 22) {          // back to the menu
                EPD_Init();
                refresh_page_news();
                return;
            }
            if (button == 7) {                          // switch feed
                EPD_Init();
                s_hebrew = !s_hebrew;
                break;
            }
            if (button == 0 || button == 14) {          // reload the same feed
                EPD_Init();
                break;
            }
            // press / bounce / repeat / long-press: not ours, keep waiting
        }
    }
}
