// Minimal C++17 hierarchical state machine (HSM), modeled after coact's
// Hsm<Context>. Table-driven, zero-heap, no exceptions. Events are uint16_t
// signal ids; Context is the caller-owned shared state.
// SPDX-License-Identifier: MIT
#pragma once

#include <cassert>
#include <cstdint>

namespace hsm {

enum class TransitionKind : uint8_t {
    External,   // leave the active subtree, then enter the target
    Internal,   // run only the action, stay in the same state
    Self        // exit up to the source, run action, re-enter the source
};

template <typename Context>
struct StateDef {
    int8_t parent;                 // parent state index; 0 is the root; -1 = none
    void (*entry)(Context&);       // entry action, may be null
    void (*exit)(Context&);        // exit action, may be null
    const char* name = nullptr;    // debug label, may be null
    int8_t initial_child = -1;     // direct initial child; -1 = leaf
};

template <typename Context>
struct TransitionDef {
    int8_t source;                 // source state index; no wildcard
    uint16_t signal;               // event id
    int8_t target;                 // target state index (External only)
    TransitionKind kind;
    bool (*guard)(const Context&, uint16_t);   // may be null (pass); pure, no side effects
    void (*action)(Context&, uint16_t);        // may be null (no-op)
};

// Run-time HSM over caller-provided static tables. Stores only addresses,
// counts, the initial state and the max depth: never copies the tables and
// never allocates. Dispatch resolves (state, signal) from the active leaf
// upward, bounded by max_depth parent hops.
template <typename Context>
class Hsm {
public:
    Hsm(const StateDef<Context>* states, uint16_t num_states,
        const TransitionDef<Context>* transitions, uint16_t num_transitions,
        int8_t initial_state, uint8_t max_depth) noexcept
        : states_(states),
          num_states_(num_states),
          transitions_(transitions),
          num_transitions_(num_transitions),
          initial_state_(initial_state),
          max_depth_(max_depth),
          current_(-1)
    {
        validate_topology();
    }

    void init(Context& ctx) noexcept
    {
        if (initial_state_ < 0 ||
            static_cast<uint16_t>(initial_state_) >= num_states_)
        {
            return;
        }
        enter_path(ctx, -1, initial_state_);
        current_ = enter_initial_descendants(ctx, initial_state_);
    }

    bool dispatch(Context& ctx, uint16_t signal) noexcept
    {
        if (current_ < 0)
        {
            return false;
        }
        int8_t state = current_;
        uint8_t hops = 0U;
        for (;;)
        {
            const TransitionDef<Context>* tran = find_transition(ctx, signal, state);
            if (nullptr != tran)
            {
                execute_transition(ctx, signal, state, *tran);
                return true;
            }
            const int8_t parent = states_[state].parent;
            if (parent < 0)
            {
                return false;
            }
            if (hops >= max_depth_)
            {
                return false;
            }
            state = parent;
            ++hops;
        }
    }

    int8_t current_state() const noexcept { return current_; }

    const char* current_state_name() const noexcept
    {
        if (current_ < 0 || static_cast<uint16_t>(current_) >= num_states_)
        {
            return nullptr;
        }
        return states_[current_].name;
    }

private:
    void validate_topology() const noexcept
    {
        assert(num_states_ <= 128U);
        assert((0U == num_states_) || (nullptr != states_));
        for (uint16_t index = 0U; index < num_states_; ++index)
        {
            assert(states_[index].parent >= -1);
            const int8_t child = states_[index].initial_child;
            assert(child >= -1);
            if (child >= 0)
            {
                assert(static_cast<uint16_t>(child) < num_states_);
                assert(states_[child].parent == static_cast<int8_t>(index));
            }
            int8_t state = static_cast<int8_t>(index);
            uint16_t hops = 0U;
            while (state >= 0)
            {
                assert(static_cast<uint16_t>(state) < num_states_);
                assert(hops < num_states_);
                state = states_[state].parent;
                ++hops;
            }
        }
    }

    int8_t parent_of(int8_t state) const noexcept { return states_[state].parent; }

    uint16_t chain_depth(int8_t state) const noexcept
    {
        uint16_t depth = 0U;
        while (state >= 0)
        {
            ++depth;
            state = parent_of(state);
        }
        return depth;
    }

    int8_t ancestor_at_depth(int8_t state, uint16_t depth) const noexcept
    {
        uint16_t current_depth = chain_depth(state);
        while (current_depth > depth)
        {
            state = parent_of(state);
            --current_depth;
        }
        return state;
    }

    int8_t find_lca(int8_t a, int8_t b) const noexcept
    {
        uint16_t da = chain_depth(a);
        uint16_t db = chain_depth(b);
        while (da > db) { a = parent_of(a); --da; }
        while (db > da) { b = parent_of(b); --db; }
        while (a != b)
        {
            if (a < 0 || b < 0) { return -1; }
            a = parent_of(a);
            b = parent_of(b);
        }
        return a;
    }

    const TransitionDef<Context>* find_transition(
        const Context& ctx, uint16_t signal, int8_t source) const noexcept
    {
        for (uint16_t i = 0U; i < num_transitions_; ++i)
        {
            const TransitionDef<Context>& tran = transitions_[i];
            if (tran.source == source && tran.signal == signal)
            {
                if (nullptr != tran.guard && !tran.guard(ctx, signal))
                {
                    continue;
                }
                return &tran;
            }
        }
        return nullptr;
    }

    void execute_transition(Context& ctx, uint16_t signal, int8_t source,
                            const TransitionDef<Context>& tran) noexcept
    {
        switch (tran.kind)
        {
        case TransitionKind::Internal:
            if (nullptr != tran.action) { tran.action(ctx, signal); }
            break;

        case TransitionKind::Self:
        {
            int8_t state = current_;
            for (;;)
            {
                if (nullptr != states_[state].exit) { states_[state].exit(ctx); }
                if (state == source) { break; }
                state = parent_of(state);
            }
            if (nullptr != tran.action) { tran.action(ctx, signal); }
            if (nullptr != states_[source].entry) { states_[source].entry(ctx); }
            current_ = enter_initial_descendants(ctx, source);
            break;
        }

        case TransitionKind::External:
        {
            const int8_t target = tran.target;
            assert(target >= 0);
            assert(static_cast<uint16_t>(target) < num_states_);
            int8_t lca = find_lca(source, target);
            if ((lca == source) || (lca == target)) { lca = parent_of(lca); }
            exit_to_lca(ctx, lca);
            if (nullptr != tran.action) { tran.action(ctx, signal); }
            enter_path(ctx, lca, target);
            current_ = enter_initial_descendants(ctx, target);
            break;
        }

        default:
            break;
        }
    }

    void exit_to_lca(Context& ctx, int8_t lca) noexcept
    {
        int8_t state = current_;
        while (state != lca)
        {
            if (nullptr != states_[state].exit) { states_[state].exit(ctx); }
            state = parent_of(state);
        }
    }

    void enter_path(Context& ctx, int8_t lca, int8_t target) noexcept
    {
        const uint16_t lca_depth = chain_depth(lca);
        const uint16_t target_depth = chain_depth(target);
        for (uint16_t level = lca_depth + 1U; level <= target_depth; ++level)
        {
            const int8_t state = ancestor_at_depth(target, level);
            if (nullptr != states_[state].entry) { states_[state].entry(ctx); }
        }
    }

    int8_t enter_initial_descendants(Context& ctx, int8_t state) noexcept
    {
        uint8_t hops = 0U;
        for (;;)
        {
            const int8_t child = states_[state].initial_child;
            if (child < 0) { break; }
            assert(static_cast<uint16_t>(child) < num_states_);
            assert(states_[child].parent == state);
            assert(hops < max_depth_);
            if (nullptr != states_[child].entry) { states_[child].entry(ctx); }
            state = child;
            ++hops;
        }
        return state;
    }

    const StateDef<Context>* states_;
    uint16_t num_states_;
    const TransitionDef<Context>* transitions_;
    uint16_t num_transitions_;
    int8_t initial_state_;
    uint8_t max_depth_;
    int8_t current_;
};

}  // namespace hsm
