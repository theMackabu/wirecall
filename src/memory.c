#include "memory.h"
#include "wirecall/protocol.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static wirecall_allocator g_allocator;

static int allocator_valid(const wirecall_allocator *allocator) {
  if (!allocator) { return 0; }
  return allocator->alloc && allocator->realloc && allocator->free;
}

int wirecall_set_allocator(const wirecall_allocator *allocator) {
  if (allocator) {
    if (!allocator_valid(allocator)) { return -1; }
    g_allocator = *allocator;
  } else {
    memset(&g_allocator, 0, sizeof(g_allocator));
  }
  return 0;
}

void wirecall_get_allocator(wirecall_allocator *out_allocator) {
  if (!out_allocator) { return; }
  *out_allocator = g_allocator;
}

void *wirecall_mem_alloc(size_t size) {
  if (size == 0) { size = 1; }
  wirecall_allocator allocator;
  wirecall_get_allocator(&allocator);
  if (allocator.alloc) { return allocator.alloc(allocator.ctx, size); }
  return malloc(size);
}

void *wirecall_mem_calloc(size_t count, size_t size) {
  if (count != 0 && size > SIZE_MAX / count) { return NULL; }
  size_t bytes = count * size;
  void *ptr = wirecall_mem_alloc(bytes);
  if (ptr) { memset(ptr, 0, bytes); }
  return ptr;
}

void *wirecall_mem_realloc(void *ptr, size_t size) {
  if (size == 0) { size = 1; }
  wirecall_allocator allocator;
  wirecall_get_allocator(&allocator);
  if (allocator.realloc) { return allocator.realloc(allocator.ctx, ptr, size); }
  return realloc(ptr, size);
}

void wirecall_mem_free(void *ptr) {
  if (!ptr) { return; }
  wirecall_allocator allocator;
  wirecall_get_allocator(&allocator);
  if (allocator.free) {
    allocator.free(allocator.ctx, ptr);
  } else {
    free(ptr);
  }
}
