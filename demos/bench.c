#include "rpc/client.h"
#include "rpc/server.h"
#include "rpc/trace.h"

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
  uint32_t workers;
} bench_server;

typedef struct bench_gate {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  uint64_t ready;
  uint64_t total;
  int start;
  int abort;
} bench_gate;

typedef struct worker_args {
  const char *host;
  char port[16];
  uint64_t requests;
  uint64_t warmup;
  uint64_t pipeline;
  uint64_t latency_offset;
  uint64_t *latencies_ns;
  bench_gate *gate;
  int started;
  int failed;
} worker_args;

static int gate_ready_and_wait(bench_gate *gate);
static int gate_wait_ready(bench_gate *gate, uint64_t total);
static void gate_start(bench_gate *gate);
static void gate_abort(bench_gate *gate);

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
  if (rpc_server_init(&server->rpc) != 0 ||
      (server->workers != 0 &&
       rpc_server_set_workers(server->rpc, server->workers) != 0) ||
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

static int validate_response(rpc_value *values, size_t count, int64_t expected) {
  return count == 1 && values[0].type == RPC_TYPE_I64 && values[0].as.i64 == expected;
}

static void *worker_main(void *arg) {
  worker_args *worker = arg;
  int announced_gate = 0;
  rpc_client *client = NULL;
  if (rpc_client_connect(&client, worker->host, worker->port) != 0) {
    worker->failed = 1;
    (void)gate_ready_and_wait(worker->gate);
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

  announced_gate = 1;
  if (gate_ready_and_wait(worker->gate) != 0) {
    worker->failed = 1;
    goto done;
  }

  if (worker->pipeline <= 1) {
    for (uint64_t i = 0; i < worker->requests; ++i) {
      uint64_t start = now_ns();
      if (run_one_call(client, &payload, 42) != 0) {
        worker->failed = 1;
        goto done;
      }
      worker->latencies_ns[worker->latency_offset + i] = now_ns() - start;
    }
    goto done;
  }

  uint64_t *call_ids = calloc(worker->pipeline, sizeof(*call_ids));
  uint64_t *starts = calloc(worker->pipeline, sizeof(*starts));
  if (!call_ids || !starts) {
    free(call_ids);
    free(starts);
    worker->failed = 1;
    goto done;
  }

  uint64_t sent = 0;
  uint64_t received = 0;
  while (sent < worker->requests && sent - received < worker->pipeline) {
    uint64_t start = now_ns();
    uint64_t call_id = 0;
    if (rpc_client_send_call(client, 1, &payload, &call_id) != 0) {
      worker->failed = 1;
      goto pipeline_done;
    }
    uint64_t slot = call_id % worker->pipeline;
    call_ids[slot] = call_id;
    starts[slot] = start;
    sent++;
  }

  while (received < worker->requests) {
    uint64_t call_id = 0;
    rpc_value *values = NULL;
    size_t count = 0;
    if (rpc_client_recv_response(client, &call_id, &values, &count) != 0 ||
        !validate_response(values, count, 42)) {
      rpc_values_free(values);
      worker->failed = 1;
      goto pipeline_done;
    }

    uint64_t slot = call_id % worker->pipeline;
    if (call_ids[slot] != call_id) {
      rpc_values_free(values);
      worker->failed = 1;
      goto pipeline_done;
    }
    worker->latencies_ns[worker->latency_offset + received] =
        now_ns() - starts[slot];
    rpc_values_free(values);
    received++;

    if (sent < worker->requests) {
      uint64_t start = now_ns();
      uint64_t next_call_id = 0;
      if (rpc_client_send_call(client, 1, &payload, &next_call_id) != 0) {
        worker->failed = 1;
        goto pipeline_done;
      }
      slot = next_call_id % worker->pipeline;
      call_ids[slot] = next_call_id;
      starts[slot] = start;
      sent++;
    }
  }

pipeline_done:
  free(call_ids);
  free(starts);

done:
  if (!announced_gate) {
    (void)gate_ready_and_wait(worker->gate);
  }
  rpc_writer_free(&payload);
  rpc_client_close(client);
  return NULL;
}

static int cmp_u64(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a;
  uint64_t y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}

static void format_duration(uint64_t ns, char out[16]) {
  if (ns < 1000ull) {
    snprintf(out, 16, "%" PRIu64 " ns", ns);
  } else if (ns < 1000000ull) {
    snprintf(out, 16, "%.2f us", (double)ns / 1000.0);
  } else if (ns < 1000000000ull) {
    snprintf(out, 16, "%.2f ms", (double)ns / 1000000.0);
  } else {
    snprintf(out, 16, "%.3f s", (double)ns / 1000000000.0);
  }
}

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

static void deadline_after_ms(struct timespec *deadline, long ms) {
  clock_gettime(CLOCK_REALTIME, deadline);
  deadline->tv_sec += ms / 1000;
  deadline->tv_nsec += (ms % 1000) * 1000000l;
  if (deadline->tv_nsec >= 1000000000l) {
    deadline->tv_sec++;
    deadline->tv_nsec -= 1000000000l;
  }
}

static int gate_ready_and_wait(bench_gate *gate) {
  pthread_mutex_lock(&gate->mutex);
  gate->ready++;
  pthread_cond_broadcast(&gate->cond);
  while (!gate->start && !gate->abort) {
    pthread_cond_wait(&gate->cond, &gate->mutex);
  }
  int ok = !gate->abort;
  pthread_mutex_unlock(&gate->mutex);
  return ok ? 0 : -1;
}

static int gate_wait_ready(bench_gate *gate, uint64_t total) {
  pthread_mutex_lock(&gate->mutex);
  gate->total = total;
  while (gate->ready < gate->total && !gate->abort) {
    struct timespec deadline;
    deadline_after_ms(&deadline, 15000);
    int rc = pthread_cond_timedwait(&gate->cond, &gate->mutex, &deadline);
    if (rc == ETIMEDOUT) {
      gate->abort = 1;
      pthread_cond_broadcast(&gate->cond);
      pthread_mutex_unlock(&gate->mutex);
      return -1;
    }
  }
  int ok = !gate->abort;
  pthread_mutex_unlock(&gate->mutex);
  return ok ? 0 : -1;
}

static void gate_start(bench_gate *gate) {
  pthread_mutex_lock(&gate->mutex);
  gate->start = 1;
  pthread_cond_broadcast(&gate->cond);
  pthread_mutex_unlock(&gate->mutex);
}

static void gate_abort(bench_gate *gate) {
  pthread_mutex_lock(&gate->mutex);
  gate->abort = 1;
  pthread_cond_broadcast(&gate->cond);
  pthread_mutex_unlock(&gate->mutex);
}

int main(int argc, char **argv) {
  uint64_t total_requests = argc > 1 ? parse_u64_arg(argv[1], 50000) : 50000;
  uint64_t clients = argc > 2 ? parse_u64_arg(argv[2], 1) : 1;
  uint64_t warmup_total = argc > 3 ? parse_u64_arg(argv[3], 1000) : 1000;
  uint64_t server_workers = argc > 4 ? parse_u64_arg(argv[4], 0) : 0;
  uint64_t pipeline = argc > 5 ? parse_u64_arg(argv[5], 1) : 1;
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
  memset(&server, 0, sizeof(server));
  server.workers = (uint32_t)server_workers;
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
  bench_gate gate = {0};
  pthread_mutex_init(&gate.mutex, NULL);
  pthread_cond_init(&gate.cond, NULL);

  uint64_t started = 0;
  for (uint64_t i = 0; i < clients; ++i) {
    uint64_t count = request_base + (i < request_rem ? 1u : 0u);
    workers[i] = (worker_args){
        .host = "127.0.0.1",
        .requests = count,
        .warmup = warmup_base + (i < warmup_rem ? 1u : 0u),
        .pipeline = pipeline,
        .latency_offset = offset,
        .latencies_ns = latencies,
        .gate = &gate,
    };
    snprintf(workers[i].port, sizeof(workers[i].port), "%s", port);
    offset += count;
    if (pthread_create(&threads[i], NULL, worker_main, &workers[i]) != 0) {
      workers[i].failed = 1;
    } else {
      workers[i].started = 1;
      started++;
    }
  }

  int failed = 0;
  int bench_started = 0;
  uint64_t bench_start = 0;
  if (gate_wait_ready(&gate, started) != 0) {
    fprintf(stderr, "benchmark workers did not become ready\n");
    failed = 1;
    stop_server(&server);
  } else {
    for (uint64_t i = 0; i < clients; ++i) {
      failed |= workers[i].failed;
    }
    if (failed) {
      gate_abort(&gate);
      stop_server(&server);
    } else {
      rpc_trace_reset();
      rpc_trace_set_enabled(1);
      bench_start = now_ns();
      bench_started = 1;
      gate_start(&gate);
    }
  }

  for (uint64_t i = 0; i < clients; ++i) {
    if (workers[i].started) {
      pthread_join(threads[i], NULL);
    }
    failed |= workers[i].failed;
  }
  rpc_trace_set_enabled(0);
  uint64_t bench_elapsed = bench_started ? now_ns() - bench_start : 0;
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
  char elapsed_buf[16];
  char avg_buf[16];
  char p50_buf[16];
  char p95_buf[16];
  char p99_buf[16];
  char max_buf[16];
  format_duration(bench_elapsed, elapsed_buf);
  format_duration(sum / total_requests, avg_buf);
  format_duration(percentile(latencies, total_requests, 50), p50_buf);
  format_duration(percentile(latencies, total_requests, 95), p95_buf);
  format_duration(percentile(latencies, total_requests, 99), p99_buf);
  format_duration(latencies[total_requests - 1], max_buf);

  printf("requests:        %" PRIu64 "\n", total_requests);
  printf("clients:         %" PRIu64 "\n", clients);
  printf("warmup:          %" PRIu64 "\n", warmup_total);
  printf("server workers:  %" PRIu64 "%s\n", server_workers,
         server_workers == 0 ? " (auto)" : "");
  printf("pipeline depth:  %" PRIu64 "\n", pipeline);
  printf("elapsed:         %s\n", elapsed_buf);
  printf("throughput:      %.0f req/s\n", throughput);
  printf("latency avg:     %s\n", avg_buf);
  printf("latency p50:     %s\n", p50_buf);
  printf("latency p95:     %s\n", p95_buf);
  printf("latency p99:     %s\n", p99_buf);
  printf("latency max:     %s\n", max_buf);
  rpc_trace_dump(stdout);

  pthread_cond_destroy(&gate.cond);
  pthread_mutex_destroy(&gate.mutex);
  free(latencies);
  free(threads);
  free(workers);
  return 0;
}
