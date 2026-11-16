/* Horizon layer: <execinfo.h>. backtrace walks frame pointers; symbols are resolved offline. */
#ifndef HZ_EXECINFO_H
#define HZ_EXECINFO_H
#ifdef __cplusplus
extern "C" {
#endif
int backtrace(void **buffer, int size);
char **backtrace_symbols(void *const *buffer, int size);
void backtrace_symbols_fd(void *const *buffer, int size, int fd);
#ifdef __cplusplus
}
#endif
#endif
