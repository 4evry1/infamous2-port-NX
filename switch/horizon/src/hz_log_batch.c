#include "hz_log_batch.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct hz_log_sink {
    int fd, needs_sync;
    pthread_mutex_t queue, consumer;
    size_t head, used;
    atomic_uint_fast64_t dropped, errors;
    unsigned char bytes[HZ_LOG_CAPACITY];
};

hz_log_sink *hz_log_create(int fd)
{
    hz_log_sink *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    if (pthread_mutex_init(&s->queue, NULL) != 0) { free(s); return NULL; }
    if (pthread_mutex_init(&s->consumer, NULL) != 0) {
        pthread_mutex_destroy(&s->queue);
        free(s);
        return NULL;
    }
    s->fd = fd;
    atomic_init(&s->dropped, 0);
    atomic_init(&s->errors, 0);
    return s;
}

void hz_log_destroy(hz_log_sink *s)
{
    pthread_mutex_destroy(&s->consumer);
    pthread_mutex_destroy(&s->queue);
    free(s);
}

void hz_log_append(hz_log_sink *s, const void *bytes, size_t length)
{
    if (!s || !length) return;
    pthread_mutex_lock(&s->queue);
    if (length <= HZ_LOG_CAPACITY - s->used) {
        size_t tail = (s->head + s->used) % HZ_LOG_CAPACITY;
        size_t first = HZ_LOG_CAPACITY - tail;
        if (first > length) first = length;
        memcpy(s->bytes + tail, bytes, first);
        memcpy(s->bytes, (const unsigned char *)bytes + first, length - first);
        s->used += length;
    } else {
        atomic_fetch_add_explicit(&s->dropped, length, memory_order_relaxed);
    }
    pthread_mutex_unlock(&s->queue);
}

void hz_log_flush(hz_log_sink *s)
{
    if (!s) return;
    pthread_mutex_lock(&s->consumer);
    pthread_mutex_lock(&s->queue);
    size_t head = s->head, remaining = s->used;
    pthread_mutex_unlock(&s->queue);
    /* Keep the snapshot occupied while writing. Producers may fill the free
     * tail, including across wrap, without waiting for the filesystem. */
    size_t saved = 0;
    while (remaining) {
        size_t span = HZ_LOG_CAPACITY - head;
        if (span > remaining) span = remaining;
        ssize_t n = write(s->fd, s->bytes + head, span);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            atomic_fetch_add_explicit(&s->errors, 1, memory_order_relaxed);
            break;
        }
        remaining -= (size_t)n;
        saved += (size_t)n;
        head = (head + (size_t)n) % HZ_LOG_CAPACITY;
    }
    if (saved) {
        pthread_mutex_lock(&s->queue);
        s->head = head;
        s->used -= saved;
        pthread_mutex_unlock(&s->queue);
        s->needs_sync = 1;
    }
    if (s->needs_sync) {
        int rc;
        do { rc = fsync(s->fd); } while (rc < 0 && errno == EINTR);
        if (!rc) s->needs_sync = 0;
        else atomic_fetch_add_explicit(&s->errors, 1, memory_order_relaxed);
    }
    pthread_mutex_unlock(&s->consumer);
}

uint64_t hz_log_dropped(const hz_log_sink *s)
{
    return s ? atomic_load_explicit(&s->dropped, memory_order_relaxed) : 0;
}
uint64_t hz_log_errors(const hz_log_sink *s)
{
    return s ? atomic_load_explicit(&s->errors, memory_order_relaxed) : 0;
}
