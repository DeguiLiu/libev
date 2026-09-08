/**
 * @file hsm_parser.c
 * @brief 方案二: 基于层次状态机的协议解析器实现
 *
 * 协议格式: 0xAA (帧头) | LEN(2B) | CMD_CLASS | CMD | DATA[LEN-2] | CRC16(2B) | 0x55 (帧尾)
 *
 */

#include "hsm_parser.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>

/* ===== 事件 ID 定义 ===== */

#define EVT_BYTE        1U      /**< 字节接收事件 */
#define EVT_RESET       2U      /**< 复位事件 */
#define EVT_TIMEOUT     3U      /**< 超时事件 */

/* ===== 前向声明 ===== */

static const hsm_state_t state_idle;
static const hsm_state_t state_length1;
static const hsm_state_t state_length2;
static const hsm_state_t state_cmd_class;
static const hsm_state_t state_cmd;
static const hsm_state_t state_data;
static const hsm_state_t state_crc1;
static const hsm_state_t state_crc2;
static const hsm_state_t state_tail;

/* ===== 辅助宏 ===== */

#define GET_PARSER(sm)  ((hsm_parser_t *)((char *)(sm) - offsetof(hsm_parser_t, sm)))

/* ===== 私有辅助函数 ===== */

/**
 * @brief 复位解析上下文
 */
static void reset_parse_context(hsm_parser_t *parser)
{
    memset(&parser->frame, 0, sizeof(uart_frame_t));
    parser->expected_len = 0U;
    parser->data_index = 0U;
    parser->payload_index = 0U;
    parser->crc_buf[0] = 0U;
    parser->crc_buf[1] = 0U;
}

/**
 * @brief 完成帧解析
 */
static void complete_frame(hsm_parser_t *parser)
{
    uint16_t crc_calc;
    uint16_t crc_recv;

    /* 计算 CRC */
    crc_calc = uart_calc_crc16(parser->payload_buf, parser->payload_index);
    crc_recv = (uint16_t)parser->crc_buf[0] | ((uint16_t)parser->crc_buf[1] << 8);

    if (crc_calc != crc_recv)
    {
        parser->stats.crc_errors++;
        printf("[HSM_PARSER] CRC error: calc=0x%04X, recv=0x%04X\n", crc_calc, crc_recv);
        return;
    }

    parser->frame.crc = crc_recv;
    parser->stats.frames_received++;

    /* 调用回调 */
    if (parser->callback != NULL)
    {
        parser->callback(&parser->frame, parser->user_data);
    }
}

/* ===== Guard 函数 ===== */

static bool_t guard_is_header(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    return (parser->current_byte == SIMPLE_FRAME_HEADER) ? TRUE : FALSE;
}

static bool_t guard_is_not_header(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    return (parser->current_byte != SIMPLE_FRAME_HEADER) ? TRUE : FALSE;
}

static bool_t guard_has_data(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    /* expected_len 包含 cmd_class + cmd + data，所以 data_len = expected_len - 2 */
    return (parser->expected_len > 2U) ? TRUE : FALSE;
}

static bool_t guard_no_data(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    /* 只有 cmd_class + cmd，没有数据 */
    return (parser->expected_len == 2U) ? TRUE : FALSE;
}

static bool_t guard_data_complete(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    /* 当前字节存储后会达到完成 */
    return ((parser->data_index + 1U) >= parser->frame.data_len) ? TRUE : FALSE;
}

static bool_t guard_data_incomplete(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    /* 当前字节存储后还未完成 */
    return ((parser->data_index + 1U) < parser->frame.data_len) ? TRUE : FALSE;
}

static bool_t guard_is_tail(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    return (parser->current_byte == SIMPLE_FRAME_TAIL) ? TRUE : FALSE;
}

static bool_t guard_length_valid(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    return ((parser->expected_len >= 2U) &&
            ((parser->expected_len - 2U) <= HSM_FRAME_DATA_MAX)) ? TRUE : FALSE;
}

static bool_t guard_length_invalid(hsm_t *sm, const hsm_event_t *event)
{
    bool_t is_valid = guard_length_valid(sm, event);
    return (FALSE == is_valid) ? TRUE : FALSE;
}

/* ===== Action 函数 ===== */

static void action_reset_context(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    reset_parse_context(parser);
}

static void action_sync_error(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    parser->stats.sync_errors++;
}

static void action_store_length1(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    parser->expected_len = parser->current_byte;
}

static void action_store_length2(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    parser->expected_len |= ((uint16_t)parser->current_byte << 8);
}

static void action_length_error(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    parser->stats.length_errors++;
}

static void action_store_cmd_class(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    parser->frame.cmd_class = parser->current_byte;
    parser->payload_buf[parser->payload_index++] = parser->current_byte;
}

static void action_store_cmd(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    parser->frame.cmd = parser->current_byte;
    parser->payload_buf[parser->payload_index++] = parser->current_byte;
    parser->frame.data_len = parser->expected_len - 2U;
    parser->data_index = 0U;
}

static void action_store_data(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    if (parser->data_index < parser->frame.data_len)
    {
        parser->frame.data[parser->data_index] = parser->current_byte;
        parser->payload_buf[parser->payload_index++] = parser->current_byte;
        parser->data_index++;
    }
}

static void action_store_crc1(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    parser->crc_buf[0] = parser->current_byte;
}

static void action_store_crc2(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    parser->crc_buf[1] = parser->current_byte;
}

static void action_complete_frame(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    complete_frame(parser);
}

static void action_tail_error(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    parser->stats.tail_errors++;
    printf("[HSM_PARSER] Tail error: expected=0x55, got=0x%02X\n", parser->current_byte);
}

static void action_timeout_error(hsm_t *sm, const hsm_event_t *event)
{
    hsm_parser_t *parser = GET_PARSER(sm);
    (void)event;
    parser->stats.timeout_errors++;
}

/* ===== 状态转换表 ===== */

static const hsm_transition_t trans_idle[] = {
    { EVT_BYTE,    &state_length1, guard_is_header,     action_reset_context, SM_TRANSITION_EXTERNAL },
    { EVT_BYTE,    &state_idle,    guard_is_not_header, action_sync_error,    SM_TRANSITION_EXTERNAL },
    { EVT_RESET,   &state_idle,    NULL,                action_reset_context, SM_TRANSITION_EXTERNAL },
};

static const hsm_transition_t trans_length1[] = {
    { EVT_BYTE,    &state_length2, NULL, action_store_length1, SM_TRANSITION_EXTERNAL },
    { EVT_TIMEOUT, &state_idle,    NULL, action_timeout_error, SM_TRANSITION_EXTERNAL },
};

static const hsm_transition_t trans_length2[] = {
    { EVT_BYTE,    &state_cmd_class, guard_length_valid,   action_store_length2, SM_TRANSITION_EXTERNAL },
    { EVT_BYTE,    &state_idle,      guard_length_invalid, action_length_error,  SM_TRANSITION_EXTERNAL },
    { EVT_TIMEOUT, &state_idle,      NULL,                 action_timeout_error, SM_TRANSITION_EXTERNAL },
};

static const hsm_transition_t trans_cmd_class[] = {
    { EVT_BYTE,    &state_cmd,  NULL, action_store_cmd_class, SM_TRANSITION_EXTERNAL },
    { EVT_TIMEOUT, &state_idle, NULL, action_timeout_error,   SM_TRANSITION_EXTERNAL },
};

static const hsm_transition_t trans_cmd[] = {
    { EVT_BYTE,    &state_data, guard_has_data, action_store_cmd, SM_TRANSITION_EXTERNAL },
    { EVT_BYTE,    &state_crc1, guard_no_data,  action_store_cmd, SM_TRANSITION_EXTERNAL },
    { EVT_TIMEOUT, &state_idle, NULL,           action_timeout_error, SM_TRANSITION_EXTERNAL },
};

static const hsm_transition_t trans_data[] = {
    { EVT_BYTE,    &state_data, guard_data_incomplete, action_store_data,    SM_TRANSITION_EXTERNAL },
    { EVT_BYTE,    &state_crc1, guard_data_complete,   action_store_data,    SM_TRANSITION_EXTERNAL },
    { EVT_TIMEOUT, &state_idle, NULL,                  action_timeout_error, SM_TRANSITION_EXTERNAL },
};

static const hsm_transition_t trans_crc1[] = {
    { EVT_BYTE,    &state_crc2, NULL, action_store_crc1,    SM_TRANSITION_EXTERNAL },
    { EVT_TIMEOUT, &state_idle, NULL, action_timeout_error, SM_TRANSITION_EXTERNAL },
};

static const hsm_transition_t trans_crc2[] = {
    { EVT_BYTE,    &state_tail, NULL, action_store_crc2,    SM_TRANSITION_EXTERNAL },
    { EVT_TIMEOUT, &state_idle, NULL, action_timeout_error, SM_TRANSITION_EXTERNAL },
};

static const hsm_transition_t trans_tail[] = {
    { EVT_BYTE, &state_idle, guard_is_tail, action_complete_frame, SM_TRANSITION_EXTERNAL },
    { EVT_BYTE, &state_idle, NULL,          action_tail_error,     SM_TRANSITION_EXTERNAL },
};

/* ===== 状态定义 ===== */

static const hsm_state_t state_idle = {
    .parent = NULL,
    .entry_action = NULL,
    .exit_action = NULL,
    .transitions = trans_idle,
    .num_transitions = sizeof(trans_idle) / sizeof(trans_idle[0]),
    .name = "Idle"
};

static const hsm_state_t state_length1 = {
    .parent = NULL,
    .entry_action = NULL,
    .exit_action = NULL,
    .transitions = trans_length1,
    .num_transitions = sizeof(trans_length1) / sizeof(trans_length1[0]),
    .name = "Length1"
};

static const hsm_state_t state_length2 = {
    .parent = NULL,
    .entry_action = NULL,
    .exit_action = NULL,
    .transitions = trans_length2,
    .num_transitions = sizeof(trans_length2) / sizeof(trans_length2[0]),
    .name = "Length2"
};

static const hsm_state_t state_cmd_class = {
    .parent = NULL,
    .entry_action = NULL,
    .exit_action = NULL,
    .transitions = trans_cmd_class,
    .num_transitions = sizeof(trans_cmd_class) / sizeof(trans_cmd_class[0]),
    .name = "CmdClass"
};

static const hsm_state_t state_cmd = {
    .parent = NULL,
    .entry_action = NULL,
    .exit_action = NULL,
    .transitions = trans_cmd,
    .num_transitions = sizeof(trans_cmd) / sizeof(trans_cmd[0]),
    .name = "Cmd"
};

static const hsm_state_t state_data = {
    .parent = NULL,
    .entry_action = NULL,
    .exit_action = NULL,
    .transitions = trans_data,
    .num_transitions = sizeof(trans_data) / sizeof(trans_data[0]),
    .name = "Data"
};

static const hsm_state_t state_crc1 = {
    .parent = NULL,
    .entry_action = NULL,
    .exit_action = NULL,
    .transitions = trans_crc1,
    .num_transitions = sizeof(trans_crc1) / sizeof(trans_crc1[0]),
    .name = "CRC1"
};

static const hsm_state_t state_crc2 = {
    .parent = NULL,
    .entry_action = NULL,
    .exit_action = NULL,
    .transitions = trans_crc2,
    .num_transitions = sizeof(trans_crc2) / sizeof(trans_crc2[0]),
    .name = "CRC2"
};

static const hsm_state_t state_tail = {
    .parent = NULL,
    .entry_action = NULL,
    .exit_action = NULL,
    .transitions = trans_tail,
    .num_transitions = sizeof(trans_tail) / sizeof(trans_tail[0]),
    .name = "Tail"
};

/* ===== 公共 API 实现 ===== */

bool_t hsm_parser_init(hsm_parser_t *parser, hsm_frame_callback_t callback, void *user_data)
{
    if (parser == NULL)
    {
        return FALSE;
    }

    /* 初始化状态机 */
    hsm_init(&parser->sm,
             &state_idle,
             (const hsm_state_t **)parser->entry_path,
             (uint8_t)ARRAY_SIZE(parser->entry_path),
             parser,
             NULL);

    /* 清除解析上下文 */
    reset_parse_context(parser);

    /* 清除统计信息 */
    memset(&parser->stats, 0, sizeof(hsm_parser_stats_t));

    /* 设置回调 */
    parser->callback = callback;
    parser->user_data = user_data;

    printf("[HSM_PARSER] Initialized, initial state: %s\n",
           hsm_get_current_state_name(&parser->sm));

    return TRUE;
}

void hsm_parser_reset(hsm_parser_t *parser)
{
    hsm_event_t evt;

    if (parser == NULL)
    {
        return;
    }

    reset_parse_context(parser);
    evt.id = EVT_RESET;
    evt.context = NULL;
    hsm_dispatch(&parser->sm, &evt);
}

bool_t hsm_parser_put_byte(hsm_parser_t *parser, uint8_t byte)
{
    hsm_event_t evt;
    bool_t result;

    if (parser == NULL)
    {
        return FALSE;
    }

    parser->current_byte = byte;
    parser->stats.bytes_received++;

    evt.id = EVT_BYTE;
    evt.context = NULL;

    result = hsm_dispatch(&parser->sm, &evt);

    return result;
}

void hsm_parser_put_data(hsm_parser_t *parser, const uint8_t *data, uint32_t len)
{
    uint32_t i;

    if ((parser == NULL) || (data == NULL) || (len == 0U))
    {
        return;
    }

    for (i = 0U; i < len; i++)
    {
        hsm_parser_put_byte(parser, data[i]);
    }
}

const char *hsm_parser_get_state(const hsm_parser_t *parser)
{
    if (parser == NULL)
    {
        return "NULL";
    }
    return hsm_get_current_state_name(&parser->sm);
}

const hsm_parser_stats_t *hsm_parser_get_stats(const hsm_parser_t *parser)
{
    if (parser == NULL)
    {
        return NULL;
    }
    return &parser->stats;
}

void hsm_parser_print_stats(const hsm_parser_t *parser)
{
    if (parser == NULL)
    {
        return;
    }

    printf("\n=== HSM Parser Stats ===\n");
    printf("Current state:    %s\n", hsm_get_current_state_name(&parser->sm));
    printf("Bytes received:   %u\n", parser->stats.bytes_received);
    printf("Frames received:  %u\n", parser->stats.frames_received);
    printf("Sync errors:      %u\n", parser->stats.sync_errors);
    printf("CRC errors:       %u\n", parser->stats.crc_errors);
    printf("Tail errors:      %u\n", parser->stats.tail_errors);
    printf("Length errors:    %u\n", parser->stats.length_errors);
    printf("Timeout errors:   %u\n", parser->stats.timeout_errors);
    printf("========================\n");
}
