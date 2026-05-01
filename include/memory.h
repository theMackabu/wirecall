#ifndef RPC_MEMORY_H
#define RPC_MEMORY_H

#include <stddef.h>

void *rpc_mem_alloc(size_t size);
void *rpc_mem_calloc(size_t count, size_t size);
void *rpc_mem_realloc(void *ptr, size_t size);
void rpc_mem_free(void *ptr);

#endif
