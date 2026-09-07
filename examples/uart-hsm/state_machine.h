/**
 * @file state_machine.h
 * @brief A lightweight, hierarchical state machine framework for bare-metal MCU.
 *
 * This framework provides a data-driven approach to creating hierarchical state machines (HSMs).
 * Ported from RT-Thread version to work without RTOS dependencies.
 *
 * Original copyright:
 * Copyright (c) 2013 Andreas Misje (MIT License)
 */

#ifndef STATE_MACHINE_H__
#define STATE_MACHINE_H__

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- User-configurable Macros --- */

/**
 * @brief User-definable assertion macro.
 * Maps to ASSERT from types.h by default.
 */
#ifndef HSM_ASSERT
#define HSM_ASSERT(expr) ASSERT(expr)
#endif

/* --- Core Types --- */

typedef struct hsm hsm_t;
typedef struct hsm_state hsm_state_t;
typedef struct hsm_event hsm_event_t;
typedef struct hsm_transition hsm_transition_t;

/**
 * @brief Event structure passed to the state machine.
 */
struct hsm_event
{
    uint32_t id;       /**< Application-specific event identifier. */
    void *context;     /**< Optional pointer to event-specific data. */
};

/**
 * @brief Defines the type of a state transition.
 */
typedef enum
{
    /**
     * @brief An external transition. Causes exit from source state and entry to target state.
     * If source and target are the same, it's a self-transition which will execute exit and entry actions.
     */
    SM_TRANSITION_EXTERNAL = 0,

    /**
     * @brief An internal transition. Executes only the action, without any exit or entry calls.
     * The state does not change. The target state in the transition table is ignored.
     */
    SM_TRANSITION_INTERNAL
} hsm_transition_type_t;

typedef void (*hsm_action_fn)(hsm_t *sm, const hsm_event_t *event);
typedef bool_t (*hsm_guard_fn)(hsm_t *sm, const hsm_event_t *event);

/**
 * @brief Defines a single state transition rule.
 */
struct hsm_transition
{
    uint32_t event_id;                   /**< The event ID that triggers this transition. */
    const hsm_state_t *target;           /**< The target state (ignored for internal transitions). */
    hsm_guard_fn guard;                  /**< Optional guard condition. Transition occurs if it returns true or is NULL. */
    hsm_action_fn action;                /**< Optional action executed during the transition. */
    hsm_transition_type_t type;          /**< The type of the transition (external or internal). */
};

/**
 * @brief Defines a state and its behavior.
 */
struct hsm_state
{
    const hsm_state_t *parent;           /**< Pointer to parent (super) state, or NULL for top-level states. */
    hsm_action_fn entry_action;          /**< Optional action executed upon entering the state. */
    hsm_action_fn exit_action;           /**< Optional action executed upon exiting the state. */
    const hsm_transition_t *transitions; /**< Pointer to the state's transition table. */
    size_t num_transitions;              /**< Number of transitions in the table. */
    const char *name;                    /**< Optional name for debugging. */
};

/**
 * @brief The state machine instance.
 */
struct hsm
{
    const hsm_state_t *current_state;       /**< The current active state. */
    const hsm_state_t *initial_state;       /**< The initial state for hsm_reset. */
    void *user_data;                        /**< Optional pointer to user-specific data. */
    hsm_action_fn unhandled_event_hook;     /**< Optional hook for unhandled events. */
    const hsm_state_t **entry_path_buffer;  /**< User-provided buffer for transition calculations. */
    uint8_t buffer_size;                    /**< Size of the user-provided buffer. */
};

/* --- Public API --- */

/**
 * @brief Initializes a state machine instance.
 * @param[out] sm                 Pointer to the state machine instance.
 * @param[in]  initial_state      Pointer to the initial state.
 * @param[in]  entry_path_buffer  A user-provided buffer to calculate transition paths. Its size must be >= max hierarchy depth.
 * @param[in]  buffer_size        The size of the entry_path_buffer.
 * @param[in]  user_data          Optional pointer to user data, can be NULL.
 * @param[in]  unhandled_hook     Optional hook for unhandled events, can be NULL.
 */
void hsm_init(hsm_t *sm,
              const hsm_state_t *initial_state,
              const hsm_state_t **entry_path_buffer,
              uint8_t buffer_size,
              void *user_data,
              hsm_action_fn unhandled_hook);

/**
 * @brief Deinitializes the state machine instance, clearing internal pointers.
 * @param[out] sm Pointer to the state machine instance.
 */
void hsm_deinit(hsm_t *sm);

/**
 * @brief Resets the state machine to its initial state.
 * @param[in,out] sm Pointer to the state machine instance.
 */
void hsm_reset(hsm_t *sm);

/**
 * @brief Dispatches an event to the state machine.
 * @param[in,out] sm    Pointer to the state machine instance.
 * @param[in]     event Pointer to the event to process.
 * @return TRUE if the event was handled, FALSE otherwise.
 */
bool_t hsm_dispatch(hsm_t *sm, const hsm_event_t *event);

/**
 * @brief Checks if the current state is a specific state or a substate of it.
 * @param[in] sm    Pointer to the state machine instance.
 * @param[in] state The state to check against (the potential parent state).
 * @return TRUE if the current state is `state` or one of its children, FALSE otherwise.
 */
bool_t hsm_is_in_state(const hsm_t *sm, const hsm_state_t *state);

/**
 * @brief Gets the name of the current state.
 * @param[in] sm Pointer to the state machine instance.
 * @return The name string of the current state, or "Unknown" if not available.
 */
const char *hsm_get_current_state_name(const hsm_t *sm);

#ifdef __cplusplus
}
#endif

#endif /* STATE_MACHINE_H__ */
