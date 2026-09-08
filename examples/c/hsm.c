/*
 * HSM engine implementation, extracted verbatim from fs-hsm.c / hsm-echo.c.
 * hsm_depth_of and hsm_lca stay file-static (only used here); the rest are
 * the public API declared in hsm.h.
 */
#include "hsm.h"

#include <stddef.h>

static uint32_t
hsm_depth_of (const hsm_state_t *s)
{
    uint32_t d = 0;

    while (s)
    {
        d++;
        s = s->parent;
    }

    return d;
}

static const hsm_state_t *
hsm_lca (const hsm_state_t *a, const hsm_state_t *b)
{
    uint32_t da = hsm_depth_of (a);
    uint32_t db = hsm_depth_of (b);

    while (da > db) { a = a->parent; da--; }
    while (db > da) { b = b->parent; db--; }

    while (a != b) { a = a->parent; b = b->parent; }

    return a;
}

void
hsm_perform_transition (hsm_t *sm, const hsm_state_t *target, const hsm_event_t *e)
{
    const hsm_state_t *src = sm->current;
    const hsm_state_t *lca;

    if (src == target)
    {
        /* external self-transition */
        if (src && src->exit_action)
            src->exit_action (sm, e);

        if (target->entry_action)
            target->entry_action (sm, e);

        return;
    }

    lca = hsm_lca (src, target);

    /* exit from src up to (not incl.) LCA */
    {
        const hsm_state_t *it = src;

        while (it && it != lca)
        {
            if (it->exit_action)
                it->exit_action (sm, e);

            it = it->parent;
        }
    }

    /* build entry path target..LCA (exclusive) */
    {
        uint32_t n = 0;
        const hsm_state_t *it = target;

        while (it && it != lca)
        {
            sm->path[n++] = it;
            it = it->parent;
        }

        /* enter parent-first */
        while (n)
        {
            n--;

            if (sm->path[n]->entry_action)
                sm->path[n]->entry_action (sm, e);
        }
    }

    sm->current = target;
}

/* dispatch: bubble up until some level's transition table handles it */
int32_t
hsm_dispatch (hsm_t *sm, const hsm_event_t *e)
{
    const hsm_state_t *s = sm->current;

    while (s)
    {
        uint32_t i;

        for (i = 0; i < s->num_transitions; i++)
        {
            const hsm_transition_t *t = &s->transitions[i];

            if (t->id != e->id)
                continue;

            if (t->guard && 0 == t->guard (sm, e))
                continue; /* guard failed: keep scanning same table */

            if (t->action)
                t->action (sm, e);

            if (HSM_TRANSITION_INTERNAL == t->type)
                return 1;

            hsm_perform_transition (sm, t->target, e);
            return 1;
        }

        s = s->parent; /* bubble up */
    }

    return 0;
}

void
hsm_init (hsm_t *sm, const hsm_state_t *initial, void *user_data)
{
    sm->current = NULL; /* forces full entry path on first transition */
    sm->initial = initial;
    sm->user_data = user_data;
    hsm_perform_transition (sm, initial, NULL);
}

int32_t
hsm_is_in (const hsm_t *sm, const hsm_state_t *state)
{
    const hsm_state_t *it = sm->current;

    while (it)
    {
        if (it == state)
            return 1;

        it = it->parent;
    }

    return 0;
}
