/*
 * console_io.c
 *
 * Minimal console I/O (printf-style output + character input) over the
 * FSP SCI UART. See console_io.h.
 */
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include "hal_data.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "console_io.h"

/* console_io_printf() prints 'l' arguments through the 32-bit number
 * formatter, so long must be 32 bits wide (true on the Arm Cortex-M targets
 * this module is built for). */
#if (LONG_MAX != INT32_MAX)
 #error "console_io assumes a 32-bit long"
#endif

/* Length modifiers accepted by console_io_printf(). */
typedef enum
{
    CONSOLE_IO_LEN_NONE,         /* int / unsigned int        */
    CONSOLE_IO_LEN_HH,           /* 'hh': signed / unsigned char  */
    CONSOLE_IO_LEN_H,            /* 'h' : short / unsigned short  */
    CONSOLE_IO_LEN_L,            /* 'l' : long / unsigned long    */
    CONSOLE_IO_LEN_UNSUPPORTED   /* 'll', 'j', 'z', 't', 'L', ... */
} console_io_len_t;

/* Bytes formatted on the caller's stack before they are queued for sending. A message longer than this is queued
 * in pieces (each piece whole or dropped), so keep a line within this size to keep it in one piece. */
#ifndef CONSOLE_IO_BUF_SIZE
#define CONSOLE_IO_BUF_SIZE        (96u)
#endif

/* TX ring buffer size in bytes; must be a power of two. A print copies its text into it and returns; the UART
 * empties it in the background, like an RTT up-buffer. */
#ifndef CONSOLE_IO_TX_RING_SIZE
#define CONSOLE_IO_TX_RING_SIZE    (256u)
#endif
#define CONSOLE_IO_TX_MASK         (CONSOLE_IO_TX_RING_SIZE - 1u)

/* A transfer whose TX-complete does not arrive within this time is aborted and its bytes are dropped, so a missing
 * interrupt can never stall the output for good. */
#ifndef CONSOLE_IO_TX_TIMEOUT_MS
#define CONSOLE_IO_TX_TIMEOUT_MS   (100u)
#endif

/* What a print does when the ring has no room for its text (a burst longer than the ring):
 *   CONSOLE_IO_FULL_WAIT: wait, in a task, until the UART has made room (bounded by twice the timeout above), so a
 *                         long report such as a card dump is not cut. Nothing waits while the ring has room.
 *   CONSOLE_IO_FULL_DROP: never wait. The message is dropped and the next one is preceded by "[N dropped]".
 * Calls from an ISR, or with interrupts masked, cannot wait and always drop. */
#define CONSOLE_IO_FULL_DROP       (0)
#define CONSOLE_IO_FULL_WAIT       (1)

#ifndef CONSOLE_IO_TX_FULL_POLICY
#define CONSOLE_IO_TX_FULL_POLICY  CONSOLE_IO_FULL_WAIT
#endif

/* Busy-wait guard for waiting for room before the FreeRTOS scheduler has started. */
#ifndef CONSOLE_IO_TX_WAIT_LOOPS
#define CONSOLE_IO_TX_WAIT_LOOPS   (0x00100000u)
#endif

#if (0u != (CONSOLE_IO_TX_RING_SIZE & CONSOLE_IO_TX_MASK))
 #error "CONSOLE_IO_TX_RING_SIZE must be a power of two"
#endif

#if (CONSOLE_IO_BUF_SIZE > CONSOLE_IO_TX_RING_SIZE)
 #error "CONSOLE_IO_BUF_SIZE must not exceed CONSOLE_IO_TX_RING_SIZE"
#endif

/* RX ring buffer size in bytes; must be a power of two (<= 65536). One slot is
 * kept free, so it holds up to (size - 1) unread characters. Sized so that a
 * full-length CLI line (255 characters + CR/LF) typed or pasted while the
 * application is busy fits, with room to spare. */
#define CONSOLE_IO_RX_BUF_SIZE     (512u)
#define CONSOLE_IO_RX_MASK         (CONSOLE_IO_RX_BUF_SIZE - 1u)

#if (0u != (CONSOLE_IO_RX_BUF_SIZE & CONSOLE_IO_RX_MASK))
 #error "CONSOLE_IO_RX_BUF_SIZE must be a power of two"
#endif

/* UART receive errors: the received character is missing or corrupted. */
#define CONSOLE_IO_RX_ERR_EVENTS   (UART_EVENT_ERR_PARITY | UART_EVENT_ERR_FRAMING | UART_EVENT_ERR_OVERFLOW)

static bool          s_open     = false;
static fsp_err_t     s_open_err = FSP_SUCCESS;  /* first UART init error, latched */

/* TX ring. s_tx_head and s_tx_tail are free-running indices (used = head - tail). Everything below is changed only
 * with interrupts masked, from tasks, ISRs and the UART callback alike. */
static uint8_t           s_tx_ring[CONSOLE_IO_TX_RING_SIZE];
static volatile uint32_t s_tx_head    = 0u;  /* next byte to write */
static volatile uint32_t s_tx_tail    = 0u;  /* next byte to send */
static volatile uint32_t s_tx_len     = 0u;  /* bytes of the transfer on the wire, 0 = UART idle */
static volatile uint32_t s_tx_start   = 0u;  /* tick at which that transfer was started */
static volatile uint32_t s_tx_dropped = 0u;  /* messages dropped and not yet reported */

static volatile uint8_t  s_rx_buf[CONSOLE_IO_RX_BUF_SIZE];
static volatile uint16_t s_rx_head = 0u;  /* written only by the RX interrupt */
static volatile uint16_t s_rx_tail = 0u;  /* written only by console_io_getc() */

/* Set by the RX interrupt when input is lost (ring full or UART receive
 * error); cleared only by console_io_getc() once it has reported the loss.
 * While it is set the interrupt stores nothing, so the gap is always at the
 * end of the buffered data and is reported in stream order. */
static volatile bool     s_rx_lost = false;

/* Given by the RX interrupt whenever it stores a character or records an input
 * loss; console_io_getc() blocks on it while the ring is empty, so the waiting
 * task does not spin and other tasks can run. A dedicated binary semaphore is
 * used instead of a task notification because notification index 0 of the
 * calling task is already used by the PTX105R IRQ wait in the NFC module
 * (configTASK_NOTIFICATION_ARRAY_ENTRIES is 1), and a stray console wake-up
 * there would end that wait early. Created in console_open() before the UART
 * is opened, so the interrupt never sees it uninitialised. */
static StaticSemaphore_t s_rx_sem_buf;
static SemaphoreHandle_t s_rx_sem = NULL;

static inline uint32_t console_irq_save (void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();

    return primask;
}

static inline void console_irq_restore (uint32_t primask)
{
    __set_PRIMASK(primask);
}

static uint32_t console_now (void)
{
    return (0u != __get_IPSR()) ? (uint32_t) xTaskGetTickCountFromISR() : (uint32_t) xTaskGetTickCount();
}

/* Start the next contiguous piece of the TX ring. Interrupts must be masked by the caller. If the write is refused
 * the ring is left as it is and the next print (or TX-complete) tries again. */
static void console_tx_kick (void)
{
    uint32_t used = s_tx_head - s_tx_tail;

    if ((0u == used) || (0u != s_tx_len))
    {
        return;
    }

    uint32_t index = s_tx_tail & CONSOLE_IO_TX_MASK;
    uint32_t n     = CONSOLE_IO_TX_RING_SIZE - index;

    if (n > used)
    {
        n = used;
    }

    if (FSP_SUCCESS == g_uart_jlob_vcom.p_api->write(g_uart_jlob_vcom.p_ctrl, &s_tx_ring[index], n))
    {
        s_tx_len   = n;
        s_tx_start = console_now();
    }
}

/* TX-complete from the UART interrupt: the piece on the wire is gone, send the next one. A completion that arrives
 * when nothing is on the wire (after an abort) is ignored. */
static void console_tx_done (void)
{
    uint32_t primask = console_irq_save();

    if (0u != s_tx_len)
    {
        s_tx_tail += s_tx_len;
        s_tx_len   = 0u;
        console_tx_kick();
    }

    console_irq_restore(primask);
}

/* If the transfer on the wire has not completed in time, abort it and drop its bytes. Interrupts masked by the
 * caller. If the abort fails the UART still owns the bytes, so they stay in the ring and the check repeats later. */
static void console_tx_check_stall (void)
{
    uint32_t len = s_tx_len;

    if ((0u != len) && ((console_now() - s_tx_start) >= (uint32_t) pdMS_TO_TICKS(CONSOLE_IO_TX_TIMEOUT_MS)))
    {
        s_tx_len = 0u;   /* so a completion raised by the abort itself is ignored */

        if (FSP_SUCCESS == g_uart_jlob_vcom.p_api->communicationAbort(g_uart_jlob_vcom.p_ctrl, UART_DIR_TX))
        {
            s_tx_tail += len;
            s_tx_dropped++;
        }
        else
        {
            s_tx_len   = len;
            s_tx_start = console_now();
        }
    }
}

/* Wake a task blocked in console_io_getc(). Called from the UART interrupt. */
static void console_rx_signal_from_isr (void)
{
    if (NULL != s_rx_sem)
    {
        BaseType_t higher_prio_task_woken = pdFALSE;

        (void) xSemaphoreGiveFromISR(s_rx_sem, &higher_prio_task_woken);
        portYIELD_FROM_ISR(higher_prio_task_woken);
    }
}

static void console_uart_callback (uart_callback_args_t * p_args)
{
    if (UART_EVENT_TX_COMPLETE == p_args->event)
    {
        console_tx_done();
    }
    else if (UART_EVENT_RX_CHAR == p_args->event)
    {
        uint16_t next = (uint16_t) ((s_rx_head + 1u) & CONSOLE_IO_RX_MASK);

        if (s_rx_lost)
        {
            /* A loss is still waiting to be reported: drop. */
        }
        else if (next == s_rx_tail)
        {
            s_rx_lost = true;   /* ring full: this character is lost */
            console_rx_signal_from_isr();
        }
        else
        {
            s_rx_buf[s_rx_head] = (uint8_t) p_args->data;
            s_rx_head           = next;
            console_rx_signal_from_isr();
        }
    }
    else if (0u != ((uint32_t) p_args->event & (uint32_t) CONSOLE_IO_RX_ERR_EVENTS))
    {
        if (!s_rx_lost)
        {
            s_rx_lost = true;
            console_rx_signal_from_isr();
        }
    }
    else
    {
        /* Other events are ignored. */
    }
}

static bool console_open_locked (void)
{
    if (s_open)
    {
        return true;
    }

    /* Initialisation already failed: do not retry on every call. */
    if (FSP_SUCCESS != s_open_err)
    {
        return false;
    }

    /* Static allocation: never fails. Created before the UART is opened, so
     * the RX interrupt can always signal it. */
    if (NULL == s_rx_sem)
    {
        s_rx_sem = xSemaphoreCreateBinaryStatic(&s_rx_sem_buf);
    }

    fsp_err_t err = g_uart_jlob_vcom.p_api->open(g_uart_jlob_vcom.p_ctrl, g_uart_jlob_vcom.p_cfg);

    if (FSP_SUCCESS == err)
    {
        err = g_uart_jlob_vcom.p_api->callbackSet(g_uart_jlob_vcom.p_ctrl, console_uart_callback, NULL, NULL);

        if (FSP_SUCCESS != err)
        {
            /* Do not leave the UART open without our callback. */
            (void) g_uart_jlob_vcom.p_api->close(g_uart_jlob_vcom.p_ctrl);
        }
    }

    if (FSP_SUCCESS != err)
    {
        s_open_err = err;

        return false;
    }

    s_open = true;

    return true;
}

/* Opens once. Interrupts are masked while it runs (first call only), so two tasks making their first call at the
 * same time cannot both open the UART. */
static bool console_open (void)
{
    if (s_open)
    {
        return true;
    }

    uint32_t primask = console_irq_save();
    bool     ok      = console_open_locked();

    console_irq_restore(primask);

    return ok;
}

fsp_err_t console_io_open (void)
{
    return console_open() ? FSP_SUCCESS : s_open_err;
}

/* One message being formatted on the caller's stack. */
typedef struct
{
    uint8_t  buf[CONSOLE_IO_BUF_SIZE];
    uint32_t len;     /* bytes in buf, not yet queued */
    uint32_t count;   /* characters accepted so far */
    bool     lost;    /* some of this call's output was dropped */
} console_msg_t;

/* Queue len bytes as one unit. Returns false if they do not fit or the UART cannot be opened. */
static bool console_ring_try (const uint8_t * p_src, uint32_t len)
{
    if (!console_open())
    {
        return false;
    }

    bool     ok      = false;
    uint32_t primask = console_irq_save();

    console_tx_check_stall();

    if (len <= (CONSOLE_IO_TX_RING_SIZE - (s_tx_head - s_tx_tail)))
    {
        for (uint32_t i = 0u; i < len; i++)
        {
            s_tx_ring[(s_tx_head + i) & CONSOLE_IO_TX_MASK] = p_src[i];
        }

        s_tx_head += len;
        ok         = true;
    }

    console_tx_kick();   /* also retries a drain that stalled after a refused write */

    console_irq_restore(primask);

    return ok;
}

#if (CONSOLE_IO_FULL_WAIT == CONSOLE_IO_TX_FULL_POLICY)

/* The ring had no room: wait for the UART to make some. Only from a task with interrupts enabled. */
static bool console_ring_wait (const uint8_t * p_src, uint32_t len)
{
    if ((0u != __get_IPSR()) || (0u != __get_PRIMASK()))
    {
        return false;
    }

    if (taskSCHEDULER_RUNNING == xTaskGetSchedulerState())
    {
        uint32_t start   = console_now();
        uint32_t timeout = 2u * (uint32_t) pdMS_TO_TICKS(CONSOLE_IO_TX_TIMEOUT_MS);

        while ((console_now() - start) < timeout)
        {
            vTaskDelay(1);

            if (console_ring_try(p_src, len))
            {
                return true;
            }
        }
    }
    else
    {
        /* No scheduler yet, so no tick: poll with interrupts enabled so the UART interrupt can drain the ring. */
        for (uint32_t n = 0u; n < CONSOLE_IO_TX_WAIT_LOOPS; n++)
        {
            if ((0u == (n & 0xFFu)) && console_ring_try(p_src, len))
            {
                return true;
            }
        }
    }

    return false;
}

#endif

static bool console_ring_put_msg (console_msg_t * p_msg)
{
    bool ok = console_ring_try(p_msg->buf, p_msg->len);

#if (CONSOLE_IO_FULL_WAIT == CONSOLE_IO_TX_FULL_POLICY)
    if (!ok)
    {
        ok = console_ring_wait(p_msg->buf, p_msg->len);
    }
#endif

    return ok;
}

static void console_msg_flush (console_msg_t * p_msg)
{
    if (0u != p_msg->len)
    {
        if (!console_ring_put_msg(p_msg))
        {
            uint32_t primask = console_irq_save();

            s_tx_dropped++;

            console_irq_restore(primask);

            p_msg->lost = true;
        }

        p_msg->len = 0u;
    }
}

static void console_putc (console_msg_t * p_msg, char c)
{
    p_msg->buf[p_msg->len] = (uint8_t) c;
    p_msg->len++;
    p_msg->count++;

    if (p_msg->len >= CONSOLE_IO_BUF_SIZE)
    {
        console_msg_flush(p_msg);
    }
}

static void console_puts (console_msg_t * p_msg, const char * p_str)
{
    if (NULL == p_str)
    {
        p_str = "(null)";
    }

    while ('\0' != *p_str)
    {
        console_putc(p_msg, *p_str);
        p_str++;
    }
}

static void console_put_num (console_msg_t * p_msg, uint32_t value, uint32_t base, bool upper, uint32_t width, char pad, bool negative)
{
    static const char digits_lc[] = "0123456789abcdef";
    static const char digits_uc[] = "0123456789ABCDEF";
    const char *      p_digits    = upper ? digits_uc : digits_lc;
    char              tmp[10];    /* 0xFFFFFFFF = 4294967295: 10 digits max */
    uint32_t          n = 0u;

    do
    {
        tmp[n] = p_digits[value % base];
        n++;
        value /= base;
    } while (0u != value);

    if (negative)
    {
        width = (width > 0u) ? (width - 1u) : 0u;

        if ('0' == pad)
        {
            console_putc(p_msg, '-');
        }
    }

    while (width > n)
    {
        console_putc(p_msg, pad);
        width--;
    }

    if (negative && (' ' == pad))
    {
        console_putc(p_msg, '-');
    }

    while (n > 0u)
    {
        n--;
        console_putc(p_msg, tmp[n]);
    }
}

/* If earlier messages were dropped, say so before the next one. The caller's own message state is left untouched;
 * if even the notice does not fit, the count is kept for next time. */
static void console_report_dropped (console_msg_t * p_msg)
{
    uint32_t primask = console_irq_save();
    uint32_t dropped = s_tx_dropped;

    s_tx_dropped = 0u;

    console_irq_restore(primask);

    if (0u != dropped)
    {
        console_putc(p_msg, '[');
        console_put_num(p_msg, dropped, 10u, false, 0u, ' ', false);
        console_puts(p_msg, " dropped]\r\n");

        if (!console_ring_try(p_msg->buf, p_msg->len))
        {
            primask = console_irq_save();
            s_tx_dropped += dropped;
            console_irq_restore(primask);
        }

        p_msg->len   = 0u;
        p_msg->count = 0u;
        p_msg->lost  = false;
    }
}

int console_io_printf (const char * p_format, ...)
{
    va_list args;

    if (NULL == p_format)
    {
        return -1;
    }

    bool          format_error = false;
    console_msg_t msg;

    msg.len   = 0u;
    msg.count = 0u;
    msg.lost  = false;

    console_report_dropped(&msg);

    va_start(args, p_format);

    for (const char * p = p_format; '\0' != *p; p++)
    {
        if ('%' != *p)
        {
            console_putc(&msg, *p);
            continue;
        }

        p++;

        char     pad   = ' ';
        uint32_t width = 0u;

        if ('0' == *p)
        {
            pad = '0';
            p++;
        }

        while ((*p >= '0') && (*p <= '9'))
        {
            width = (width * 10u) + (uint32_t) (*p - '0');
            p++;
        }

        /* Length modifier. Each supported one is read with its own va_arg
         * type below; anything else (e.g. 'll' = 64-bit) cannot be read
         * safely, so formatting stops there. */
        console_io_len_t len = CONSOLE_IO_LEN_NONE;

        if ('h' == *p)
        {
            p++;
            len = CONSOLE_IO_LEN_H;

            if ('h' == *p)
            {
                p++;
                len = CONSOLE_IO_LEN_HH;
            }
        }
        else if ('l' == *p)
        {
            p++;
            len = CONSOLE_IO_LEN_L;

            if ('l' == *p)
            {
                len = CONSOLE_IO_LEN_UNSUPPORTED;
            }
        }
        else if (('j' == *p) || ('z' == *p) || ('t' == *p) || ('L' == *p) || ('q' == *p))
        {
            len = CONSOLE_IO_LEN_UNSUPPORTED;
        }
        else
        {
            /* No length modifier. */
        }

        /* %c and %s take no length modifier ('l' would mean wide characters). */
        if ((CONSOLE_IO_LEN_UNSUPPORTED == len) ||
            ((CONSOLE_IO_LEN_NONE != len) && (('c' == *p) || ('s' == *p))))
        {
            /* The argument type is unknown, so no further argument can be
             * read safely: print a marker, skip the rest of the format and
             * report the call as failed. */
            console_putc(&msg, '%');
            console_putc(&msg, '?');
            format_error = true;
            break;
        }

        switch (*p)
        {
            case 'c':
            {
                console_putc(&msg, (char) va_arg(args, int));
                break;
            }

            case 's':
            {
                console_puts(&msg, va_arg(args, const char *));
                break;
            }

            case 'd':
            case 'i':
            {
                int32_t v;

                if (CONSOLE_IO_LEN_L == len)
                {
                    v = (int32_t) va_arg(args, long);
                }
                else
                {
                    v = (int32_t) va_arg(args, int);   /* char / short arrive promoted to int */

                    if (CONSOLE_IO_LEN_H == len)
                    {
                        v = (int32_t) (short) v;
                    }
                    else if (CONSOLE_IO_LEN_HH == len)
                    {
                        v = (int32_t) (signed char) v;
                    }
                    else
                    {
                        /* Plain int. */
                    }
                }

                console_put_num(&msg, (v < 0) ? (0u - (uint32_t) v) : (uint32_t) v, 10u, false, width, pad, v < 0);
                break;
            }

            case 'u':
            case 'x':
            case 'X':
            {
                uint32_t v;

                if (CONSOLE_IO_LEN_L == len)
                {
                    v = (uint32_t) va_arg(args, unsigned long);
                }
                else
                {
                    v = (uint32_t) va_arg(args, unsigned int);   /* char / short arrive promoted */

                    if (CONSOLE_IO_LEN_H == len)
                    {
                        v = (uint32_t) (unsigned short) v;
                    }
                    else if (CONSOLE_IO_LEN_HH == len)
                    {
                        v = (uint32_t) (unsigned char) v;
                    }
                    else
                    {
                        /* Plain unsigned int. */
                    }
                }

                console_put_num(&msg, v, ('u' == *p) ? 10u : 16u, ('X' == *p), width, pad, false);
                break;
            }

            case '%':
            {
                console_putc(&msg, '%');
                break;
            }

            case '\0':
            {
                /* Trailing '%': stop (step back so the loop terminates). */
                p--;
                break;
            }

            default:
            {
                /* Unknown conversion: print it verbatim. */
                console_putc(&msg, '%');
                console_putc(&msg, *p);
                break;
            }
        }
    }

    va_end(args);

    console_msg_flush(&msg);

    return (msg.lost || format_error) ? -1 : (int) msg.count;
}

int console_io_putc (char c)
{
    console_msg_t msg;

    msg.len   = 0u;
    msg.count = 0u;
    msg.lost  = false;

    console_putc(&msg, c);
    console_msg_flush(&msg);

    return msg.lost ? -1 : (int) (uint8_t) c;
}

int console_io_getc (void)
{
    if (!console_open())
    {
        return -1;
    }

    for (;;)
    {
        /* Read the flag before checking for data. Once it is set the RX
         * interrupt stores nothing more, so if the buffer is then empty every
         * character received before the loss has already been returned. */
        bool lost = s_rx_lost;

        if (s_rx_head != s_rx_tail)
        {
            uint8_t c = s_rx_buf[s_rx_tail];
            s_rx_tail = (uint16_t) ((s_rx_tail + 1u) & CONSOLE_IO_RX_MASK);

            return (int) c;
        }

        if (lost)
        {
            s_rx_lost = false;

            return CONSOLE_IO_RX_LOST;
        }

        /* Nothing to return yet: block until the RX interrupt signals a new
         * character or an input loss, then check again. If it fired after the
         * checks above, the semaphore is already given and this returns at
         * once, so no wake-up is missed. Before the scheduler starts there is
         * no task to block, so keep polling. */
        if (taskSCHEDULER_RUNNING == xTaskGetSchedulerState())
        {
            (void) xSemaphoreTake(s_rx_sem, portMAX_DELAY);
        }
    }
}
