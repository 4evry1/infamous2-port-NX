#pragma once
#include <stddef.h>
#include <stdint.h>
enum { HZ_LOG_CAPACITY = 128 * 1024 };
typedef struct hz_log_sink hz_log_sink;
/* The caller owns the descriptor for the lifetime of the sink. */
hz_log_sink *hz_log_create(int fd);
void hz_log_destroy(hz_log_sink *sink);
void hz_log_append(hz_log_sink *sink, const void *bytes, size_t length);
void hz_log_flush(hz_log_sink *sink);
uint64_t hz_log_dropped(const hz_log_sink *sink);
uint64_t hz_log_errors(const hz_log_sink *sink);
