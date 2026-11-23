#include "delivery.h"
#include "monotonic.h"

void delivery_begin(delivery_t *s, uint32_t sequence, int64_t now_ms) {
    *s = (delivery_t){.sequence = sequence, .sent_ms = now_ms, .pending = true};
}

bool delivery_ack(delivery_t *s, uint32_t sequence) {
    if (!s->pending || s->sequence != sequence)
        return false;
    s->pending = false;
    return true;
}

delivery_action_t delivery_poll(delivery_t *s, int64_t now_ms) {
    if (!s->pending || !wd_elapsed_ms(now_ms, s->sent_ms, 2000))
        return DELIVERY_WAIT;
    if (s->retries == 3) {
        s->pending = false;
        return DELIVERY_TIMEOUT;
    }
    ++s->retries;
    s->sent_ms = now_ms;
    return DELIVERY_RETRY;
}
