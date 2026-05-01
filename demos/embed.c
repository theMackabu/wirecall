#include "wirecall/client.h"
#include "wirecall/server.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct embed_alloc_stats {
  size_t allocs;
  size_t reallocs;
  size_t frees;
} embed_alloc_stats;

typedef struct embed_state {
  const char *name;
  int64_t bias;
  embed_alloc_stats *stats;
} embed_state;

typedef struct embed_server {
  wirecall_server *rpc;
  pthread_t thread;
} embed_server;

static void *embed_alloc(void *ctx, size_t size) {
  embed_alloc_stats *stats = ctx;
  stats->allocs++;
  return malloc(size);
}

static void *embed_realloc(void *ctx, void *ptr, size_t size) {
  embed_alloc_stats *stats = ctx;
  stats->reallocs++;
  return realloc(ptr, size);
}

static void embed_free(void *ctx, void *ptr) {
  embed_alloc_stats *stats = ctx;
  stats->frees++;
  free(ptr);
}

static embed_state *state_new(embed_alloc_stats *stats, const char *name, int64_t bias) {
  embed_state *state = embed_alloc(stats, sizeof(*state));
  if (!state) return NULL;
  *state = (embed_state){
    .name = name,
    .bias = bias,
    .stats = stats,
  };
  return state;
}

static void state_finalizer(void *user_data) {
  embed_state *state = user_data;
  if (!state) return;
  printf("finalize route state: %s\n", state->name);
  embed_free(state->stats, state);
}

static int add_with_bias(wirecall_ctx *ctx, const wirecall_value *args, size_t argc, wirecall_writer *out,
                         void *user_data) {
  (void)ctx;
  embed_state *state = user_data;
  if (!state || argc != 2 || args[0].type != WIRECALL_TYPE_I64 || args[1].type != WIRECALL_TYPE_I64) return -1;
  return wirecall_writer_i64(out, args[0].as.i64 + args[1].as.i64 + state->bias);
}

static void *server_main(void *arg) {
  embed_server *server = arg;
  (void)wirecall_server_run(server->rpc);
  return NULL;
}

int main(void) {
  embed_alloc_stats stats = {0};
  wirecall_allocator allocator = {
    .ctx = &stats,
    .alloc = embed_alloc,
    .realloc = embed_realloc,
    .free = embed_free,
  };

  if (wirecall_set_allocator(&allocator) != 0) {
    fprintf(stderr, "allocator install failed\n");
    return 1;
  }

  embed_server server = {0};
  embed_state *first = state_new(&stats, "add/bias=1", 1);
  embed_state *second = state_new(&stats, "add/bias=100", 100);
  if (!first || !second) {
    fprintf(stderr, "state allocation failed\n");
    state_finalizer(first);
    state_finalizer(second);
    wirecall_set_allocator(NULL);
    return 1;
  }

  if (wirecall_server_init(&server.rpc) != 0 || wirecall_server_set_workers(server.rpc, 1) != 0 ||
      wirecall_server_add_route_name_ex(server.rpc, "add", add_with_bias, first, state_finalizer) != 0 ||
      wirecall_server_add_route_name_ex(server.rpc, "add", add_with_bias, second, state_finalizer) != 0 ||
      wirecall_server_bind(server.rpc, "127.0.0.1", "0") != 0 || wirecall_server_listen(server.rpc) != 0) {
    fprintf(stderr, "server setup failed\n");
    wirecall_server_destroy(server.rpc);
    wirecall_set_allocator(NULL);
    return 1;
  }

  char port[16];
  snprintf(port, sizeof(port), "%u", wirecall_server_port(server.rpc));
  if (pthread_create(&server.thread, NULL, server_main, &server) != 0) {
    fprintf(stderr, "server thread failed\n");
    wirecall_server_destroy(server.rpc);
    wirecall_set_allocator(NULL);
    return 1;
  }

  wirecall_client *client = NULL;
  wirecall_writer args;
  wirecall_writer_init(&args);
  wirecall_value *values = NULL;
  size_t value_count = 0;
  int rc = 1;

  if (wirecall_client_connect(&client, "127.0.0.1", port) != 0) {
    fprintf(stderr, "connect failed\n");
    goto done;
  }
  if (wirecall_writer_i64(&args, 20) != 0 || wirecall_writer_i64(&args, 22) != 0) {
    fprintf(stderr, "argument encode failed\n");
    goto done;
  }
  if (wirecall_client_call_name(client, "add", &args, &values, &value_count) != 0) {
    fprintf(stderr, "call failed: %s\n", wirecall_client_error(client));
    goto done;
  }
  if (value_count != 1 || values[0].type != WIRECALL_TYPE_I64) {
    fprintf(stderr, "unexpected response\n");
    goto done;
  }

  printf("20 + 22 + embedded bias = %lld\n", (long long)values[0].as.i64);
  rc = 0;

done:
  wirecall_values_free(values);
  wirecall_writer_free(&args);
  wirecall_client_close(client);

  (void)wirecall_server_remove_route_name(server.rpc, "add");
  wirecall_server_stop(server.rpc);
  pthread_join(server.thread, NULL);
  wirecall_server_destroy(server.rpc);

  printf("allocator stats: alloc=%zu realloc=%zu free=%zu\n", stats.allocs, stats.reallocs, stats.frees);
  wirecall_set_allocator(NULL);
  return rc;
}
