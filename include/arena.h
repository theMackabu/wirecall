#ifndef WIRECALL_ARENA_H
#define WIRECALL_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

typedef struct wirecall_fixed_arena {
  uint8_t *base;
  size_t elem_size;
  size_t capacity;
  size_t watermark;
  size_t bytes;
  void *free_list;
} wirecall_fixed_arena;

static inline size_t wirecall_arena_align(size_t n) {
  size_t a = sizeof(void *);
  return (n + a - 1u) & ~(a - 1u);
}

static inline int wirecall_fixed_arena_init(wirecall_fixed_arena *arena, size_t elem_size, size_t capacity) {
  memset(arena, 0, sizeof(*arena));
  arena->elem_size = wirecall_arena_align(elem_size < sizeof(void *) ? sizeof(void *) : elem_size);
  arena->capacity = capacity;
  arena->bytes = arena->elem_size * capacity;
  arena->base = mmap(NULL, arena->bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (arena->base == MAP_FAILED) {
    memset(arena, 0, sizeof(*arena));
    return -1;
  }
  return 0;
}

static inline void wirecall_fixed_arena_destroy(wirecall_fixed_arena *arena) {
  if (arena->base) { munmap(arena->base, arena->bytes); }
  memset(arena, 0, sizeof(*arena));
}

static inline void *wirecall_fixed_arena_alloc(wirecall_fixed_arena *arena) {
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

static inline void wirecall_fixed_arena_free(wirecall_fixed_arena *arena, void *p) {
  if (!p) { return; }
  *(void **)p = arena->free_list;
  arena->free_list = p;
}

#endif
