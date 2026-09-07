/**
 * @file types.h
 * @brief 公共类型定义，用于MCU裸板环境
 *
 * 本文件提供了跨平台的基础类型定义，可直接用于MCU裸板程序
 */

#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* 布尔类型 - 兼容MCU环境 */
#ifndef BOOL_DEFINED
#define BOOL_DEFINED
typedef uint8_t bool_t;
#define TRUE  1
#define FALSE 0
#endif

/* NULL定义 */
#ifndef NULL
#define NULL ((void*)0)
#endif

/* 断言宏 - 可根据平台自定义 */
#ifndef ASSERT
#ifdef DEBUG
    #include <stdio.h>
    #define ASSERT(expr) do { \
        if (!(expr)) { \
            printf("ASSERT failed: %s, file %s, line %d\n", #expr, __FILE__, __LINE__); \
            while(1); \
        } \
    } while(0)
#else
    #define ASSERT(expr) ((void)0)
#endif
#endif

/* 内联函数 */
#ifndef INLINE
#define INLINE static inline
#endif

/* 弱符号 */
#ifndef WEAK
#if defined(__GNUC__)
#define WEAK __attribute__((weak))
#else
#define WEAK
#endif
#endif

/* 内存屏障 - ARM Cortex-M系列 */
#ifndef DMB
#if defined(__ARM_ARCH)
#define DMB() __asm volatile ("dmb" ::: "memory")
#else
#define DMB() ((void)0)
#endif
#endif

/* 数组大小计算 */
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* 最小/最大值 */
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

#endif /* TYPES_H */
