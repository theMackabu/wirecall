#include "routes.h"
#include "rpc/trace.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void *route_mmap(size_t bytes) {
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}

static rpc_route_slot *route_page_alloc(const rpc_routes *routes) {
  return route_mmap(routes->page_bytes);
}

static void route_chain_free(rpc_route *route) {
  while (route) {
    rpc_route *next = route->next;
    free(route);
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

static void free_retired(rpc_routes *routes) {
  if (atomic_load_explicit(&routes->active_readers, memory_order_acquire) != 0) return;

  rpc_retired_route *node = routes->retired;
  routes->retired = NULL;

  while (node) {
    rpc_retired_route *next = node->next;
    route_chain_free(node->route);
    free(node);
    node = next;
  }
}

static int retire_route(rpc_routes *routes, rpc_route *route) {
  if (!route) return 0;

  rpc_retired_route *node = calloc(1, sizeof(*node));
  if (!node) return -1;

  node->route = route;
  node->next = routes->retired;
  routes->retired = node;
  free_retired(routes);

  return 0;
}

int rpc_routes_init(rpc_routes *routes) {
  if (!routes) return -1;

  memset(routes, 0, sizeof(*routes));
  routes->root_bytes = RPC_ROUTE_ROOT_SIZE * sizeof(*routes->pages);
  routes->page_bytes = RPC_ROUTE_PAGE_SIZE * sizeof(rpc_route_slot);
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

void rpc_routes_destroy(rpc_routes *routes) {
  if (!routes) return;

  pthread_mutex_lock(&routes->mutate_lock);
  if (!routes->pages) goto retired;

  for (size_t page_idx = 0; page_idx < RPC_ROUTE_ROOT_SIZE; ++page_idx) {
    rpc_route_slot *page = atomic_load_explicit(&routes->pages[page_idx], memory_order_relaxed);
    if (!page) continue;

    for (size_t i = 0; i < RPC_ROUTE_PAGE_SIZE; ++i) {
      rpc_route *route = atomic_load_explicit(&page[i], memory_order_relaxed);
      route_chain_free(route);
    }
    munmap(page, routes->page_bytes);
  }
  munmap(routes->pages, routes->root_bytes);

retired:
  rpc_retired_route *node = routes->retired;

  while (node) {
    rpc_retired_route *next = node->next;
    route_chain_free(node->route);
    free(node);
    node = next;
  }

  routes->retired = NULL;
  pthread_mutex_unlock(&routes->mutate_lock);
  pthread_mutex_destroy(&routes->mutate_lock);
}

int rpc_routes_add(rpc_routes *routes, uint64_t proc_id, rpc_handler_fn handler, void *user_data) {
  return rpc_routes_add_ex(routes, proc_id, handler, user_data, 0);
}

int rpc_routes_add_ex(rpc_routes *routes, uint64_t proc_id, rpc_handler_fn handler, void *user_data, int is_async) {
  if (!routes || !handler) return -1;

  rpc_route *route = calloc(1, sizeof(*route));
  if (!route) return -1;

  *route = (rpc_route){.proc_id = proc_id, .handler = handler, .user_data = user_data, .is_async = is_async ? 1 : 0};

  pthread_mutex_lock(&routes->mutate_lock);
  uint32_t index = route_index(proc_id);
  uint32_t page_idx = index >> RPC_ROUTE_PAGE_BITS;
  uint32_t slot_idx = index & RPC_ROUTE_PAGE_MASK;
  rpc_route_slot *page = atomic_load_explicit(&routes->pages[page_idx], memory_order_acquire);
  if (!page) {
    page = route_page_alloc(routes);
    if (!page) {
      pthread_mutex_unlock(&routes->mutate_lock);
      free(route);
      return -1;
    }
    atomic_store_explicit(&routes->pages[page_idx], page, memory_order_release);
  }
  rpc_route *old = atomic_load_explicit(&page[slot_idx], memory_order_acquire);
  rpc_route *copy_head = route;
  rpc_route **copy_tail = &route->next;
  for (rpc_route *it = old; it; it = it->next) {
    if (it->proc_id == proc_id) continue;
    rpc_route *copy = calloc(1, sizeof(*copy));
    if (!copy) {
      route_chain_free(copy_head);
      pthread_mutex_unlock(&routes->mutate_lock);
      return -1;
    }
    *copy = *it;
    copy->next = NULL;
    *copy_tail = copy;
    copy_tail = &copy->next;
  }
  atomic_store_explicit(&page[slot_idx], copy_head, memory_order_release);

  int rc = retire_route(routes, old);
  pthread_mutex_unlock(&routes->mutate_lock);

  return rc;
}

int rpc_routes_remove(rpc_routes *routes, uint64_t proc_id) {
  if (!routes) return -1;

  pthread_mutex_lock(&routes->mutate_lock);
  uint32_t index = route_index(proc_id);
  uint32_t page_idx = index >> RPC_ROUTE_PAGE_BITS;
  uint32_t slot_idx = index & RPC_ROUTE_PAGE_MASK;
  rpc_route_slot *page = atomic_load_explicit(&routes->pages[page_idx], memory_order_acquire);
  rpc_route *old = page ? atomic_load_explicit(&page[slot_idx], memory_order_acquire) : NULL;
  rpc_route *copy_head = NULL;
  rpc_route **copy_tail = &copy_head;
  int removed = 0;
  for (rpc_route *it = old; it; it = it->next) {
    if (it->proc_id == proc_id) {
      removed = 1;
      continue;
    }
    rpc_route *copy = calloc(1, sizeof(*copy));
    if (!copy) {
      route_chain_free(copy_head);
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
    route_chain_free(copy_head);
  }

  int rc = removed ? retire_route(routes, old) : -1;
  pthread_mutex_unlock(&routes->mutate_lock);

  return rc;
}

int rpc_routes_lookup(rpc_routes *routes, uint64_t proc_id, rpc_route *out) {
  uint64_t trace = rpc_trace_begin();
  if (!routes || !out) {
    rpc_trace_end(RPC_TRACE_ROUTE_LOOKUP, trace);
    return -1;
  }

  atomic_fetch_add_explicit(&routes->active_readers, 1u, memory_order_acquire);
  uint32_t index = route_index(proc_id);
  uint32_t page_idx = index >> RPC_ROUTE_PAGE_BITS;
  uint32_t slot_idx = index & RPC_ROUTE_PAGE_MASK;
  rpc_route_slot *page = atomic_load_explicit(&routes->pages[page_idx], memory_order_acquire);
  rpc_route *route = page ? atomic_load_explicit(&page[slot_idx], memory_order_acquire) : NULL;
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
  rpc_trace_end(RPC_TRACE_ROUTE_LOOKUP, trace);
  return route ? 0 : -1;
}
