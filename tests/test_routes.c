#include "routes.h"

#include <assert.h>

static int handler_a(wirecall_ctx *ctx, const wirecall_value *args, size_t argc, wirecall_writer *out,
                     void *user_data) {
  (void)ctx;
  (void)args;
  (void)argc;
  (void)out;
  (void)user_data;
  return 0;
}

static int handler_b(wirecall_ctx *ctx, const wirecall_value *args, size_t argc, wirecall_writer *out,
                     void *user_data) {
  (void)ctx;
  (void)args;
  (void)argc;
  (void)out;
  (void)user_data;
  return 0;
}

static void count_finalizer(void *user_data) {
  int *count = user_data;
  (*count)++;
}

int main(void) {
  wirecall_routes routes;
  wirecall_route route;
  int finalized_a = 0;
  int finalized_b = 0;
  assert(wirecall_routes_init(&routes) == 0);
  assert(wirecall_routes_lookup(&routes, 10, &route) != 0);
  assert(wirecall_routes_add_ex(&routes, 10, handler_a, &finalized_a, count_finalizer, 0) == 0);
  assert(wirecall_routes_lookup(&routes, 10, &route) == 0);
  assert(route.handler == handler_a);
  assert(route.user_data == &finalized_a);
  assert(wirecall_routes_add_ex(&routes, 10, handler_b, &finalized_b, count_finalizer, 0) == 0);
  assert(finalized_a == 1);
  assert(wirecall_routes_lookup(&routes, 10, &route) == 0);
  assert(route.handler == handler_b);
  assert(route.user_data == &finalized_b);
  assert(wirecall_routes_remove(&routes, 10) == 0);
  assert(finalized_b == 1);
  assert(wirecall_routes_lookup(&routes, 10, &route) != 0);
  wirecall_routes_destroy(&routes);
  assert(finalized_a == 1);
  assert(finalized_b == 1);
  return 0;
}
