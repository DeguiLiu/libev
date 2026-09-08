
/******************************************************************************
 * @file    spsc_queue.h
 * @brief   Single Producer Single Consumer (SPSC) Lock-Free Queue
 ******************************************************************************/
#ifndef SPSC_QUEUE_H
#define SPSC_QUEUE_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/*
 * @brief Memory barrier macro for ordering memory operations.
 *        On ARM GCC, emits a 'dmb' (data memory barrier) instruction.
 *        On other compilers, it is a no-op.
 *        Ensures that all memory accesses before the barrier are completed
 *        before any after the barrier, critical for lock-free concurrency.
 */
#ifndef SPSC_QUEUE_DMB
    #define SPSC_QUEUE_DMB()  ((void)0)
#endif

#define SPSC_QUEUE_OK      (1)
#define SPSC_QUEUE_FAIL    (0)

/*
 * @brief Returns the minimum of two unsigned integers.
 * @param a First value.
 * @param b Second value.
 * @return Smaller of a and b.
 */
static inline uint32_t spsc_queue_min(uint32_t a, uint32_t b)
{
    uint32_t result = a;
    if (b < a)
    {
        result = b;
    }
    /* else, do nothing */
    return result;
}

/*
 * @struct spsc_queue_t
 * @brief  Single-producer, single-consumer lock-free ring buffer.
 *
 * @var size   The capacity of the buffer in bytes. Must be a power of 2.
 * @var mask   Bit mask for fast modulo operation (equals size - 1).
 * @var head   Index for consumer (read pointer). Only the consumer modifies this.
 * @var tail   Index for producer (write pointer). Only the producer modifies this.
 * @var buffer Pointer to the buffer memory.
 */
typedef struct
{
    uint32_t size;             /*!< Queue buffer size (must be power of 2) */
    uint32_t mask;             /*!< Fast modulo mask (size - 1) */
    volatile uint32_t head;    /*!< Consumer read index (modified by consumer only) */
    volatile uint32_t tail;    /*!< Producer write index (modified by producer only) */
    uint8_t *buffer;           /*!< Pointer to buffer memory */
} spsc_queue_t;

/*
 * @brief  Creates and initializes an SPSC queue.
 * @param  size Buffer size in bytes (must be a power of 2).
 * @return Pointer to the queue structure, or NULL on failure.
 */
static inline spsc_queue_t* spsc_queue_create(uint32_t size)
{
    spsc_queue_t *q = (spsc_queue_t*)NULL;
    uint8_t *buf = (uint8_t*)NULL;
    spsc_queue_t *result = (spsc_queue_t*)NULL;
    uint32_t valid_size = 0U;

    /* Check if size is power of 2 and non-zero */
    if ((size != 0U) && ((size & (size - 1U)) == 0U))
    {
        valid_size = 1U;
    }
    else
    {
        valid_size = 0U;
    }

    if (valid_size == 1U)
    {
        /* Allocate queue structure */
        q = (spsc_queue_t*)malloc(sizeof(spsc_queue_t));
        if (q != NULL)
        {
            /* Allocate buffer */
            buf = (uint8_t*)malloc(size);
            if (buf != NULL)
            {
                q->buffer = buf;
                q->size = size;
                q->mask = size - 1U;
                q->head = 0U;
                q->tail = 0U;
                result = q;
            }
            else
            {
                free(q);
                result = (spsc_queue_t*)NULL;
            }
        }
        else
        {
            result = (spsc_queue_t*)NULL;
        }
    }
    else
    {
        result = (spsc_queue_t*)NULL;
    }
    return result;
}

/*
 * @brief  Frees the memory used by an SPSC queue.
 * @param  q Pointer to the queue to destroy.
 * @return None.
 */
static inline void spsc_queue_destroy(spsc_queue_t *q)
{
    if (q != NULL)
    {
        if (q->buffer != NULL)
        {
            free(q->buffer);
            q->buffer = (uint8_t*)NULL;
        }
        free(q);
    }
}

/*
 * @brief  Pushes data into the queue (producer).
 * @param  q    Pointer to the queue.
 * @param  data Pointer to the data to push.
 * @param  len  Number of bytes to push.
 * @return SPSC_QUEUE_OK if successful, SPSC_QUEUE_FAIL if not enough space or error.
 * @note   Only the producer thread should call this function.
 */
static inline int32_t spsc_queue_push(spsc_queue_t *q, const uint8_t *data, uint32_t len)
{
    int32_t result = SPSC_QUEUE_FAIL;
    uint32_t tail = 0U;
    uint32_t head = 0U;
    uint32_t pos = 0U;
    uint32_t first = 0U;

    if ((q != NULL) && (data != NULL) && (len != 0U))
    {
        tail = q->tail;
        head = q->head;
        /* Check available space */
        if ((q->size - (tail - head)) >= len)
        {
            pos = tail & q->mask;
            first = spsc_queue_min(len, q->size - pos);
            (void)memcpy(&q->buffer[pos], data, first);
            if (first < len)
            {
                (void)memcpy(q->buffer, &data[first], len - first);
            }
            SPSC_QUEUE_DMB(); /* Ensure all data is written before updating tail */
            q->tail = tail + len;
            result = SPSC_QUEUE_OK;
        }
        else
        {
            /* Not enough space in queue */
            result = SPSC_QUEUE_FAIL;
        }
    }
    else
    {
        /* Invalid parameter */
        result = SPSC_QUEUE_FAIL;
    }

    return result;
}

/*
 * @brief  Pops data from the queue (consumer).
 * @param  q    Pointer to the queue.
 * @param  data Pointer to the buffer to store popped data.
 * @param  len  Number of bytes to pop.
 * @return Number of bytes actually popped (may be less than requested or 0).
 * @note   Only the consumer thread should call this function.
 */
static inline uint32_t spsc_queue_pop(spsc_queue_t *q, uint8_t *data, uint32_t len)
{
    uint32_t tail = 0U;
    uint32_t head = 0U;
    uint32_t avail = 0U;
    uint32_t pos = 0U;
    uint32_t first = 0U;
    uint32_t ret_len = 0U;

    if ((q != NULL) && (data != NULL) && (len != 0U))
    {
        tail = q->tail;
        head = q->head;
        avail = tail - head;
        /* Check if data is available */
        if (avail != 0U)
        {
            if (len > avail)
            {
                len = avail;
            }
            pos = head & q->mask;
            first = spsc_queue_min(len, q->size - pos);

            (void)memcpy(data, &q->buffer[pos], first);
            if (first < len)
            {
                (void)memcpy(&data[first], q->buffer, len - first);
            }
            SPSC_QUEUE_DMB(); /* Ensure all data is read before updating head */
            q->head = head + len;
            ret_len = len;
        }
        else
        {
            /* Queue is empty */
            ret_len = 0U;
        }
    }
    else
    {
        /* Invalid parameter */
        ret_len = 0U;
    }
    return ret_len;
}

/*
 * @brief  Gets the number of bytes currently stored in the queue.
 * @param  q Pointer to the queue.
 * @return Number of bytes in the queue, or 0 if q is NULL.
 */
static inline uint32_t spsc_queue_data_len(const spsc_queue_t *q)
{
    uint32_t result = 0U;
    if (q != NULL)
    {
        result = (q->tail - q->head);
    }
    else
    {
        result = 0U;
    }
    return result;
}

/*
 * @brief  Gets the number of free bytes remaining in the queue.
 * @param  q Pointer to the queue.
 * @return Number of free bytes, or 0 if q is NULL.
 */
static inline uint32_t spsc_queue_free_space(const spsc_queue_t *q)
{
    uint32_t result = 0U;
    if (q != NULL)
    {
        result = (q->size - (q->tail - q->head));
    }
    else
    {
        result = 0U;
    }
    return result;
}

#endif /* SPSC_QUEUE_H */
