#include "memory.h"
#include "rpc/protocol.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static rpc_allocator g_allocator;

static int allocator_valid(const rpc_allocator *allocator) {
  if (!allocator) { return 0; }
  return allocator->alloc && allocator->realloc && allocator->free;
}

int rpc_set_allocator(const rpc_allocator *allocator) {
  if (allocator) {
    if (!allocator_valid(allocator)) {
      return -1;
    }
    g_allocator = *allocator;
  } else {
    memset(&g_allocator, 0, sizeof(g_allocator));
  }
  return 0;
}

void rpc_get_allocator(rpc_allocator *out_allocator) {
  if (!out_allocator) { return; }
  *out_allocator = g_allocator;
}

void *rpc_mem_alloc(size_t size) {
  if (size == 0) { size = 1; }
  rpc_allocator allocator;
  rpc_get_allocator(&allocator);
  if (allocator.alloc) { return allocator.alloc(allocator.ctx, size); }
  return malloc(size);
}

void *rpc_mem_calloc(size_t count, size_t size) {
  if (count != 0 && size > SIZE_MAX / count) { return NULL; }
  size_t bytes = count * size;
  void *ptr = rpc_mem_alloc(bytes);
  if (ptr) { memset(ptr, 0, bytes); }
  return ptr;
}

void *rpc_mem_realloc(void *ptr, size_t size) {
  if (size == 0) { size = 1; }
  rpc_allocator allocator;
  rpc_get_allocator(&allocator);
  if (allocator.realloc) { return allocator.realloc(allocator.ctx, ptr, size); }
  return realloc(ptr, size);
}

void rpc_mem_free(void *ptr) {
  if (!ptr) { return; }
  rpc_allocator allocator;
  rpc_get_allocator(&allocator);
  if (allocator.free) {
    allocator.free(allocator.ctx, ptr);
  } else {
    free(ptr);
  }
}
