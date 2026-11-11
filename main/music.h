#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    MUSIC_STATE_UNKNOWN = 0,
    MUSIC_STATE_PLAY,
    MUSIC_STATE_PAUSE,
} music_state_t;

typedef struct {
    char track[96];
    char artist[64];
    music_state_t state;
    bool has_info;
} music_snapshot_t;

void music_init(void);

// Called by notifications.c when "musicstate"/"musicinfo" arrives
void music_update_info(const char *track, const char *artist);
void music_update_state(music_state_t state);

// For UI
void music_get_snapshot(music_snapshot_t *out);
void music_send_control(const char *cmd);         // "play","pause","next","previous"
void music_set_local_state(music_state_t state);  // optimistic