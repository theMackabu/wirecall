#include "routes.h"

#include <assert.h>

static int handler_a(rpc_ctx *ctx, const rpc_value *args, size_t argc, rpc_writer *out, void *user_data) {
  (void)ctx;
  (void)args;
  (void)argc;
  (void)out;
  (void)user_data;
  return 0;
}

static int handler_b(rpc_ctx *ctx, const rpc_value *args, size_t argc, rpc_writer *out, void *user_data) {
  (void)ctx;
  (void)args;
  (void)argc;
  (void)out;
  (void)user_data;
  return 0;
}

int main(void) {
  rpc_routes routes;
  rpc_route route;
  assert(rpc_routes_init(&routes) == 0);
  assert(rpc_routes_lookup(&routes, 10, &route) != 0);
  assert(rpc_routes_add(&routes, 10, handler_a, (void *)1) == 0);
  assert(rpc_routes_lookup(&routes, 10, &route) == 0);
  assert(route.handler == handler_a);
  assert(route.user_data == (void *)1);
  assert(rpc_routes_add(&routes, 10, handler_b, (void *)2) == 0);
  assert(rpc_routes_lookup(&routes, 10, &route) == 0);
  assert(route.handler == handler_b);
  assert(route.user_data == (void *)2);
  assert(rpc_routes_remove(&routes, 10) == 0);
  assert(rpc_routes_lookup(&routes, 10, &route) != 0);
  rpc_routes_destroy(&routes);
  return 0;
}
