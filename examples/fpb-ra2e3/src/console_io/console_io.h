/*
 * console_io.h
 *
 * Minimal console I/O over the FSP SCI UART (g_uart_jlob_vcom).
 * Replaces the picolibc stdio retarget (rs-console-io): no libc printf /
 * getchar, no stdout/stdin FILE, no byte queue module, no malloc.
 *
 * Output - console_io_printf() supports: %c %s %d %i %u %x %X %%
 *   flags  : '0' (zero pad)
 *   width  : decimal field width (numbers only), e.g. %02X, %5u
 *   length : 'hh', 'h', 'l' for %d %i %u %x %X - each is read with its own
 *            argument type (long is 32 bits on this target)
 * Not supported: floating point, 64-bit ('ll', 'j'), 'z', 't', 'L', precision,
 * '-' / '+' flags. An unsupported length modifier (or one on %c / %s) prints
 * "%?" and ends the output at that point, since the argument type is unknown;
 * the call then returns -1.
 * Output is buffered and non-blocking, like an RTT up-buffer. Each call
 * formats into a small buffer on the caller's stack (CONSOLE_IO_BUF_SIZE) and
 * copies the result into a RAM ring buffer (CONSOLE_IO_TX_RING_SIZE); the
 * UART empties the ring in the background from its TX-complete callback. A
 * call therefore costs formatting plus a short copy and never waits for the
 * wire while the ring has room. If a burst is longer than the ring, a print
 * from a task waits until the UART has made room (CONSOLE_IO_FULL_WAIT, the
 * default), or the message is dropped and the next one is preceded by a
 * "[N dropped]" line (CONSOLE_IO_FULL_DROP). A print from an ISR, or with
 * interrupts masked, cannot wait and always drops. A message longer than
 * CONSOLE_IO_BUF_SIZE is queued in pieces, each piece whole or dropped. If the UART cannot be opened, a write is refused, or TX-complete
 * does not arrive within CONSOLE_IO_TX_TIMEOUT_MS (the transfer is then
 * aborted), the affected output is dropped. A call that lost output returns -1.
 *
 * Input - received bytes are stored by the UART RX interrupt in a ring
 * buffer (CONSOLE_IO_RX_BUF_SIZE); console_io_getc() takes them out. While
 * the buffer is empty the calling task blocks on a FreeRTOS binary semaphore
 * that the RX interrupt gives, so it uses no CPU while waiting. If input
 * is lost (buffer full or UART receive error), console_io_getc() returns
 * CONSOLE_IO_RX_LOST at the point in the stream where the gap occurred.
 *
 * Output (console_io_printf, console_io_putc) is safe from several tasks and,
 * for short messages, from an ISR; the ring is changed only with interrupts
 * masked. The first call opens the UART with interrupts masked, so make it
 * from a task. Input (console_io_getc) is for a single reading task.
 */

#ifndef CONSOLE_IO_H_
#define CONSOLE_IO_H_

#include "bsp_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** console_io_getc() result: received input was lost before this point. */
#define CONSOLE_IO_RX_LOST    (-2)

/**
 * Open the UART and install the console callback (done lazily by every other
 * console_io function too). Returns FSP_SUCCESS, or the FSP error of the
 * failed open()/callbackSet(). The first failure is latched: the UART is
 * closed again and later calls return the same error without retrying, so
 * all console_io output is dropped and console_io_getc() returns -1.
 */
fsp_err_t console_io_open(void);

/**
 * Formatted print to the UART console (lazy-opens the UART on first use).
 * Returns the number of characters queued for sending, or -1 if any output
 * was lost or the format contained an unsupported length modifier.
 */
int console_io_printf(const char * p_format, ...) __attribute__((format(printf, 1, 2)));

/**
 * Queue one character for sending (e.g. local echo).
 * Returns the character (0..255), or -1 if it was not queued.
 */
int console_io_putc(char c);

/**
 * Block until one character is received on the UART and return it (0..255).
 * The calling task sleeps while no input is available (it polls only if
 * called before the FreeRTOS scheduler has started).
 * Opens the UART on first use, so reception starts with the first call to
 * any console_io function.
 * Returns CONSOLE_IO_RX_LOST once after input was lost (all characters
 * received before the loss are returned first), or -1 if the UART cannot be
 * opened.
 */
int console_io_getc(void);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_IO_H_ */
