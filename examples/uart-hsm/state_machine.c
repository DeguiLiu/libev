/**
 * @file state_machine.c
 * @brief Hierarchical state machine implementation for bare-metal MCU.
 *
 * Ported from RT-Thread version to work without RTOS dependencies.
 *
 * Original copyright:
 * Copyright (c) 2013 Andreas Misje (MIT License)
 */

#include "state_machine.h"

/* --- Private Helper Function Declarations --- */
static uint8_t hsm_get_state_depth(const hsm_state_t *state);
static const hsm_state_t *hsm_find_lca(const hsm_state_t *s1, const hsm_state_t *s2);
static void hsm_perform_transition(hsm_t *sm, const hsm_state_t *target_state, const hsm_event_t *event);
static const hsm_transition_t *hsm_find_matching_transition(const hsm_state_t *state, const hsm_event_t *event, bool_t *guard_passed, hsm_t *sm);
static bool_t hsm_execute_transition(hsm_t *sm, const hsm_transition_t *transition, const hsm_event_t *event);
static bool_t hsm_process_state_transitions(hsm_t *sm, const hsm_state_t *state, const hsm_event_t *event);
static void hsm_execute_exit_actions(hsm_t *sm, const hsm_state_t *source_state, const hsm_state_t *lca, const hsm_event_t *event);
static bool_t hsm_build_entry_path(hsm_t *sm, const hsm_state_t *target_state, const hsm_state_t *lca);
static void hsm_execute_entry_actions(hsm_t *sm, uint8_t path_length, const hsm_event_t *event);

/* --- Public API Implementation --- */

void hsm_init(hsm_t *sm,
              const hsm_state_t *initial_state,
              const hsm_state_t **entry_path_buffer,
              uint8_t buffer_size,
              void *user_data,
              hsm_action_fn unhandled_hook)
{
    bool_t valid_parameters = FALSE;

    /* Parameter validation */
    if ((sm != NULL) && (initial_state != NULL) &&
        (entry_path_buffer != NULL) && (buffer_size > 0U))
    {
        valid_parameters = TRUE;
    }

    if (valid_parameters == TRUE)
    {
        /* Assign user data and hooks first, as they might be used in the initial entry actions */
        sm->user_data = user_data;
        sm->unhandled_event_hook = unhandled_hook;
        sm->initial_state = initial_state;
        sm->entry_path_buffer = entry_path_buffer;
        sm->buffer_size = buffer_size;
        sm->current_state = NULL; /* Setting to NULL ensures the full entry path is executed on first transition */

        /* Perform the initial transition */
        hsm_perform_transition(sm, initial_state, NULL);
    }
}

void hsm_deinit(hsm_t *sm)
{
    if (sm != NULL)
    {
        sm->current_state = NULL;
        sm->initial_state = NULL;
        sm->user_data = NULL;
        sm->unhandled_event_hook = NULL;
        sm->entry_path_buffer = NULL;
        sm->buffer_size = 0U;
    }
}

void hsm_reset(hsm_t *sm)
{
    bool_t valid_parameters = FALSE;

    /* Parameter validation */
    if ((sm != NULL) && (sm->initial_state != NULL))
    {
        valid_parameters = TRUE;
    }

    if (valid_parameters == TRUE)
    {
        /* Transition from the current state to the initial state */
        hsm_perform_transition(sm, sm->initial_state, NULL);
    }
}

bool_t hsm_dispatch(hsm_t *sm, const hsm_event_t *event)
{
    bool_t is_handled = FALSE;
    const hsm_state_t *state_iter = NULL;
    bool_t continue_processing = TRUE;
    bool_t valid_parameters = FALSE;

    /* Parameter validation */
    if ((sm != NULL) && (event != NULL))
    {
        valid_parameters = TRUE;
        state_iter = sm->current_state;
    }

    if (valid_parameters == TRUE)
    {
        /* Process event through state hierarchy */
        while ((state_iter != NULL) && (continue_processing == TRUE))
        {
            /* Try to process transitions for this state */
            if (hsm_process_state_transitions(sm, state_iter, event) == TRUE)
            {
                is_handled = TRUE;
                continue_processing = FALSE; /* Event handled, stop bubbling up */
            }
            else
            {
                /* If not handled at this level, bubble up to parent */
                state_iter = state_iter->parent;
            }
        }

        /* Call unhandled event hook if event was not processed */
        if ((is_handled == FALSE) && (sm->unhandled_event_hook != NULL))
        {
            sm->unhandled_event_hook(sm, event);
        }
    }

    return is_handled;
}

bool_t hsm_is_in_state(const hsm_t *sm, const hsm_state_t *state)
{
    bool_t is_in_state = FALSE;
    const hsm_state_t *current_iter = NULL;
    bool_t continue_search = TRUE;
    bool_t valid_parameters = FALSE;

    /* Parameter validation */
    if ((sm != NULL) && (state != NULL))
    {
        valid_parameters = TRUE;
        current_iter = sm->current_state;
    }

    if (valid_parameters == TRUE)
    {
        /* Search through state hierarchy */
        while ((current_iter != NULL) && (continue_search == TRUE))
        {
            if (current_iter == state)
            {
                is_in_state = TRUE;
                continue_search = FALSE; /* Found the state, stop searching */
            }
            else
            {
                current_iter = current_iter->parent;
            }
        }
    }

    return is_in_state;
}

const char *hsm_get_current_state_name(const hsm_t *sm)
{
    const char *name = "Unknown";

    /* Parameter validation and name retrieval */
    if ((sm != NULL) && (sm->current_state != NULL) && (sm->current_state->name != NULL))
    {
        name = sm->current_state->name;
    }

    return name;
}

/* --- Private Helper Implementations --- */

/**
 * @brief Performs the state transition logic, executing exit and entry actions.
 * @note This is the core transition function.
 */
static void hsm_perform_transition(hsm_t *sm, const hsm_state_t *target_state, const hsm_event_t *event)
{
    const hsm_state_t *source_state = NULL;
    const hsm_state_t *lca = NULL;
    bool_t same_state = FALSE;
    bool_t valid_parameters = FALSE;
    bool_t path_built = FALSE;

    /* Parameter validation */
    if ((sm != NULL) && (target_state != NULL))
    {
        valid_parameters = TRUE;
        source_state = sm->current_state;
        same_state = (source_state == target_state) ? TRUE : FALSE;
    }

    if (valid_parameters == TRUE)
    {
        if (same_state == TRUE)
        {
            /* External self-transition: execute exit then entry on the same state */
            if ((source_state != NULL) && (source_state->exit_action != NULL))
            {
                source_state->exit_action(sm, event);
            }
            if (target_state->entry_action != NULL)
            {
                target_state->entry_action(sm, event);
            }
        }
        else
        {
            /* Different states: perform hierarchical transition */
            lca = hsm_find_lca(source_state, target_state);
            hsm_execute_exit_actions(sm, source_state, lca, event);

            /* Build entry path and check if buffer is sufficient */
            path_built = hsm_build_entry_path(sm, target_state, lca);

            if (path_built == TRUE)
            {
                uint8_t path_length = 0U;
                const hsm_state_t *entry_iter = target_state;

                /* Calculate path length for entry actions */
                while ((entry_iter != NULL) && (entry_iter != lca))
                {
                    path_length++;
                    entry_iter = entry_iter->parent;
                }

                /* Update current state */
                sm->current_state = target_state;

                /* Execute entry actions */
                hsm_execute_entry_actions(sm, path_length, event);
            }
            else
            {
                /* Buffer insufficient for entry path */
                HSM_ASSERT(0);
            }
        }
    }
}

static uint8_t hsm_get_state_depth(const hsm_state_t *state)
{
    uint8_t depth = 0U;
    const hsm_state_t *current_state = state;

    /* Calculate depth by traversing parent chain */
    while (current_state != NULL)
    {
        depth++;
        current_state = current_state->parent;
    }
    return depth;
}

static const hsm_state_t *hsm_find_lca(const hsm_state_t *s1, const hsm_state_t *s2)
{
    const hsm_state_t *result = NULL;
    const hsm_state_t *p1 = NULL;
    const hsm_state_t *p2 = NULL;
    uint8_t depth1 = 0U;
    uint8_t depth2 = 0U;
    bool_t valid_parameters = FALSE;

    /* Parameter validation and initialization */
    if ((s1 != NULL) && (s2 != NULL))
    {
        valid_parameters = TRUE;
        p1 = s1;
        p2 = s2;
        depth1 = hsm_get_state_depth(p1);
        depth2 = hsm_get_state_depth(p2);
    }
    else if (s1 == NULL)
    {
        result = s2;
    }
    else if (s2 == NULL)
    {
        result = s1;
    }
    else
    {
        /* Both are NULL - should not happen in normal operation */
        result = NULL;
    }

    if (valid_parameters == TRUE)
    {
        /* Normalize depths to same level */
        while (depth1 > depth2)
        {
            p1 = p1->parent;
            depth1--;
        }
        while (depth2 > depth1)
        {
            p2 = p2->parent;
            depth2--;
        }

        /* Find common ancestor */
        while (p1 != p2)
        {
            p1 = p1->parent;
            p2 = p2->parent;
        }
        result = p1;
    }

    return result;
}

/**
 * @brief Finds a matching transition for the given event in the state's transition table.
 * @param[in] state The state to search for transitions.
 * @param[in] event The event to match.
 * @param[out] guard_passed Set to true if a matching transition's guard passes, false otherwise.
 * @param[in] sm The state machine instance (needed for guard evaluation).
 * @return Pointer to the matching transition, or NULL if none found.
 */
static const hsm_transition_t *hsm_find_matching_transition(const hsm_state_t *state, const hsm_event_t *event, bool_t *guard_passed, hsm_t *sm)
{
    const hsm_transition_t *result = NULL;
    size_t i = 0U;
    bool_t found = FALSE;
    bool_t valid_parameters = FALSE;

    /* Initialize output parameter */
    *guard_passed = FALSE;

    /* Parameter validation */
    if ((state != NULL) && (state->transitions != NULL) &&
        (event != NULL) && (state->num_transitions > 0U))
    {
        valid_parameters = TRUE;
    }

    if (valid_parameters == TRUE)
    {
        /* Search through all transitions to find first matching one with passing guard */
        while ((i < state->num_transitions) && (found == FALSE))
        {
            const hsm_transition_t *current_transition = &state->transitions[i];
            if (current_transition->event_id == event->id)
            {
                /* Evaluate guard condition */
                bool_t current_guard_passed = FALSE;
                if (current_transition->guard == NULL)
                {
                    current_guard_passed = TRUE;
                }
                else
                {
                    current_guard_passed = current_transition->guard(sm, event);
                }

                /* If guard passes, use this transition */
                if (current_guard_passed == TRUE)
                {
                    result = current_transition;
                    *guard_passed = TRUE;
                    found = TRUE;
                }
                /* If guard fails, continue searching for other transitions with same event */
            }
            i++;
        }
    }

    return result;
}

/**
 * @brief Executes a transition action and state change if needed.
 * @param[in,out] sm The state machine instance.
 * @param[in] transition The transition to execute.
 * @param[in] event The event being processed.
 * @return true if the transition was executed, false otherwise.
 */
static bool_t hsm_execute_transition(hsm_t *sm, const hsm_transition_t *transition, const hsm_event_t *event)
{
    bool_t executed = FALSE;
    bool_t valid_parameters = FALSE;

    /* Parameter validation */
    if ((sm != NULL) && (transition != NULL))
    {
        valid_parameters = TRUE;
    }

    if (valid_parameters == TRUE)
    {
        if (transition->type == SM_TRANSITION_INTERNAL)
        {
            /* Internal transition: only execute action */
            if (transition->action != NULL)
            {
                transition->action(sm, event);
            }
        }
        else
        {
            /* External transition: execute action and change state */
            if (transition->action != NULL)
            {
                transition->action(sm, event);
            }
            hsm_perform_transition(sm, transition->target, event);
        }
        executed = TRUE;
    }

    return executed;
}

static bool_t hsm_process_state_transitions(hsm_t *sm, const hsm_state_t *state, const hsm_event_t *event)
{
    bool_t handled = FALSE;
    const hsm_transition_t *matching_transition = NULL;
    bool_t guard_passed = FALSE;
    bool_t valid_parameters = FALSE;

    /* Parameter validation */
    if ((sm != NULL) && (state != NULL) && (event != NULL))
    {
        valid_parameters = TRUE;
    }

    if (valid_parameters == TRUE)
    {
        /* Find matching transition */
        matching_transition = hsm_find_matching_transition(state, event, &guard_passed, sm);

        if ((matching_transition != NULL) && (guard_passed == TRUE))
        {
            /* Execute the transition */
            if (hsm_execute_transition(sm, matching_transition, event) == TRUE)
            {
                handled = TRUE;
            }
        }
        /* If no valid transition found (either no matching event or all guards failed),
           simply continue - this is normal behavior for hierarchical state machines */
    }

    return handled;
}

/**
 * @brief Executes exit actions from source state up to (but not including) the LCA.
 * @param[in,out] sm The state machine instance.
 * @param[in] source_state The source state to start from.
 * @param[in] lca The lowest common ancestor state (exit actions stop before this state).
 * @param[in] event The event being processed.
 */
static void hsm_execute_exit_actions(hsm_t *sm, const hsm_state_t *source_state, const hsm_state_t *lca, const hsm_event_t *event)
{
    const hsm_state_t *exit_iter = source_state;
    while ((exit_iter != NULL) && (exit_iter != lca))
    {
        if (exit_iter->exit_action != NULL)
        {
            exit_iter->exit_action(sm, event);
        }
        exit_iter = exit_iter->parent;
    }
}

/**
 * @brief Builds the entry path from target state down to (but not including) the LCA.
 * @param[in,out] sm The state machine instance.
 * @param[in] target_state The target state to build path from.
 * @param[in] lca The lowest common ancestor state (path building stops before this state).
 * @return true if path was built successfully, false if buffer insufficient.
 */
static bool_t hsm_build_entry_path(hsm_t *sm, const hsm_state_t *target_state, const hsm_state_t *lca)
{
    bool_t success = TRUE;
    uint8_t path_idx = 0U;
    const hsm_state_t *entry_iter = target_state;
    bool_t valid_parameters = FALSE;

    /* Parameter validation */
    if ((sm != NULL) && (target_state != NULL))
    {
        valid_parameters = TRUE;
    }

    if (valid_parameters == TRUE)
    {
        /* Build entry path from target state up to (but not including) the LCA */
        while ((entry_iter != NULL) && (entry_iter != lca) && (success == TRUE))
        {
            if (path_idx < sm->buffer_size)
            {
                sm->entry_path_buffer[path_idx] = entry_iter;
                path_idx++;
                entry_iter = entry_iter->parent;
            }
            else
            {
                success = FALSE;
            }
        }
    }
    else
    {
        success = FALSE;
    }

    return success;
}

/**
 * @brief Executes entry actions in reverse order from the entry path.
 * @param[in,out] sm The state machine instance.
 * @param[in] path_length The number of states in the entry path.
 * @param[in] event The event being processed.
 */
static void hsm_execute_entry_actions(hsm_t *sm, uint8_t path_length, const hsm_event_t *event)
{
    int8_t entry_idx = (int8_t)path_length - 1;
    bool_t valid_parameters = FALSE;

    /* Parameter validation */
    if ((sm != NULL) && (path_length > 0U))
    {
        valid_parameters = TRUE;
    }

    if (valid_parameters == TRUE)
    {
        /* Execute entry actions in reverse order (parent to child) */
        while (entry_idx >= 0)
        {
            if ((sm->entry_path_buffer[entry_idx] != NULL) &&
                (sm->entry_path_buffer[entry_idx]->entry_action != NULL))
            {
                sm->entry_path_buffer[entry_idx]->entry_action(sm, event);
            }
            entry_idx--;
        }
    }
}
