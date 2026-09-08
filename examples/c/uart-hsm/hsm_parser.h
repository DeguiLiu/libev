/**
 * @file hsm_parser.h
 * @brief 方案二: 基于层次状态机的协议解析器
 *
 * 实现思路:
 * - 事件驱动，每字节到达作为事件分发到状态机
 * - 每个解析阶段由独立状态维护 (Idle, Header, Length, CmdClass, Cmd, Data, CRC, Tail)
 * - 层次结构，统一同步和复位处理
 * - 使用 hsm_dispatch() 进行层次事件处理
 *
 * 状态图:
 * [*] --> Idle
 * Idle --> Header: BYTE_EVENT (0xAA)
 * Header --> Length1: received 0xAA
 * Length1 --> Length2: first length byte
 * Length2 --> CmdClass: second length byte
 * CmdClass --> Cmd: cmd_class received
 * Cmd --> Data: cmd received (if has data)
 * Cmd --> CRC1: cmd received (if no data)
 * Data --> CRC1: data complete
 * CRC1 --> CRC2: first CRC byte
 * CRC2 --> Tail: second CRC byte
 * Tail --> Idle: frame complete/frame error
 *
 */

#ifndef HSM_PARSER_H
#define HSM_PARSER_H

#include "types.h"
#include "uart_protocol.h"
#include "state_machine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 最大帧数据长度 */
#define HSM_FRAME_DATA_MAX      256U

/**
 * @brief 解析器事件定义
 */
typedef enum {
    HSM_EVENT_BYTE = 1,     /**< 字节接收事件 */
    HSM_EVENT_RESET,        /**< 复位事件 */
    HSM_EVENT_TIMEOUT       /**< 超时事件 */
} hsm_parser_event_e;

/**
 * @brief 解析器统计信息
 */
typedef struct {
    uint32_t frames_received;   /**< 接收帧数 */
    uint32_t bytes_received;    /**< 接收字节数 */
    uint32_t sync_errors;       /**< 同步错误 */
    uint32_t crc_errors;        /**< CRC 错误 */
    uint32_t tail_errors;       /**< 帧尾错误 */
    uint32_t length_errors;     /**< 长度错误 */
    uint32_t timeout_errors;    /**< 超时错误 */
} hsm_parser_stats_t;

/**
 * @brief 帧接收回调函数类型
 */
typedef void (*hsm_frame_callback_t)(const uart_frame_t *frame, void *user_data);

/**
 * @brief HSM 解析器结构
 */
typedef struct {
    hsm_t sm;                           /**< 状态机实例 */
    const hsm_state_t *entry_path[8];   /**< 状态路径缓冲区 */

    /* 解析上下文 */
    uart_frame_t frame;                   /**< 当前帧 */
    uint8_t current_byte;               /**< 当前接收字节 */
    uint16_t expected_len;              /**< 期望数据长度 */
    uint16_t data_index;                /**< 数据索引 */
    uint8_t crc_buf[2];                 /**< CRC 缓冲区 */
    uint8_t payload_buf[HSM_FRAME_DATA_MAX + 4]; /**< payload 缓冲区用于 CRC 计算 */
    uint16_t payload_index;             /**< payload 索引 */

    /* 统计信息 */
    hsm_parser_stats_t stats;           /**< 统计信息 */

    /* 回调 */
    hsm_frame_callback_t callback;      /**< 帧接收回调 */
    void *user_data;                    /**< 用户数据 */
} hsm_parser_t;

/**
 * @brief 初始化 HSM 解析器
 * @param parser 解析器指针
 * @param callback 帧接收回调函数
 * @param user_data 用户数据
 * @return TRUE 成功, FALSE 失败
 */
bool_t hsm_parser_init(hsm_parser_t *parser, hsm_frame_callback_t callback, void *user_data);

/**
 * @brief 复位解析器
 * @param parser 解析器指针
 */
void hsm_parser_reset(hsm_parser_t *parser);

/**
 * @brief 输入单字节
 * @param parser 解析器指针
 * @param byte 输入字节
 * @return TRUE 已处理, FALSE 未处理
 */
bool_t hsm_parser_put_byte(hsm_parser_t *parser, uint8_t byte);

/**
 * @brief 批量输入数据
 * @param parser 解析器指针
 * @param data 数据指针
 * @param len 数据长度
 */
void hsm_parser_put_data(hsm_parser_t *parser, const uint8_t *data, uint32_t len);

/**
 * @brief 获取当前状态名称
 * @param parser 解析器指针
 * @return 状态名称
 */
const char *hsm_parser_get_state(const hsm_parser_t *parser);

/**
 * @brief 获取统计信息
 * @param parser 解析器指针
 * @return 统计信息指针
 */
const hsm_parser_stats_t *hsm_parser_get_stats(const hsm_parser_t *parser);

/**
 * @brief 打印统计信息 (调试用)
 * @param parser 解析器指针
 */
void hsm_parser_print_stats(const hsm_parser_t *parser);

#ifdef __cplusplus
}
#endif

#endif /* HSM_PARSER_H */
