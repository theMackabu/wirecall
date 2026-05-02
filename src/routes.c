#include "routes.h"
#include "wirecall/trace.h"

#include "memory.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void *route_mmap(size_t bytes) {
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}

static wirecall_route_slot *route_page_alloc(const wirecall_routes *routes) {
  return route_mmap(routes->page_bytes);
}

static void route_finalize(const wirecall_route *route) {
  if (route && route->finalizer) { route->finalizer(route->user_data); }
}

static void route_chain_free(wirecall_route *route, int finalize_all, int finalize_one, uint64_t finalize_proc_id) {
  while (route) {
    wirecall_route *next = route->next;
    if (finalize_all || (finalize_one && route->proc_id == finalize_proc_id)) { route_finalize(route); }
    wirecall_mem_free(route);
    route = next;
  }
}

static uint32_t route_index(uint64_t proc_id) {
  proc_id ^= proc_id >> 33u;
  proc_id *= 0xff51afd7ed558ccdull;
  proc_id ^= proc_id >> 33u;
  proc_id *= 0xc4ceb9fe1a85ec53ull;
  proc_id ^= proc_id >> 33u;
  return (uint32_t)proc_id;
}

static void free_retired(wirecall_routes *routes) {
  if (atomic_load_explicit(&routes->active_readers, memory_order_acquire) != 0) return;

  wirecall_retired_route *node = routes->retired;
  routes->retired = NULL;

  while (node) {
    wirecall_retired_route *next = node->next;
    route_chain_free(node->route, node->finalize_all, node->finalize_one, node->finalize_proc_id);
    wirecall_mem_free(node);
    node = next;
  }
}

static int retire_route(wirecall_routes *routes, wirecall_route *route, int finalize_one, uint64_t finalize_proc_id) {
  if (!route) return 0;
  if (atomic_load_explicit(&routes->active_readers, memory_order_acquire) == 0) {
    route_chain_free(route, 0, finalize_one, finalize_proc_id);
    return 0;
  }

  wirecall_retired_route *node = wirecall_mem_calloc(1, sizeof(*node));
  if (!node) return -1;

  node->route = route;
  node->finalize_proc_id = finalize_proc_id;
  node->finalize_one = finalize_one;
  node->next = routes->retired;
  routes->retired = node;
  free_retired(routes);

  return 0;
}

int wirecall_routes_init(wirecall_routes *routes) {
  if (!routes) return -1;

  memset(routes, 0, sizeof(*routes));
  routes->root_bytes = WIRECALL_ROUTE_ROOT_SIZE * sizeof(*routes->pages);
  routes->page_bytes = WIRECALL_ROUTE_PAGE_SIZE * sizeof(wirecall_route_slot);
  routes->pages = route_mmap(routes->root_bytes);

  if (!routes->pages) return -1;

  if (pthread_mutex_init(&routes->mutate_lock, NULL) != 0) {
    munmap(routes->pages, routes->root_bytes);
    memset(routes, 0, sizeof(*routes));
    return -1;
  }

  atomic_init(&routes->active_readers, 0);

  return 0;
}

void wirecall_routes_destroy(wirecall_routes *routes) {
  if (!routes) return;

  pthread_mutex_lock(&routes->mutate_lock);
  if (!routes->pages) goto retired;

  for (size_t page_idx = 0; page_idx < WIRECALL_ROUTE_ROOT_SIZE; ++page_idx) {
    wirecall_route_slot *page = atomic_load_explicit(&routes->pages[page_idx], memory_order_relaxed);
    if (!page) continue;

    for (size_t i = 0; i < WIRECALL_ROUTE_PAGE_SIZE; ++i) {
      wirecall_route *route = atomic_load_explicit(&page[i], memory_order_relaxed);
      route_chain_free(route, 1, 0, 0);
    }
    munmap(page, routes->page_bytes);
  }
  munmap(routes->pages, routes->root_bytes);

retired:
  wirecall_retired_route *node = routes->retired;

  while (node) {
    wirecall_retired_route *next = node->next;
    route_chain_free(node->route, node->finalize_all, node->finalize_one, node->finalize_proc_id);
    wirecall_mem_free(node);
    node = next;
  }

  routes->retired = NULL;
  pthread_mutex_unlock(&routes->mutate_lock);
  pthread_mutex_destroy(&routes->mutate_lock);
}

int wirecall_routes_add(wirecall_routes *routes, uint64_t proc_id, wirecall_handler_fn handler, void *user_data) {
  return wirecall_routes_add_ex(routes, proc_id, handler, user_data, NULL, 0);
}

static int routes_add_route(wirecall_routes *routes, wirecall_route *route) {
  if (!routes || !route) return -1;

  uint64_t proc_id = route->proc_id;
  pthread_mutex_lock(&routes->mutate_lock);
  uint32_t index = route_index(proc_id);
  uint32_t page_idx = index >> WIRECALL_ROUTE_PAGE_BITS;
  uint32_t slot_idx = index & WIRECALL_ROUTE_PAGE_MASK;
  wirecall_route_slot *page = atomic_load_explicit(&routes->pages[page_idx], memory_order_acquire);
  if (!page) {
    page = route_page_alloc(routes);
    if (!page) {
      pthread_mutex_unlock(&routes->mutate_lock);
      wirecall_mem_free(route);
      return -1;
    }
    atomic_store_explicit(&routes->pages[page_idx], page, memory_order_release);
  }
  wirecall_route *old = atomic_load_explicit(&page[slot_idx], memory_order_acquire);
  wirecall_route *copy_head = route;
  wirecall_route **copy_tail = &route->next;
  int replaced = 0;
  for (wirecall_route *it = old; it; it = it->next) {
    if (it->proc_id == proc_id) {
      replaced = 1;
      continue;
    }
    wirecall_route *copy = wirecall_mem_calloc(1, sizeof(*copy));
    if (!copy) {
      route_chain_free(copy_head, 0, 0, 0);
      pthread_mutex_unlock(&routes->mutate_lock);
      return -1;
    }
    *copy = *it;
    copy->next = NULL;
    *copy_tail = copy;
    copy_tail = &copy->next;
  }
  atomic_store_explicit(&page[slot_idx], copy_head, memory_order_release);

  int rc = retire_route(routes, old, replaced, proc_id);
  pthread_mutex_unlock(&routes->mutate_lock);

  return rc;
}

int wirecall_routes_add_ex(wirecall_routes *routes, uint64_t proc_id, wirecall_handler_fn handler, void *user_data,
                           wirecall_route_finalizer_fn finalizer, int is_async) {
  if (!routes || !handler) return -1;

  wirecall_route *route = wirecall_mem_calloc(1, sizeof(*route));
  if (!route) return -1;

  *route = (wirecall_route){.proc_id = proc_id,
                            .handler = handler,
                            .user_data = user_data,
                            .finalizer = finalizer,
                            .is_async = is_async ? 1 : 0};

  return routes_add_route(routes, route);
}

int wirecall_routes_add_deferred_ex(wirecall_routes *routes, uint64_t proc_id, wirecall_deferred_handler_fn handler,
                                    void *user_data, wirecall_route_finalizer_fn finalizer) {
  if (!routes || !handler) return -1;

  wirecall_route *route = wirecall_mem_calloc(1, sizeof(*route));
  if (!route) return -1;

  *route = (wirecall_route){
    .proc_id = proc_id, .deferred_handler = handler, .user_data = user_data, .finalizer = finalizer, .is_deferred = 1};

  return routes_add_route(routes, route);
}

int wirecall_routes_remove(wirecall_routes *routes, uint64_t proc_id) {
  if (!routes) return -1;

  pthread_mutex_lock(&routes->mutate_lock);
  uint32_t index = route_index(proc_id);
  uint32_t page_idx = index >> WIRECALL_ROUTE_PAGE_BITS;
  uint32_t slot_idx = index & WIRECALL_ROUTE_PAGE_MASK;
  wirecall_route_slot *page = atomic_load_explicit(&routes->pages[page_idx], memory_order_acquire);
  wirecall_route *old = page ? atomic_load_explicit(&page[slot_idx], memory_order_acquire) : NULL;
  wirecall_route *copy_head = NULL;
  wirecall_route **copy_tail = &copy_head;
  int removed = 0;
  for (wirecall_route *it = old; it; it = it->next) {
    if (it->proc_id == proc_id) {
      removed = 1;
      continue;
    }
    wirecall_route *copy = wirecall_mem_calloc(1, sizeof(*copy));
    if (!copy) {
      route_chain_free(copy_head, 0, 0, 0);
      pthread_mutex_unlock(&routes->mutate_lock);
      return -1;
    }
    *copy = *it;
    copy->next = NULL;
    *copy_tail = copy;
    copy_tail = &copy->next;
  }
  if (page && removed) {
    atomic_store_explicit(&page[slot_idx], copy_head, memory_order_release);
  } else {
    route_chain_free(copy_head, 0, 0, 0);
  }

  int rc = removed ? retire_route(routes, old, 1, proc_id) : -1;
  pthread_mutex_unlock(&routes->mutate_lock);

  return rc;
}

int wirecall_routes_lookup(wirecall_routes *routes, uint64_t proc_id, wirecall_route *out) {
  uint64_t trace = wirecall_trace_begin();
  if (!routes || !out) {
    wirecall_trace_end(WIRECALL_TRACE_ROUTE_LOOKUP, trace);
    return -1;
  }

  atomic_fetch_add_explicit(&routes->active_readers, 1u, memory_order_acquire);
  uint32_t index = route_index(proc_id);
  uint32_t page_idx = index >> WIRECALL_ROUTE_PAGE_BITS;
  uint32_t slot_idx = index & WIRECALL_ROUTE_PAGE_MASK;
  wirecall_route_slot *page = atomic_load_explicit(&routes->pages[page_idx], memory_order_acquire);
  wirecall_route *route = page ? atomic_load_explicit(&page[slot_idx], memory_order_acquire) : NULL;
  while (route && route->proc_id != proc_id) {
    route = route->next;
  }
  if (route) { *out = *route; }
  unsigned old = atomic_fetch_sub_explicit(&routes->active_readers, 1u, memory_order_release);
  if (old == 1u) {
    pthread_mutex_lock(&routes->mutate_lock);
    free_retired(routes);
    pthread_mutex_unlock(&routes->mutate_lock);
  }
  wirecall_trace_end(WIRECALL_TRACE_ROUTE_LOOKUP, trace);
  return route ? 0 : -1;
}
