/**
 * @file uart_protocol.h
 * @brief UART 通信协议定义
 *
 * 定义标准 UART 通信协议格式
 *
 * 协议帧格式:
 * - 简单帧: 0xAA | LEN(2B) | CMD_CLASS | CMD | DATA[LEN-2] | CRC16(2B) | 0x55
 * - 标准帧: 'UCI' | LEN(2B) | CMD_CLASS | CMD | SUB_CMD | RSVD | PARAM(4B) | ADDR(4B) |
 *          DATA_LEN(2B) | DATA_CRC(2B) | HDR_CRC(2B) | DATA[DATA_LEN]
 */

#ifndef UART_PROTOCOL_H
#define UART_PROTOCOL_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 协议常量定义 ===== */

/* 简单帧格式常量 */
#define SIMPLE_FRAME_HEADER     0xAAU   /**< 简单帧头 */
#define SIMPLE_FRAME_TAIL       0x55U   /**< 简单帧尾 */
#define SIMPLE_FRAME_MIN_LEN    6U      /**< 最小帧长度: 头(1)+长度(2)+命令(2)+尾(1) */
#define SIMPLE_FRAME_OVERHEAD   5U      /**< 帧开销: 头(1)+长度(2)+尾(1)+CRC不含 */

/* UCI帧格式常量 */
#define UCI_MAGIC_0             'U'
#define UCI_MAGIC_1             'C'
#define UCI_MAGIC_2             'I'
#define UCI_MAX_LENGTH          4096U   /**< UCI最大包长度 */
#define UCI_HEADER_LEN          18U     /**< UCI标准头长度 */

/* Vendor命令类定义 */
#define VDCMD_CLASS_SYS         0x01U   /**< 系统命令 */
#define VDCMD_CLASS_SPI         0x02U   /**< SPI Flash命令 */
#define VDCMD_CLASS_DIAG        0x03U   /**< 诊断命令 */
#define VDCMD_CLASS_OTA         0x04U   /**< OTA升级命令 */
#define VDCMD_CLASS_CONFIG      0x10U   /**< 配置命令 */

/* 命令方向和数据有效标志 */
#define VDCMD_DIR_IN            0x00U   /**< 设备到主机 */
#define VDCMD_DIR_OUT           0x40U   /**< 主机到设备 */
#define VDCMD_DATA_VLD          0x80U   /**< 有数据传输 */
#define VDCMD_DATA_INVLD        0x00U   /**< 无数据传输 */

/* 系统命令定义 */
#define VDCMD_SYS_RST_NORMAL    (0x01U | VDCMD_DATA_INVLD | VDCMD_DIR_OUT)   /**< 复位到正常模式 */
#define VDCMD_SYS_RST_BOOT      (0x07U | VDCMD_DATA_INVLD | VDCMD_DIR_OUT)   /**< 复位到Bootloader */
#define VDCMD_SYS_GET_INFO      (0x01U | VDCMD_DATA_VLD | VDCMD_DIR_IN)      /**< 获取设备信息 */
#define VDCMD_SYS_SET_BAUD      (0x44U | VDCMD_DATA_INVLD | VDCMD_DIR_OUT)   /**< 设置波特率 */

/* SPI Flash命令定义 */
#define VDCMD_SPI_READ_ID       (0x01U | VDCMD_DATA_VLD | VDCMD_DIR_IN)      /**< 读取Flash ID */
#define VDCMD_SPI_READ          (0x02U | VDCMD_DATA_VLD | VDCMD_DIR_IN)      /**< 读取Flash */
#define VDCMD_SPI_WRITE         (0x01U | VDCMD_DATA_VLD | VDCMD_DIR_OUT)     /**< 写入Flash */
#define VDCMD_SPI_ERASE         (0x04U | VDCMD_DATA_INVLD | VDCMD_DIR_OUT)   /**< 擦除Flash */
#define VDCMD_SPI_GET_CRC       (0x04U | VDCMD_DATA_VLD | VDCMD_DIR_IN)      /**< 获取CRC */

/* OTA升级命令定义 */
#define VDCMD_OTA_START         0x01U   /**< 开始升级 */
#define VDCMD_OTA_DATA          0x02U   /**< 升级数据 */
#define VDCMD_OTA_END           0x03U   /**< 结束升级 */
#define VDCMD_OTA_VERIFY        0x04U   /**< 校验升级 */

/* 设备信息类型 */
typedef enum {
    DEV_INFO_NAME = 1U,         /**< 设备名称 */
    DEV_INFO_FW_VER,            /**< 固件版本 */
    DEV_INFO_CUSTOMER_ID,       /**< 客户ID */
    DEV_INFO_VENDOR_ID,         /**< 厂商ID */
    DEV_INFO_PRODUCT_ID,        /**< 产品ID */
    DEV_INFO_SN,                /**< 序列号 */
    DEV_INFO_ISP_VER,           /**< ISP版本 */
    DEV_INFO_BOOT_VER,          /**< Bootloader版本 */
    DEV_INFO_MAX
} dev_info_type_e;

/* 命令状态码 */
typedef enum {
    VDCMD_STS_SUCCESS = 0x00U,  /**< 成功 */
    VDCMD_STS_UNKNOWN = 0x01U,  /**< 未知命令 */
    VDCMD_STS_PARAM_ERR = 0x02U,/**< 参数错误 */
    VDCMD_STS_CRC_ERR = 0x03U,  /**< CRC错误 */
    VDCMD_STS_HW_ERR = 0x04U,   /**< 硬件错误 */
    VDCMD_STS_BUSY = 0x05U,     /**< 设备忙 */
    VDCMD_STS_TIMEOUT = 0x06U   /**< 超时 */
} vdcmd_status_e;

/* ===== 帧结构定义 ===== */

/**
 * @brief 简单协议帧结构
 * 格式: 0xAA | LEN(2B) | CMD_CLASS | CMD | DATA[LEN-2] | CRC16(2B) | 0x55
 */
#pragma pack(push, 1)
typedef struct {
    uint8_t header;             /**< 帧头 0xAA */
    uint16_t length;            /**< 数据长度(不含帧头帧尾和CRC) */
    uint8_t cmd_class;          /**< 命令类 */
    uint8_t cmd;                /**< 命令 */
    /* 后续为: data[length-2] + crc16(2) + tail(1) */
} simple_frame_header_t;

/**
 * @brief UCI标准帧头结构
 */
typedef struct {
    uint8_t cmd_class;          /**< 命令类 */
    uint8_t cmd;                /**< 命令 */
    uint8_t sub_cmd;            /**< 子命令 */
    uint8_t reserved;           /**< 保留 */
    uint32_t param;             /**< 参数 */
    uint32_t addr;              /**< 地址 */
    uint16_t data_len;          /**< 数据长度 */
    uint16_t data_crc;          /**< 数据CRC */
    uint16_t hdr_crc;           /**< 帧头CRC */
} uci_std_header_t;
#pragma pack(pop)

/**
 * @brief 协议帧完整结构 (用于解析后的数据)
 */
typedef struct {
    uint8_t cmd_class;          /**< 命令类 */
    uint8_t cmd;                /**< 命令 */
    uint8_t sub_cmd;            /**< 子命令 */
    uint32_t param;             /**< 参数 */
    uint32_t addr;              /**< 地址 */
    uint16_t data_len;          /**< 数据长度 */
    uint8_t data[256];          /**< 数据缓冲区 */
    uint16_t crc;               /**< CRC值 */
} uart_frame_t;

/**
 * @brief 协议统计信息
 */
typedef struct {
    uint32_t rx_bytes;          /**< 接收字节数 */
    uint32_t rx_frames;         /**< 接收帧数 */
    uint32_t tx_bytes;          /**< 发送字节数 */
    uint32_t tx_frames;         /**< 发送帧数 */
    uint32_t sync_errors;       /**< 同步错误 */
    uint32_t crc_errors;        /**< CRC错误 */
    uint32_t timeout_errors;    /**< 超时错误 */
    uint32_t overflow_errors;   /**< 溢出错误 */
} uart_protocol_stats_t;

/**
 * @brief 帧接收回调函数类型
 */
typedef void (*uart_frame_callback_t)(const uart_frame_t *frame, void *user_data);

/* ===== CRC计算函数 ===== */

/**
 * @brief CRC16-CCITT 查表法
 */
static const uint16_t crc16_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50A5, 0x60C6, 0x70E7,
    0x8108, 0x9129, 0xA14A, 0xB16B, 0xC18C, 0xD1AD, 0xE1CE, 0xF1EF,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52B5, 0x4294, 0x72F7, 0x62D6,
    0x9339, 0x8318, 0xB37B, 0xA35A, 0xD3BD, 0xC39C, 0xF3FF, 0xE3DE,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64E6, 0x74C7, 0x44A4, 0x5485,
    0xA56A, 0xB54B, 0x8528, 0x9509, 0xE5EE, 0xF5CF, 0xC5AC, 0xD58D,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76D7, 0x66F6, 0x5695, 0x46B4,
    0xB75B, 0xA77A, 0x9719, 0x8738, 0xF7DF, 0xE7FE, 0xD79D, 0xC7BC,
    0x48C4, 0x58E5, 0x6886, 0x78A7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xC9CC, 0xD9ED, 0xE98E, 0xF9AF, 0x8948, 0x9969, 0xA90A, 0xB92B,
    0x5AF5, 0x4AD4, 0x7AB7, 0x6A96, 0x1A71, 0x0A50, 0x3A33, 0x2A12,
    0xDBFD, 0xCBDC, 0xFBBF, 0xEB9E, 0x9B79, 0x8B58, 0xBB3B, 0xAB1A,
    0x6CA6, 0x7C87, 0x4CE4, 0x5CC5, 0x2C22, 0x3C03, 0x0C60, 0x1C41,
    0xEDAE, 0xFD8F, 0xCDEC, 0xDDCD, 0xAD2A, 0xBD0B, 0x8D68, 0x9D49,
    0x7E97, 0x6EB6, 0x5ED5, 0x4EF4, 0x3E13, 0x2E32, 0x1E51, 0x0E70,
    0xFF9F, 0xEFBE, 0xDFDD, 0xCFFC, 0xBF1B, 0xAF3A, 0x9F59, 0x8F78,
    0x9188, 0x81A9, 0xB1CA, 0xA1EB, 0xD10C, 0xC12D, 0xF14E, 0xE16F,
    0x1080, 0x00A1, 0x30C2, 0x20E3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83B9, 0x9398, 0xA3FB, 0xB3DA, 0xC33D, 0xD31C, 0xE37F, 0xF35E,
    0x02B1, 0x1290, 0x22F3, 0x32D2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xB5EA, 0xA5CB, 0x95A8, 0x8589, 0xF56E, 0xE54F, 0xD52C, 0xC50D,
    0x34E2, 0x24C3, 0x14A0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xA7DB, 0xB7FA, 0x8799, 0x97B8, 0xE75F, 0xF77E, 0xC71D, 0xD73C,
    0x26D3, 0x36F2, 0x0691, 0x16B0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xD94C, 0xC96D, 0xF90E, 0xE92F, 0x99C8, 0x89E9, 0xB98A, 0xA9AB,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18C0, 0x08E1, 0x3882, 0x28A3,
    0xCB7D, 0xDB5C, 0xEB3F, 0xFB1E, 0x8BF9, 0x9BD8, 0xABBB, 0xBB9A,
    0x4A75, 0x5A54, 0x6A37, 0x7A16, 0x0AF1, 0x1AD0, 0x2AB3, 0x3A92,
    0xFD2E, 0xED0F, 0xDD6C, 0xCD4D, 0xBDAA, 0xAD8B, 0x9DE8, 0x8DC9,
    0x7C26, 0x6C07, 0x5C64, 0x4C45, 0x3CA2, 0x2C83, 0x1CE0, 0x0CC1,
    0xEF1F, 0xFF3E, 0xCF5D, 0xDF7C, 0xAF9B, 0xBFBA, 0x8FD9, 0x9FF8,
    0x6E17, 0x7E36, 0x4E55, 0x5E74, 0x2E93, 0x3EB2, 0x0ED1, 0x1EF0
};

/**
 * @brief 计算CRC16
 */
INLINE uint16_t uart_calc_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0x0000U;
    uint32_t i;

    if (data != NULL)
    {
        for (i = 0U; i < len; i++)
        {
            crc = (uint16_t)((crc << 8) ^ crc16_table[(uint8_t)((crc >> 8) ^ data[i])]);
        }
    }

    return crc;
}

/**
 * @brief 构建简单协议帧
 * @param buffer 输出缓冲区
 * @param cmd_class 命令类
 * @param cmd 命令
 * @param data 数据
 * @param data_len 数据长度
 * @return 帧总长度
 */
INLINE uint32_t uart_build_simple_frame(uint8_t *buffer, uint8_t cmd_class, uint8_t cmd,
                                      const uint8_t *data, uint16_t data_len)
{
    uint32_t idx = 0U;
    uint16_t crc;
    uint16_t payload_len;
    uint32_t i;

    if (buffer == NULL)
    {
        return 0U;
    }

    payload_len = (uint16_t)(2U + data_len);  /* cmd_class + cmd + data */

    /* Header */
    buffer[idx] = SIMPLE_FRAME_HEADER;
    idx++;

    /* Length (little endian) */
    buffer[idx] = (uint8_t)(payload_len & 0xFFU);
    idx++;
    buffer[idx] = (uint8_t)((payload_len >> 8) & 0xFFU);
    idx++;

    /* Command class and command */
    buffer[idx] = cmd_class;
    idx++;
    buffer[idx] = cmd;
    idx++;

    /* Data */
    if ((data != NULL) && (data_len > 0U))
    {
        for (i = 0U; i < data_len; i++)
        {
            buffer[idx] = data[i];
            idx++;
        }
    }

    /* CRC16 (over cmd_class + cmd + data) */
    crc = uart_calc_crc16(&buffer[3], payload_len);
    buffer[idx] = (uint8_t)(crc & 0xFFU);
    idx++;
    buffer[idx] = (uint8_t)((crc >> 8) & 0xFFU);
    idx++;

    /* Tail */
    buffer[idx] = SIMPLE_FRAME_TAIL;
    idx++;

    return idx;
}

#ifdef __cplusplus
}
#endif

#endif /* UART_PROTOCOL_H */
