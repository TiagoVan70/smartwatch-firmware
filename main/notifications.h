#pragma once
#include <stddef.h>
#include <stdint.h>

#define MAX_NOTIFICATIONS 10

typedef struct {
    uint32_t id;
    char     src[32];
    char     title[64];
    char     body[256];
    int64_t  timestamp_us;
} notification_t;

void                   notifications_init(void);
void                   notifications_handle_rx(const uint8_t *data, size_t len);
int                    notifications_count(void);
const notification_t  *notifications_get(int index);   // 0 = newest
void                   notifications_clear(void);