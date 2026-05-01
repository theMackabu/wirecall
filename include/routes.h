#ifndef WIRECALL_ROUTES_H
#define WIRECALL_ROUTES_H

#include "wirecall/server.h"

#include <pthread.h>
#include <stdatomic.h>

#define WIRECALL_ROUTE_PAGE_BITS 16u
#define WIRECALL_ROUTE_PAGE_SIZE (1u << WIRECALL_ROUTE_PAGE_BITS)
#define WIRECALL_ROUTE_PAGE_MASK (WIRECALL_ROUTE_PAGE_SIZE - 1u)
#define WIRECALL_ROUTE_ROOT_SIZE (1u << (32u - WIRECALL_ROUTE_PAGE_BITS))

typedef struct wirecall_route {
  uint64_t proc_id;
  wirecall_handler_fn handler;
  void *user_data;
  wirecall_route_finalizer_fn finalizer;
  int is_async;
  struct wirecall_route *next;
} wirecall_route;

typedef struct wirecall_retired_route {
  wirecall_route *route;
  uint64_t finalize_proc_id;
  int finalize_all;
  int finalize_one;
  struct wirecall_retired_route *next;
} wirecall_retired_route;

typedef _Atomic(wirecall_route *) wirecall_route_slot;

typedef struct wirecall_routes {
  _Atomic(wirecall_route_slot *) *pages;
  size_t root_bytes;
  size_t page_bytes;
  atomic_uint active_readers;
  pthread_mutex_t mutate_lock;
  wirecall_retired_route *retired;
} wirecall_routes;

int wirecall_routes_init(wirecall_routes *routes);
void wirecall_routes_destroy(wirecall_routes *routes);
int wirecall_routes_add(wirecall_routes *routes, uint64_t proc_id, wirecall_handler_fn handler, void *user_data);
int wirecall_routes_add_ex(wirecall_routes *routes, uint64_t proc_id, wirecall_handler_fn handler, void *user_data,
                           wirecall_route_finalizer_fn finalizer, int is_async);
int wirecall_routes_remove(wirecall_routes *routes, uint64_t proc_id);
int wirecall_routes_lookup(wirecall_routes *routes, uint64_t proc_id, wirecall_route *out);

#endif
