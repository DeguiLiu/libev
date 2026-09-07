/*
 * Minimal hierarchical state machine (HSM) engine, shared by the C examples
 * fs-hsm.c and hsm-echo.c. Extracted verbatim from those files to remove the
 * two duplicate embedded copies (they were identical except for the path
 * array size: `path[8]` vs `path[HSM_DEPTH]`, both 8).
 *
 * Data-driven: states carry parent / entry / exit and a transition table;
 * hsm_dispatch resolves (event id) by bubbling up the parent chain.
 */
#ifndef EXAMPLES_C_HSM_H
#define EXAMPLES_C_HSM_H

#include <stdint.h>

typedef struct hsm hsm_t;
typedef struct hsm_state hsm_state_t;
typedef struct hsm_event hsm_event_t;
typedef struct hsm_transition hsm_transition_t;

struct hsm_event {
    uint32_t id;
    void *context;
};

enum {
    HSM_TRANSITION_EXTERNAL = 0,   /* exit source chain, enter target chain */
    HSM_TRANSITION_INTERNAL        /* run action only, stay in state       */
};

typedef void (*hsm_action_fn)(hsm_t *sm, const hsm_event_t *event);
typedef int32_t (*hsm_guard_fn)(hsm_t *sm, const hsm_event_t *event); /* 1 pass */

struct hsm_transition {
    uint32_t id;
    const hsm_state_t *target;     /* NULL for internal transitions */
    hsm_guard_fn guard;            /* NULL = always passes */
    hsm_action_fn action;          /* NULL = none */
    int32_t type;
};

struct hsm_state {
    const hsm_state_t *parent;
    hsm_action_fn entry_action;
    hsm_action_fn exit_action;
    const hsm_transition_t *transitions;
    uint32_t num_transitions;
    const char *name;
};

#define HSM_MAX_DEPTH 8

struct hsm {
    const hsm_state_t *current;
    const hsm_state_t *initial;
    const hsm_state_t *path[HSM_MAX_DEPTH];
    void *user_data;
};

void hsm_init(hsm_t *sm, const hsm_state_t *initial, void *user_data);
int32_t hsm_dispatch(hsm_t *sm, const hsm_event_t *e);
int32_t hsm_is_in(const hsm_t *sm, const hsm_state_t *state);
void hsm_perform_transition(hsm_t *sm, const hsm_state_t *target, const hsm_event_t *e);

#endif /* EXAMPLES_C_HSM_H */
