// C++17 UART frame parser as an HSM (mirrors examples/c/uart-hsm/hsm_parser.c
// + state_machine.c in C++). Header-only; the C state_machine engine is
// replaced by hsm.hpp. States: Idle -> Length1/2 -> CmdClass -> Cmd ->
// Data/CRC1/CRC2 -> Tail, all flat under the root. Shared by uart_hsm.cpp.
// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "hsm.hpp"
#include "uart_protocol.hpp"

namespace uart {

struct Frame {
    uint8_t cmd_class = 0U;
    uint8_t cmd = 0U;
    uint16_t data_len = 0U;
    uint8_t data[kFrameDataMax] = {0};
    uint16_t crc = 0U;
};

struct Stats {
    uint32_t frames_received = 0U;
    uint32_t bytes_received = 0U;
    uint32_t sync_errors = 0U;
    uint32_t crc_errors = 0U;
    uint32_t tail_errors = 0U;
    uint32_t length_errors = 0U;
    uint32_t timeout_errors = 0U;
};

struct ParserContext {
    Frame frame;
    uint8_t current_byte = 0U;
    uint16_t expected_len = 0U;
    uint16_t data_index = 0U;
    uint8_t crc_buf[2] = {0U, 0U};
    uint8_t payload_buf[kFrameDataMax + 4U] = {0};
    uint16_t payload_index = 0U;
    Stats stats;
    void (*callback)(const Frame&, void*) = nullptr;
    void* user_data = nullptr;
};

enum Signal : uint16_t {
    kByte = 1U,
    kReset = 2U,
    kTimeout = 3U
};

constexpr int8_t kRoot = 0;
constexpr int8_t kIdle = 1;
constexpr int8_t kLength1 = 2;
constexpr int8_t kLength2 = 3;
constexpr int8_t kCmdClass = 4;
constexpr int8_t kCmd = 5;
constexpr int8_t kData = 6;
constexpr int8_t kCrc1 = 7;
constexpr int8_t kCrc2 = 8;
constexpr int8_t kStateTail = 9;

/* ---- guards ---- */
bool guard_is_header(const ParserContext&, uint16_t);
bool guard_is_not_header(const ParserContext&, uint16_t);
bool guard_has_data(const ParserContext&, uint16_t);
bool guard_no_data(const ParserContext&, uint16_t);
bool guard_data_complete(const ParserContext&, uint16_t);
bool guard_data_incomplete(const ParserContext&, uint16_t);
bool guard_is_tail(const ParserContext&, uint16_t);
bool guard_length_valid(const ParserContext&, uint16_t);
bool guard_length_invalid(const ParserContext&, uint16_t);

/* ---- actions ---- */
void act_reset_context(ParserContext&, uint16_t);
void act_sync_error(ParserContext&, uint16_t);
void act_store_length1(ParserContext&, uint16_t);
void act_store_length2(ParserContext&, uint16_t);
void act_length_error(ParserContext&, uint16_t);
void act_store_cmd_class(ParserContext&, uint16_t);
void act_store_cmd(ParserContext&, uint16_t);
void act_store_data(ParserContext&, uint16_t);
void act_store_crc1(ParserContext&, uint16_t);
void act_store_crc2(ParserContext&, uint16_t);
void act_complete_frame(ParserContext&, uint16_t);
void act_tail_error(ParserContext&, uint16_t);
void act_timeout_error(ParserContext&, uint16_t);

const hsm::StateDef<ParserContext> kParserStates[] = {
    { -1, nullptr, nullptr, "Root" },
    { kRoot, nullptr, nullptr, "Idle" },
    { kRoot, nullptr, nullptr, "Length1" },
    { kRoot, nullptr, nullptr, "Length2" },
    { kRoot, nullptr, nullptr, "CmdClass" },
    { kRoot, nullptr, nullptr, "Cmd" },
    { kRoot, nullptr, nullptr, "Data" },
    { kRoot, nullptr, nullptr, "CRC1" },
    { kRoot, nullptr, nullptr, "CRC2" },
    { kRoot, nullptr, nullptr, "Tail" },
};

const hsm::TransitionDef<ParserContext> kParserTransitions[] = {
    /* Idle: header byte starts a frame, anything else is a sync error */
    { kIdle, kByte, kLength1, hsm::TransitionKind::External, guard_is_header, act_reset_context },
    { kIdle, kByte, kIdle, hsm::TransitionKind::External, guard_is_not_header, act_sync_error },
    { kIdle, kReset, kIdle, hsm::TransitionKind::External, nullptr, act_reset_context },

    /* Length1 */
    { kLength1, kByte, kLength2, hsm::TransitionKind::External, nullptr, act_store_length1 },
    { kLength1, kTimeout, kIdle, hsm::TransitionKind::External, nullptr, act_timeout_error },

    /* Length2 */
    { kLength2, kByte, kCmdClass, hsm::TransitionKind::External, guard_length_valid, act_store_length2 },
    { kLength2, kByte, kIdle, hsm::TransitionKind::External, guard_length_invalid, act_length_error },
    { kLength2, kTimeout, kIdle, hsm::TransitionKind::External, nullptr, act_timeout_error },

    /* CmdClass */
    { kCmdClass, kByte, kCmd, hsm::TransitionKind::External, nullptr, act_store_cmd_class },
    { kCmdClass, kTimeout, kIdle, hsm::TransitionKind::External, nullptr, act_timeout_error },

    /* Cmd */
    { kCmd, kByte, kData, hsm::TransitionKind::External, guard_has_data, act_store_cmd },
    { kCmd, kByte, kCrc1, hsm::TransitionKind::External, guard_no_data, act_store_cmd },
    { kCmd, kTimeout, kIdle, hsm::TransitionKind::External, nullptr, act_timeout_error },

    /* Data */
    { kData, kByte, kData, hsm::TransitionKind::External, guard_data_incomplete, act_store_data },
    { kData, kByte, kCrc1, hsm::TransitionKind::External, guard_data_complete, act_store_data },
    { kData, kTimeout, kIdle, hsm::TransitionKind::External, nullptr, act_timeout_error },

    /* CRC1 / CRC2 */
    { kCrc1, kByte, kCrc2, hsm::TransitionKind::External, nullptr, act_store_crc1 },
    { kCrc1, kTimeout, kIdle, hsm::TransitionKind::External, nullptr, act_timeout_error },
    { kCrc2, kByte, kStateTail, hsm::TransitionKind::External, nullptr, act_store_crc2 },
    { kCrc2, kTimeout, kIdle, hsm::TransitionKind::External, nullptr, act_timeout_error },

    /* Tail */
    { kStateTail, kByte, kIdle, hsm::TransitionKind::External, guard_is_tail, act_complete_frame },
    { kStateTail, kByte, kIdle, hsm::TransitionKind::External, nullptr, act_tail_error },
};

constexpr uint16_t kParserNumStates = static_cast<uint16_t>(sizeof(kParserStates) / sizeof(kParserStates[0]));
constexpr uint16_t kParserNumTransitions = static_cast<uint16_t>(sizeof(kParserTransitions) / sizeof(kParserTransitions[0]));

/* ---- guard implementations ---- */

bool guard_is_header(const ParserContext& ctx, uint16_t)
{
    return ctx.current_byte == kHeader;
}

bool guard_is_not_header(const ParserContext& ctx, uint16_t)
{
    return ctx.current_byte != kHeader;
}

bool guard_has_data(const ParserContext& ctx, uint16_t)
{
    return ctx.expected_len > 2U;
}

bool guard_no_data(const ParserContext& ctx, uint16_t)
{
    return ctx.expected_len == 2U;
}

bool guard_data_complete(const ParserContext& ctx, uint16_t)
{
    return (ctx.data_index + 1U) >= ctx.frame.data_len;
}

bool guard_data_incomplete(const ParserContext& ctx, uint16_t)
{
    return (ctx.data_index + 1U) < ctx.frame.data_len;
}

bool guard_is_tail(const ParserContext& ctx, uint16_t)
{
    return ctx.current_byte == kTail;
}

bool guard_length_valid(const ParserContext& ctx, uint16_t)
{
    return (ctx.expected_len >= 2U) && ((ctx.expected_len - 2U) <= kFrameDataMax);
}

bool guard_length_invalid(const ParserContext& ctx, uint16_t signal)
{
    return !guard_length_valid(ctx, signal);
}

/* ---- action implementations ---- */

void act_reset_context(ParserContext& ctx, uint16_t)
{
    ctx.frame = Frame{};
    ctx.expected_len = 0U;
    ctx.data_index = 0U;
    ctx.payload_index = 0U;
    ctx.crc_buf[0] = 0U;
    ctx.crc_buf[1] = 0U;
}

void act_sync_error(ParserContext& ctx, uint16_t)
{
    ++ctx.stats.sync_errors;
}

void act_store_length1(ParserContext& ctx, uint16_t)
{
    ctx.expected_len = ctx.current_byte;
}

void act_store_length2(ParserContext& ctx, uint16_t)
{
    ctx.expected_len |= static_cast<uint16_t>(static_cast<uint16_t>(ctx.current_byte) << 8);
}

void act_length_error(ParserContext& ctx, uint16_t)
{
    ++ctx.stats.length_errors;
}

void act_store_cmd_class(ParserContext& ctx, uint16_t)
{
    ctx.frame.cmd_class = ctx.current_byte;
    ctx.payload_buf[ctx.payload_index++] = ctx.current_byte;
}

void act_store_cmd(ParserContext& ctx, uint16_t)
{
    ctx.frame.cmd = ctx.current_byte;
    ctx.payload_buf[ctx.payload_index++] = ctx.current_byte;
    ctx.frame.data_len = static_cast<uint16_t>(ctx.expected_len - 2U);
    ctx.data_index = 0U;
}

void act_store_data(ParserContext& ctx, uint16_t)
{
    if (ctx.data_index < ctx.frame.data_len)
    {
        ctx.frame.data[ctx.data_index] = ctx.current_byte;
        ctx.payload_buf[ctx.payload_index++] = ctx.current_byte;
        ++ctx.data_index;
    }
}

void act_store_crc1(ParserContext& ctx, uint16_t)
{
    ctx.crc_buf[0] = ctx.current_byte;
}

void act_store_crc2(ParserContext& ctx, uint16_t)
{
    ctx.crc_buf[1] = ctx.current_byte;
}

void act_complete_frame(ParserContext& ctx, uint16_t)
{
    const uint16_t crc_calc = crc16(ctx.payload_buf, ctx.payload_index);
    const uint16_t crc_recv = static_cast<uint16_t>(ctx.crc_buf[0] | static_cast<uint16_t>(ctx.crc_buf[1] << 8));

    if (crc_calc != crc_recv)
    {
        ++ctx.stats.crc_errors;
        std::printf("[HSM_PARSER] CRC error: calc=0x%04X, recv=0x%04X\n",
                    static_cast<unsigned>(crc_calc), static_cast<unsigned>(crc_recv));
        return;
    }

    ctx.frame.crc = crc_recv;
    ++ctx.stats.frames_received;

    if (nullptr != ctx.callback)
    {
        ctx.callback(ctx.frame, ctx.user_data);
    }
}

void act_tail_error(ParserContext& ctx, uint16_t)
{
    ++ctx.stats.tail_errors;
    std::printf("[HSM_PARSER] Tail error: expected=0x55, got=0x%02X\n",
                static_cast<unsigned>(ctx.current_byte));
}

void act_timeout_error(ParserContext& ctx, uint16_t)
{
    ++ctx.stats.timeout_errors;
}

/* ---- parser facade ---- */

class HsmParser {
public:
    HsmParser() noexcept
        : hsm_(kParserStates, kParserNumStates, kParserTransitions, kParserNumTransitions,
               kIdle, /*max_depth=*/2U)
    {
    }

    void init(void (*callback)(const Frame&, void*), void* user_data) noexcept
    {
        ctx_.callback = callback;
        ctx_.user_data = user_data;
        ctx_.stats = Stats{};
        act_reset_context(ctx_, 0U);
        hsm_.init(ctx_);
    }

    void put_byte(uint8_t byte) noexcept
    {
        ctx_.current_byte = byte;
        ++ctx_.stats.bytes_received;
        hsm_.dispatch(ctx_, kByte);
    }

    void put_data(const uint8_t* data, uint32_t len) noexcept
    {
        if ((nullptr != data) && (len > 0U))
        {
            for (uint32_t i = 0U; i < len; ++i)
            {
                put_byte(data[i]);
            }
        }
    }

    const Stats& stats() const noexcept { return ctx_.stats; }
    const char* state_name() const noexcept { return hsm_.current_state_name(); }

private:
    ParserContext ctx_;
    hsm::Hsm<ParserContext> hsm_;
};

}  // namespace uart
