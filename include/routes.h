#ifndef RPC_ROUTES_H
#define RPC_ROUTES_H

#include "rpc/server.h"

#include <pthread.h>
#include <stdatomic.h>

#define RPC_ROUTE_PAGE_BITS 16u
#define RPC_ROUTE_PAGE_SIZE (1u << RPC_ROUTE_PAGE_BITS)
#define RPC_ROUTE_PAGE_MASK (RPC_ROUTE_PAGE_SIZE - 1u)
#define RPC_ROUTE_ROOT_SIZE (1u << (32u - RPC_ROUTE_PAGE_BITS))

typedef struct rpc_route {
  uint64_t proc_id;
  rpc_handler_fn handler;
  void *user_data;
  rpc_route_finalizer_fn finalizer;
  int is_async;
  struct rpc_route *next;
} rpc_route;

typedef struct rpc_retired_route {
  rpc_route *route;
  uint64_t finalize_proc_id;
  int finalize_all;
  int finalize_one;
  struct rpc_retired_route *next;
} rpc_retired_route;

typedef _Atomic(rpc_route *) rpc_route_slot;

typedef struct rpc_routes {
  _Atomic(rpc_route_slot *) *pages;
  size_t root_bytes;
  size_t page_bytes;
  atomic_uint active_readers;
  pthread_mutex_t mutate_lock;
  rpc_retired_route *retired;
} rpc_routes;

int rpc_routes_init(rpc_routes *routes);
void rpc_routes_destroy(rpc_routes *routes);
int rpc_routes_add(rpc_routes *routes, uint64_t proc_id, rpc_handler_fn handler, void *user_data);
int rpc_routes_add_ex(rpc_routes *routes, uint64_t proc_id, rpc_handler_fn handler, void *user_data,
                      rpc_route_finalizer_fn finalizer, int is_async);
int rpc_routes_remove(rpc_routes *routes, uint64_t proc_id);
int rpc_routes_lookup(rpc_routes *routes, uint64_t proc_id, rpc_route *out);

#endif
