/*
 * kcrt.c — the two CRT symbols GCC emits calls to even in freestanding
 * code (struct copies, zero-initialisation). Kernel modules link no CRT
 * and win32k.sys exports neither, so both drivers carry these. Built with
 * -fno-tree-loop-distribute-patterns so the loops are not turned back into
 * calls to themselves.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--) {
        *d++ = (unsigned char)c;
    }
    return dst;
}
