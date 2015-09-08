/*
 * Copyright (c) 1994-2008 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _GNU_SOURCE

#include "buf.h"

#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <assert.h>

/* buffer handling functions */

static size_t roundup(size_t size)
{
    if (size < 32)
        return 32;
    if (size < 64)
        return 64;
    if (size < 128)
        return 128;
    if (size < 256)
        return 256;
    if (size < 512)
        return 512;
    return ((size + 1024) & ~1023);
}

/* this function has a side-effect of always leaving the buffer writable */
void _buf_ensure(struct buf *buf, size_t n)
{
    size_t newlen = buf->len + n;
    char *s;

    assert(newlen); /* we never alloc zero bytes */

    if (buf->alloc >= newlen)
        return;

    if (buf->alloc) {
        buf->alloc = roundup(newlen);
        buf->s = realloc(buf->s, buf->alloc);
    }
    else {
        buf->alloc = roundup(newlen);
        s = malloc(buf->alloc);

        /* if no allocation, but data exists, it means copy on write.
         * grab a copy of what's there now */
        if (buf->len) {
            assert(buf->s);
            memcpy(s, buf->s, buf->len);
        }

        buf->s = s;
    }
}

const char *buf_cstring(struct buf *buf)
{
    buf_ensure(buf, 1);
    buf->s[buf->len] = '\0';
    return buf->s;
}

char *buf_newcstring(struct buf *buf)
{
    char *ret = strdup(buf_cstring(buf));
    buf_reset(buf);
    return ret;
}

char *buf_release(struct buf *buf)
{
    char *ret = (char *)buf_cstring(buf);
    buf_init(buf);
    return ret;
}

const char *buf_cstringnull(struct buf *buf)
{
    if (!buf->s) return NULL;
    return buf_cstring(buf);
}

char *buf_releasenull(struct buf *buf)
{
    char *ret = (char *)buf_cstringnull(buf);
    buf_init(buf);
    return ret;
}

void buf_getmap(struct buf *buf, const char **base, size_t *len)
{
    *base = buf->s;
    *len = buf->len;
}

/* fetch a single line a file - terminated with \n ONLY.
 * buf does not contain the \n.
 * NOTE: if the final line does not contain a \n we still
 * return true so that the caller will process the line,
 * so a file A\nB will return two true responses with bufs
 * containing "A" and "B" respectively before returning a
 * false to the third call */
int buf_getline(struct buf *buf, FILE *fp)
{
    int c;

    buf_reset(buf);
    while ((c = fgetc(fp)) != EOF) {
        if (c == '\n')
            break;
        buf_putc(buf, c);
    }
    /* ensure trailing NULL */
    buf_cstring(buf);

    /* EOF and no content, we're done */
    return (!(buf->len == 0 && c == EOF));
}

size_t buf_len(const struct buf *buf)
{
    return buf->len;
}

const char *buf_base(const struct buf *buf)
{
    return buf->s;
}

void buf_reset(struct buf *buf)
{
    buf->len = 0;
}

void buf_truncate(struct buf *buf, ssize_t len)
{
    if (len < 0) {
        len = buf->len + len;
        if (len < 0) len = 0;
    }
    if ((size_t)len > buf->alloc) {
        /* grow the buffer and zero-fill the new bytes */
        size_t more = len - buf->len;
        buf_ensure(buf, more);
        memset(buf->s + buf->len, 0, more);
    }
    buf->len = len;
}

void buf_setcstr(struct buf *buf, const char *str)
{
    buf_setmap(buf, str, strlen(str));
}

void buf_setmap(struct buf *buf, const char *base, size_t len)
{
    buf_reset(buf);
    if (len) {
        buf_ensure(buf, len);
        memcpy(buf->s, base, len);
        buf->len = len;
    }
}

void buf_copy(struct buf *dst, const struct buf *src)
{
    buf_setmap(dst, src->s, src->len);
}

void buf_append(struct buf *dst, const struct buf *src)
{
    buf_appendmap(dst, src->s, src->len);
}

void buf_appendcstr(struct buf *buf, const char *str)
{
    buf_appendmap(buf, str, strlen(str));
}

void buf_appendmap(struct buf *buf, const char *base, size_t len)
{
    if (len) {
        buf_ensure(buf, len);
        memcpy(buf->s + buf->len, base, len);
        buf->len += len;
    }
}

/* This is like buf_appendmap() but attempts an optimisation where the
 * first append to an empty buf results in a read-only pointer to the
 * data at 'base' instead of a writable copy. */
void buf_cowappendmap(struct buf *buf, const char *base, unsigned int len)
{
    if (!buf->s)
        buf_init_ro(buf, base, len);
    else
        buf_appendmap(buf, base, len);
}

/* This is like buf_cowappendmap() but takes over the given map 'base',
 * which is a malloc()ed C string buffer of at least 'len' bytes. */
void buf_cowappendfree(struct buf *buf, char *base, unsigned int len)
{
    if (!buf->s)
        buf_initm(buf, base, len);
    else {
        buf_appendmap(buf, base, len);
        free(base);
    }
}

void buf_vprintf(struct buf *buf, const char *fmt, va_list args)
{
    va_list ap;
    int room;
    int n;

    /* Add some more room to the buffer.  We just guess a
     * size and rely on vsnprintf() to tell us if it
     * needs to overrun the size. */
    buf_ensure(buf, 1024);

    /* Copy args in case we guess wrong on the size */
    va_copy(ap, args);

    room = buf->alloc - buf->len;
    n = vsnprintf(buf->s + buf->len, room, fmt, args);

    if (n >= room) {
        /* woops, we guessed wrong...retry with enough space */
        buf_ensure(buf, n+1);
        n = vsnprintf(buf->s + buf->len, n+1, fmt, ap);
    }
    va_end(ap);

    buf->len += n;
}

void buf_printf(struct buf *buf, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    buf_vprintf(buf, fmt, args);
    va_end(args);
}

static void buf_replace_buf(struct buf *buf,
                            size_t offset,
                            size_t length,
                            const struct buf *replace)
{
    if (offset > buf->len) return;
    if (offset + length > buf->len)
        length = buf->len - offset;

    /* we need buf to be a writable C string now please */
    buf_cstring(buf);

    if (replace->len > length) {
        /* string will need to expand */
        buf_ensure(buf, replace->len - length + 1);
    }
    if (length != replace->len) {
        /* +1 to copy the NULL to keep cstring semantics */
        memmove(buf->s + offset + replace->len,
                buf->s + offset + length,
                buf->len - offset - length + 1);
        buf->len += (replace->len - length);
    }
    if (replace->len)
        memcpy(buf->s + offset, replace->s, replace->len);
}

/**
 * Replace all instances of the string literal @match in @buf
 * with the string @replace, which may be NULL to just remove
 * instances of @match.
 * Returns: the number of substitutions made.
 */
int buf_replace_all(struct buf *buf, const char *match,
                             const char *replace)
{
    int n = 0;
    int matchlen = strlen(match);
    struct buf replace_buf = BUF_INITIALIZER;
    size_t off;
    char *p;

    buf_init_ro_cstr(&replace_buf, replace);

    /* we need buf to be a nul terminated string now please */
    buf_cstring(buf);

    off = 0;
    while ((p = strstr(buf->s + off, match))) {
        off = (p - buf->s);
        buf_replace_buf(buf, off, matchlen, &replace_buf);
        n++;
        off += replace_buf.len;
    }

    return n;
}

int buf_replace_char(struct buf *buf, char match, char replace)
{
    int n = 0;
    size_t i;

    /* we need writable, so may as well cstring it */
    buf_cstring(buf);

    for (i = 0; i < buf->len; i++) {
        if (buf->s[i] == match) {
            buf->s[i] = replace;
            n++;
        }
    }

    return n;
}

void buf_insert(struct buf *dst, unsigned int off, const struct buf *src)
{
    buf_replace_buf(dst, off, 0, src);
}

void buf_insertcstr(struct buf *dst, unsigned int off, const char *str)
{
    struct buf str_buf = BUF_INITIALIZER;
    buf_init_ro_cstr(&str_buf, str);
    buf_replace_buf(dst, off, 0, &str_buf);
}

void buf_insertmap(struct buf *dst, unsigned int off,
                            const char *base, int len)
{
    struct buf map_buf = BUF_INITIALIZER;
    buf_init_ro(&map_buf, base, len);
    buf_replace_buf(dst, off, 0, &map_buf);
}

void buf_remove(struct buf *dst, unsigned int off, unsigned int len)
{
    struct buf empty_buf = BUF_INITIALIZER;
    buf_replace_buf(dst, off, len, &empty_buf);
}

/*
 * Compare two struct bufs bytewise.  Returns a number
 * like strcmp(), suitable for sorting e.g. with qsort(),
 */
#define MIN(a,b) ((a) < (b) ? (a) : (b))
int buf_cmp(const struct buf *a, const struct buf *b)
{
    size_t len = MIN(a->len, b->len);
    int r = 0;

    if (len)
        r = memcmp(a->s, b->s, len);

    if (!r) {
        if (a->len < b->len)
            r = -1;
        else if (a->len > b->len)
            r = 1;
    }

    return r;
}

void buf_init(struct buf *buf)
{
    buf->alloc = 0;
    buf->len = 0;
    buf->s = NULL;
}

/*
 * Initialise a struct buf to point to read-only data.  The key here is
 * setting buf->alloc=0 which indicates CoW is in effect, i.e. the data
 * pointed to needs to be copied should it ever be modified.
 */
void buf_init_ro(struct buf *buf, const char *base, size_t len)
{
    buf->alloc = 0;
    buf->len = len;
    buf->s = (char *)base;
}

/*
 * Initialise a struct buf to point to writable data at 'base', which
 * must be a malloc()ed allocation at least 'len' bytes long and is
 * taken over by the struct buf.
 */
void buf_initm(struct buf *buf, char *base, int len)
{
    buf->alloc = buf->len = len;
    buf->s = base;
}

/*
 * Initialise a struct buf to point to a read-only C string.
 */
void buf_init_ro_cstr(struct buf *buf, const char *str)
{
    buf->alloc = 0;
    buf->len = (str ? strlen(str) : 0);
    buf->s = (char *)str;
}

static void _buf_free_data(struct buf *buf)
{
    if (buf->alloc)
        free(buf->s);
}

void buf_free(struct buf *buf)
{
    _buf_free_data(buf);
    buf->alloc = 0;
    buf->s = NULL;
    buf->len = 0;
}

void buf_move(struct buf *dst, struct buf *src)
{
    _buf_free_data(dst);
    *dst = *src;
    buf_init(src);
}

int buf_findchar(const struct buf *buf, unsigned int off, int c)
{
    const char *p;

    if (off < buf->len && (p = memchr(buf->s + off, c, buf->len - off)))
        return (p - buf->s);
    return -1;
}

/*
 * Find (the first line in) 'line' in the buffer 'buf'.  The found text
 * will be a complete line, i.e. bounded by either \n newlines or by the
 * edges of 'buf'.  Returns the byte index into 'buf' of the found text,
 * or -1 if not found.
 */
int buf_findline(const struct buf *buf, const char *line)
{
    int linelen;
    const char *p;
    const char *end = buf->s + buf->len;

    if (!line) return -1;

    /* find the length of the first line in the text at 'line' */
    p = strchr(line, '\n');
    linelen = (p ? (size_t)(p - line) : strlen(line));
    if (linelen == 0) return -1;

    for (p = buf->s ;
         (p = (const char *)memmem(p, end-p, line, linelen)) != NULL ;
         p++) {

        /* check the found string is at line boundaries */
        if (p > buf->s && p[-1] != '\n')
            continue;
        if ((p+linelen) < end && p[linelen] != '\n')
            continue;

        return (p - buf->s);
    }

    return -1;
}

size_t buf_gets(struct buf *buf, char *out, size_t len)
{
    if (buf->len == 0 || len <= 1) {
        *out = '\0';
        return 0;
    }
    buf_cstring(buf);
    char *end = strchr(buf->s, '\n');
    if (end)
        end++;
    else
        end = buf->s + buf->len;
    char *s = buf->s;
    size_t c = 0;
    while (s != end && c < len-1) {
        out[c] = *s;
        s++; c++;
    }
    out[c] = '\0';
    buf_remove(buf, 0, c);
    return c;
}

size_t buf_recv(struct buf *buf, char *out, size_t len)
{
    size_t n = buf_peek(buf, out, len);
    buf_remove(buf, 0, n);
    return n;
}

size_t buf_peek(struct buf *buf, char *out, size_t len)
{
    if (buf->len == 0 || len <= 0)
        return 0;
    size_t n = MIN(len, buf->len);
    memcpy(out, buf->s, n);
    return n;
}
