#ifndef WIRECALL_MEMORY_H
#define WIRECALL_MEMORY_H

#include <stddef.h>

void *wirecall_mem_alloc(size_t size);
void *wirecall_mem_calloc(size_t count, size_t size);
void *wirecall_mem_realloc(void *ptr, size_t size);
void wirecall_mem_free(void *ptr);

#endif
