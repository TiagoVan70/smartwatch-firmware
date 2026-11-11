#include "music.h"
#include "ble_watch.h"
#include "ui.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "music";
static music_snapshot_t s_snap;

void music_init(void)
{
    memset(&s_snap, 0, sizeof(s_snap));
    s_snap.state = MUSIC_STATE_UNKNOWN;
}

void music_update_info(const char *track, const char *artist)
{
    if (track) {
        strncpy(s_snap.track, track, sizeof(s_snap.track) - 1);
        s_snap.track[sizeof(s_snap.track) - 1] = '\0';
    }
    if (artist) {
        strncpy(s_snap.artist, artist, sizeof(s_snap.artist) - 1);
        s_snap.artist[sizeof(s_snap.artist) - 1] = '\0';
    }
    s_snap.has_info = true;
    ESP_LOGI(TAG, "info: %s - %s", s_snap.artist, s_snap.track);
    ui_music_updated();
}

void music_update_state(music_state_t state)
{
    s_snap.state = state;
    ESP_LOGI(TAG, "state: %d", (int)state);
    ui_music_updated();
}

void music_set_local_state(music_state_t state) { s_snap.state = state; }

void music_get_snapshot(music_snapshot_t *out) { if (out) *out = s_snap; }

void music_send_control(const char *cmd)
{
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "\r\n{\"t\":\"music\",\"n\":\"%s\"}\r\n", cmd);
    if (n > 0 && n < (int)sizeof(buf)) {
        ble_watch_tx(buf);
        ESP_LOGI(TAG, "ctrl: %s", cmd);
    }
}