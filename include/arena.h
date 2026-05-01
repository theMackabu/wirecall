#ifndef RPC_ARENA_H
#define RPC_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

typedef struct rpc_fixed_arena {
  uint8_t *base;
  size_t elem_size;
  size_t capacity;
  size_t watermark;
  size_t bytes;
  void *free_list;
} rpc_fixed_arena;

static inline size_t rpc_arena_align(size_t n) {
  size_t a = sizeof(void *);
  return (n + a - 1u) & ~(a - 1u);
}

static inline int rpc_fixed_arena_init(rpc_fixed_arena *arena, size_t elem_size, size_t capacity) {
  memset(arena, 0, sizeof(*arena));
  arena->elem_size = rpc_arena_align(elem_size < sizeof(void *) ? sizeof(void *) : elem_size);
  arena->capacity = capacity;
  arena->bytes = arena->elem_size * capacity;
  arena->base = mmap(NULL, arena->bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (arena->base == MAP_FAILED) {
    memset(arena, 0, sizeof(*arena));
    return -1;
  }
  return 0;
}

static inline void rpc_fixed_arena_destroy(rpc_fixed_arena *arena) {
  if (arena->base) { munmap(arena->base, arena->bytes); }
  memset(arena, 0, sizeof(*arena));
}

static inline void *rpc_fixed_arena_alloc(rpc_fixed_arena *arena) {
  void *p = NULL;
  if (arena->free_list) {
    p = arena->free_list;
    arena->free_list = *(void **)p;
  } else if (arena->watermark < arena->capacity) {
    p = arena->base + arena->watermark++ * arena->elem_size;
  }
  if (p) { memset(p, 0, arena->elem_size); }
  return p;
}

static inline void rpc_fixed_arena_free(rpc_fixed_arena *arena, void *p) {
  if (!p) { return; }
  *(void **)p = arena->free_list;
  arena->free_list = p;
}

#endif
