/* Pure BOOT-button decision logic. See button_policy.h. */
#include "button_policy.h"

void bp_reset(bp_state_t *st) {
    if (!st) return;
    st->cleared = false;
    st->reset = false;
}

bp_action_t bp_on_release(unsigned ms) {
    /* A bounce is not a press; a long press was already handled by bp_on_hold() and must not also refresh. */
    if (ms < BP_DEBOUNCE_MS || ms >= BP_REFRESH_MAX_MS) return BP_NONE;
    return BP_REFRESH;
}

bp_action_t bp_on_hold(bp_state_t *st, unsigned ms) {
    if (!st) return BP_NONE;

    /* Highest crossed threshold wins on a single big jump; mark everything below it as fired so no lesser
     * action follows on a later poll. */
    if (ms >= BP_FACTORY_RESET_MS) {
        bool first = !st->reset;
        st->cleared = true;
        st->reset = true;
        return first ? BP_FACTORY_RESET : BP_NONE;
    }
    if (ms >= BP_CLEAR_PW_MS) {
        if (st->cleared) return BP_NONE;
        st->cleared = true;
        return BP_CLEAR_PW;
    }
    return BP_NONE;
}
