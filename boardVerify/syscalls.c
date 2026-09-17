/**
 * @file syscalls.c
 * @brief Minimal newlib stubs for bare-metal link with --specs=nosys.specs.
 *
 * _write routes stdout/stderr to USART1 when the HAL handle is ready.
 */
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "stm32f1xx_hal.h"

extern UART_HandleTypeDef huart1;
extern char _end; /* from linker script */
static char *heap_end;

void *_sbrk(ptrdiff_t incr)
{
    char *prev;
    if (heap_end == 0) {
        heap_end = &_end;
    }
    prev = heap_end;
    heap_end += incr;
    return prev;
}

int _kill(int pid, int sig)
{
    (void)pid;
    (void)sig;
    errno = EINVAL;
    return -1;
}

int _getpid(void)
{
    return 1;
}

void _exit(int status)
{
    (void)status;
    for (;;) {
    }
}

int _write(int fd, const void *buf, size_t count)
{
    if ((fd != STDOUT_FILENO) && (fd != STDERR_FILENO)) {
        errno = EBADF;
        return -1;
    }
    if ((buf == NULL) || (count == 0U)) {
        return 0;
    }
    /* Best-effort: ignore HAL status so printf never hard-faults the link. */
    if (huart1.Instance != NULL) {
        (void)HAL_UART_Transmit(
            &huart1, (uint8_t *)buf, (uint16_t)count, 100U);
    }
    return (int)count;
}

int _read(int fd, void *buf, size_t count)
{
    (void)fd;
    (void)buf;
    (void)count;
    return 0;
}

int _close(int fd)
{
    (void)fd;
    return -1;
}

int _fstat(int fd, struct stat *st)
{
    (void)fd;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int fd)
{
    (void)fd;
    return 1;
}

int _lseek(int fd, int ptr, int dir)
{
    (void)fd;
    (void)ptr;
    (void)dir;
    return 0;
}
