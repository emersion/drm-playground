#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdnoreturn.h>

noreturn void fatal(const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);

	fprintf(stderr, "\n");

	exit(1);
}

void *xalloc(size_t size) {
	void *ptr = calloc(1, size);
	if (!ptr) {
		fatal_errno("Allocation of size %zu failed", size);
	}

	return ptr;
}
