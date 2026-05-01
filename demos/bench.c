#include "rpc/client.h"
#include "rpc/server.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct bench_server {
  rpc_server *rpc;
  pthread_t thread;
} bench_server;

typedef struct worker_args {
  const char *host;
  char port[16];
  uint64_t requests;
  uint64_t warmup;
  uint64_t latency_offset;
  uint64_t *latencies_ns;
  int failed;
} worker_args;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int add_handler(rpc_ctx *ctx, const rpc_value *args, size_t argc,
                       rpc_writer *out, void *user_data) {
  (void)ctx;
  (void)user_data;
  if (argc != 2 || args[0].type != RPC_TYPE_I64 || args[1].type != RPC_TYPE_I64) {
    return -1;
  }
  return rpc_writer_i64(out, args[0].as.i64 + args[1].as.i64);
}

static void *server_main(void *arg) {
  bench_server *server = arg;
  (void)rpc_server_run(server->rpc);
  return NULL;
}

static int start_server(bench_server *server, char port[16]) {
  memset(server, 0, sizeof(*server));
  if (rpc_server_init(&server->rpc) != 0 ||
      rpc_server_add_route(server->rpc, 1, add_handler, NULL) != 0 ||
      rpc_server_bind(server->rpc, "127.0.0.1", "0") != 0 ||
      rpc_server_listen(server->rpc) != 0) {
    return -1;
  }

  snprintf(port, 16, "%u", rpc_server_port(server->rpc));
  if (pthread_create(&server->thread, NULL, server_main, server) != 0) {
    rpc_server_destroy(server->rpc);
    server->rpc = NULL;
    return -1;
  }
  return 0;
}

static void stop_server(bench_server *server) {
  if (!server->rpc) {
    return;
  }
  rpc_server_stop(server->rpc);
  pthread_join(server->thread, NULL);
  rpc_server_destroy(server->rpc);
  server->rpc = NULL;
}

static int run_one_call(rpc_client *client, rpc_writer *payload, int64_t expected) {
  rpc_value *values = NULL;
  size_t count = 0;
  int rc = rpc_client_call(client, 1, payload, &values, &count);
  if (rc == 0 &&
      (count != 1 || values[0].type != RPC_TYPE_I64 || values[0].as.i64 != expected)) {
    rc = -1;
  }
  rpc_values_free(values);
  return rc;
}

static void *worker_main(void *arg) {
  worker_args *worker = arg;
  rpc_client *client = NULL;
  if (rpc_client_connect(&client, worker->host, worker->port) != 0) {
    worker->failed = 1;
    return NULL;
  }

  rpc_writer payload;
  rpc_writer_init(&payload);
  if (rpc_writer_i64(&payload, 20) != 0 || rpc_writer_i64(&payload, 22) != 0) {
    worker->failed = 1;
    goto done;
  }

  for (uint64_t i = 0; i < worker->warmup; ++i) {
    if (run_one_call(client, &payload, 42) != 0) {
      worker->failed = 1;
      goto done;
    }
  }

  for (uint64_t i = 0; i < worker->requests; ++i) {
    uint64_t start = now_ns();
    if (run_one_call(client, &payload, 42) != 0) {
      worker->failed = 1;
      goto done;
    }
    worker->latencies_ns[worker->latency_offset + i] = now_ns() - start;
  }

done:
  rpc_writer_free(&payload);
  rpc_client_close(client);
  return NULL;
}

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a;
  uint64_t y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

static double ns_to_us(uint64_t ns) { return (double)ns / 1000.0; }

static uint64_t percentile(const uint64_t *values, uint64_t count, double pct) {
  if (count == 0) {
    return 0;
  }
  uint64_t idx = (uint64_t)((pct / 100.0) * (double)(count - 1));
  return values[idx];
}

static uint64_t parse_u64_arg(const char *text, uint64_t fallback) {
  if (!text || !*text) {
    return fallback;
  }
  char *end = NULL;
  errno = 0;
  unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || !end || *end != '\0' || value == 0) {
    return fallback;
  }
  return (uint64_t)value;
}

int main(int argc, char **argv) {
  uint64_t total_requests = argc > 1 ? parse_u64_arg(argv[1], 50000) : 50000;
  uint64_t clients = argc > 2 ? parse_u64_arg(argv[2], 1) : 1;
  uint64_t warmup_total = argc > 3 ? parse_u64_arg(argv[3], 1000) : 1000;
  if (clients > total_requests) {
    clients = total_requests;
  }

  uint64_t *latencies = calloc(total_requests, sizeof(*latencies));
  pthread_t *threads = calloc(clients, sizeof(*threads));
  worker_args *workers = calloc(clients, sizeof(*workers));
  if (!latencies || !threads || !workers) {
    fprintf(stderr, "allocation failed\n");
    free(latencies);
    free(threads);
    free(workers);
    return 1;
  }

  bench_server server;
  char port[16];
  if (start_server(&server, port) != 0) {
    fprintf(stderr, "failed to start benchmark server\n");
    free(latencies);
    free(threads);
    free(workers);
    return 1;
  }

  uint64_t offset = 0;
  uint64_t warmup_base = warmup_total / clients;
  uint64_t warmup_rem = warmup_total % clients;
  uint64_t request_base = total_requests / clients;
  uint64_t request_rem = total_requests % clients;

  uint64_t bench_start = now_ns();
  for (uint64_t i = 0; i < clients; ++i) {
    uint64_t count = request_base + (i < request_rem ? 1u : 0u);
    workers[i] = (worker_args){
        .host = "127.0.0.1",
        .requests = count,
        .warmup = warmup_base + (i < warmup_rem ? 1u : 0u),
        .latency_offset = offset,
        .latencies_ns = latencies,
    };
    snprintf(workers[i].port, sizeof(workers[i].port), "%s", port);
    offset += count;
    if (pthread_create(&threads[i], NULL, worker_main, &workers[i]) != 0) {
      workers[i].failed = 1;
    }
  }

  int failed = 0;
  for (uint64_t i = 0; i < clients; ++i) {
    pthread_join(threads[i], NULL);
    failed |= workers[i].failed;
  }
  uint64_t bench_elapsed = now_ns() - bench_start;
  stop_server(&server);

  if (failed) {
    fprintf(stderr, "benchmark failed\n");
    free(latencies);
    free(threads);
    free(workers);
    return 1;
  }

  qsort(latencies, total_requests, sizeof(*latencies), cmp_u64);
  uint64_t sum = 0;
  for (uint64_t i = 0; i < total_requests; ++i) {
    sum += latencies[i];
  }

  double seconds = (double)bench_elapsed / 1000000000.0;
  double throughput = (double)total_requests / seconds;
  double avg_us = ns_to_us(sum / total_requests);

  printf("requests:        %" PRIu64 "\n", total_requests);
  printf("clients:         %" PRIu64 "\n", clients);
  printf("warmup:          %" PRIu64 "\n", warmup_total);
  printf("elapsed:         %.3f s\n", seconds);
  printf("throughput:      %.0f req/s\n", throughput);
  printf("latency avg:     %.2f us\n", avg_us);
  printf("latency p50:     %.2f us\n", ns_to_us(percentile(latencies, total_requests, 50)));
  printf("latency p95:     %.2f us\n", ns_to_us(percentile(latencies, total_requests, 95)));
  printf("latency p99:     %.2f us\n", ns_to_us(percentile(latencies, total_requests, 99)));
  printf("latency max:     %.2f us\n", ns_to_us(latencies[total_requests - 1]));

  free(latencies);
  free(threads);
  free(workers);
  return 0;
}
