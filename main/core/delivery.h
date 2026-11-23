#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef enum { DELIVERY_WAIT, DELIVERY_RETRY, DELIVERY_TIMEOUT } delivery_action_t;
typedef struct {
    uint32_t sequence;
    int64_t sent_ms;
    unsigned retries;
    bool pending;
} delivery_t;

void delivery_begin(delivery_t *state, uint32_t sequence, int64_t now_ms);
bool delivery_ack(delivery_t *state, uint32_t sequence);
delivery_action_t delivery_poll(delivery_t *state, int64_t now_ms);
