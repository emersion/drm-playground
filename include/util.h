#ifndef DP_UTIL_H
#define DP_UTIL_H

#include <errno.h>
#include <stddef.h>
#include <stdnoreturn.h>
#include <string.h>

noreturn void fatal(const char *fmt, ...);
#define fatal_errno(fmt, ...) fatal(fmt ": %s", ##__VA_ARGS__, strerror(errno))

void *xalloc(size_t size);

#endif
