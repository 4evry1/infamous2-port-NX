#include "../horizon/src/hz_log_batch.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned char result[HZ_LOG_CAPACITY * 2];
static size_t written;
static int waiting, resumed, interrupted, budget = -1, sync_error;
static unsigned writes, syncs;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static pthread_t producer;

ssize_t __real_write(int, const void *, size_t);
ssize_t __wrap_write(int fd, const void *data, size_t size)
{
    if (fd != 123) return __real_write(fd, data, size);
    assert(!pthread_equal(producer, pthread_self()));
    ++writes;
    pthread_mutex_lock(&gate);
    waiting = 1;
    pthread_cond_broadcast(&changed);
    while (!resumed) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    if (interrupted) { interrupted = 0; errno = EINTR; return -1; }
    if (budget == 0) { errno = ENOSPC; return -1; }
    if (budget > 0 && size > (size_t)budget) size = (size_t)budget;
    if (budget > 0) budget -= (int)size;
    assert(size <= sizeof result - written);
    memcpy(result + written, data, size);
    written += size;
    return (ssize_t)size;
}
int __wrap_fsync(int fd)
{
    assert(fd == 123 && !pthread_equal(producer, pthread_self()));
    ++syncs;
    return sync_error ? -1 : 0;
}
static void *drain(void *sink) { hz_log_flush(sink); return NULL; }
static void flush(hz_log_sink *sink)
{
    pthread_t writer;
    assert(!pthread_create(&writer, NULL, drain, sink));
    assert(!pthread_join(writer, NULL));
}
int main(void)
{
    alarm(10);
    producer = pthread_self();
    hz_log_sink *sink = hz_log_create(123);
    assert(sink);
    const size_t prefix = HZ_LOG_CAPACITY - 5;
    unsigned char *bytes = malloc(prefix);
    assert(bytes);
    for (size_t i = 0; i < prefix; ++i) bytes[i] = (unsigned char)i;
    for (size_t i = 0; i < prefix; ++i) hz_log_append(sink, bytes+i, 1);
    assert(writes == 0);
    pthread_t writer;
    assert(!pthread_create(&writer, NULL, drain, sink));
    pthread_mutex_lock(&gate);
    while (!waiting) pthread_cond_wait(&changed, &gate);
    pthread_mutex_unlock(&gate);
    hz_log_append(sink, "abcde", 5); /* completes even with the writer blocked */
    hz_log_append(sink, "lost", 4);
    pthread_mutex_lock(&gate);
    resumed = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    assert(!pthread_join(writer, NULL));
    assert(written == prefix && !memcmp(result, bytes, prefix));
    assert(writes == 1 && syncs == 1 && hz_log_dropped(sink) == 4);
    hz_log_append(sink, "123456", 6); /* wraps around to the beginning */
    interrupted = 1; budget = 3;
    flush(sink);
    assert(written == prefix + 3);
    budget = -1; sync_error = 1;
    flush(sink);
    assert(written == prefix + 11 && !memcmp(result+prefix, "abcde123456", 11));
    unsigned last_sync = syncs, last_write = writes;
    sync_error = 0; flush(sink); flush(sink);
    assert(writes == last_write && syncs == last_sync + 1);
    assert(hz_log_errors(sink) == 2);
    free(bytes);
    hz_log_destroy(sink);
}
