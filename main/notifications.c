#include "notifications.h"
#include "ui.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>
#include <stdbool.h>
#include "music.h"

static const char *TAG = "notif";

#define RX_BUF_SIZE 2048

static notification_t s_notifs[MAX_NOTIFICATIONS];
static int            s_notif_count = 0;
static char           s_rx_buf[RX_BUF_SIZE];
static size_t         s_rx_len = 0;

// --- tiny JSON helpers ----------------------------------------------------
// Find key "foo" in json; return pointer to char after "foo": or NULL.
static const char *find_key_value(const char *json, const char *key)
{
    char pattern[64];
    int n = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (n <= 0 || n >= (int)sizeof(pattern)) return NULL;

    const char *p = json;
    while ((p = strstr(p, pattern)) != NULL) {
        const char *after = p + strlen(pattern);
        // Skip spaces
        while (*after == ' ' || *after == '\t') after++;
        if (*after == ':') return after + 1;
        p = after;
    }
    return NULL;
}

// Read a JSON string value starting at p (which points after the ':').
// Returns false if not a string, otherwise writes to out and returns true.
static bool read_string_value(const char *p, char *out, size_t out_sz)
{
    if (!p) return false;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i < out_sz - 1) {
        if (*p == '\\' && *(p + 1)) {
            char nx = *(p + 1);
            if      (nx == 'n')  out[i++] = '\n';
            else if (nx == 't')  out[i++] = '\t';
            else if (nx == '"')  out[i++] = '"';
            else if (nx == '\\') out[i++] = '\\';
            else                 out[i++] = nx;
            p += 2;
        } else {
            out[i++] = *p++;
        }
    }
    out[i] = '\0';
    return true;
}

// Read a JSON integer value.
static int read_int_value(const char *p)
{
    if (!p) return 0;
    while (*p == ' ' || *p == '\t') p++;
    return (int)strtol(p, NULL, 10);
}

static bool json_get_string(const char *json, const char *key, char *out, size_t out_sz)
{
    return read_string_value(find_key_value(json, key), out, out_sz);
}

// --- Allowlist ------------------------------------------------------------
static bool str_contains_i(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0) return true;
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, nlen) == 0) return true;
    }
    return false;
}

static const char *s_allowed[] = {
    "WhatsApp",
    "Instagram",
    "TikTok",
    "Gmail", "Email", "Outlook", "Mail",
    NULL
};

static bool is_allowed_source(const char *src)
{
    if (!src || !*src) return false;
    for (int i = 0; s_allowed[i]; i++) {
        if (str_contains_i(src, s_allowed[i])) return true;
    }
    return false;
}

static void push_notification(const notification_t *n)
{
    int keep = (s_notif_count < MAX_NOTIFICATIONS) ? s_notif_count : MAX_NOTIFICATIONS - 1;
    for (int i = keep; i > 0; i--) s_notifs[i] = s_notifs[i - 1];
    s_notifs[0] = *n;
    if (s_notif_count < MAX_NOTIFICATIONS) s_notif_count++;
}

static void handle_notify_json(const char *json)
{
    ESP_LOGI(TAG, "handle_notify: %.100s", json);
    notification_t n = { 0 };

    if (!json_get_string(json, "src", n.src, sizeof(n.src))) {
        strncpy(n.src, "Unknown", sizeof(n.src) - 1);
    }
    if (!is_allowed_source(n.src)) {
        ESP_LOGI(TAG, "filtered: src=\"%s\"", n.src);
        return;
    }
    if (!json_get_string(json, "title", n.title, sizeof(n.title))) {
        json_get_string(json, "sender", n.title, sizeof(n.title));
    }
    json_get_string(json, "body", n.body, sizeof(n.body));
    n.id = (uint32_t)read_int_value(find_key_value(json, "id"));
    n.timestamp_us = esp_timer_get_time();

    push_notification(&n);
    ESP_LOGI(TAG, "[%s] %s: %s", n.src, n.title, n.body);
    ui_notification_added();
}

static void process_line(char *line)
{
    ESP_LOGI(TAG, "line (%u chars): %.100s", (unsigned)strlen(line), line);

    
    while (*line == ' ' || *line == '\r' || *line == '\t' || (unsigned char)*line == 0x10) line++;

    char *json_start = line;
    if (strncmp(line, "GB(", 3) == 0) {
        json_start = line + 3;
        char *end = strrchr(json_start, ')');
        if (end) *end = '\0';
    } else if (*line != '{') {
        return;
    }

    // Get the "t" (type) field to decide what to do.
    char type_buf[32] = { 0 };
    if (!json_get_string(json_start, "t", type_buf, sizeof(type_buf))) return;

    if (strcmp(type_buf, "notify") == 0) {
        handle_notify_json(json_start);
    } else if (strcmp(type_buf, "musicstate") == 0) {
        char st[16] = {0};
        if (json_get_string(json_start, "state", st, sizeof(st))) {
            if      (strcmp(st, "play")  == 0) music_update_state(MUSIC_STATE_PLAY);
            else if (strcmp(st, "pause") == 0) music_update_state(MUSIC_STATE_PAUSE);
        }
    } else if (strcmp(type_buf, "musicinfo") == 0) {
        char track[96] = {0};
        char artist[64] = {0};
        json_get_string(json_start, "track", track, sizeof(track));
        json_get_string(json_start, "artist", artist, sizeof(artist));
        music_update_info(track, artist);
    }   
    // notify-, call, musicinfo, setTime will come in Phase 3c.
}

// --- Public API -----------------------------------------------------------
void notifications_init(void)
{
    s_notif_count = 0;
    s_rx_len = 0;
    ESP_LOGI(TAG, "ready");
}

void notifications_handle_rx(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\n' || c == '\r') {
            if (s_rx_len > 0) {
                s_rx_buf[s_rx_len] = '\0';
                process_line(s_rx_buf);
                s_rx_len = 0;
            }
            continue;
        }
        if (s_rx_len < RX_BUF_SIZE - 1) {
            s_rx_buf[s_rx_len++] = c;
        } else {
            ESP_LOGW(TAG, "rx overflow, resetting");
            s_rx_len = 0;
        }
    }
}

int notifications_count(void) { return s_notif_count; }

const notification_t *notifications_get(int index)
{
    if (index < 0 || index >= s_notif_count) return NULL;
    return &s_notifs[index];
}

void notifications_clear(void)
{
    s_notif_count = 0;
    ui_notification_added();
}