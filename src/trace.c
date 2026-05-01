#include "wirecall/trace.h"

#include <stdatomic.h>
#include <time.h>

typedef struct wirecall_trace_counter {
  atomic_uint_fast64_t count;
  atomic_uint_fast64_t total_ns;
  atomic_uint_fast64_t max_ns;
} wirecall_trace_counter;

static const char *trace_names[WIRECALL_TRACE_COUNT] = {
  [WIRECALL_TRACE_CLIENT_CALL] = "client.call",
  [WIRECALL_TRACE_CLIENT_SEND] = "client.send",
  [WIRECALL_TRACE_CLIENT_RECV] = "client.recv",
  [WIRECALL_TRACE_CLIENT_DECODE] = "client.decode",
  [WIRECALL_TRACE_SERVER_ACCEPT] = "server.accept",
  [WIRECALL_TRACE_SERVER_POLL_WAIT] = "server.poll_wait",
  [WIRECALL_TRACE_SERVER_POLL_EVENTS] = "server.poll_events",
  [WIRECALL_TRACE_SERVER_LOOP_ACTIVE] = "server.loop_active",
  [WIRECALL_TRACE_SERVER_READ] = "server.read",
  [WIRECALL_TRACE_SERVER_PARSE] = "server.parse",
  [WIRECALL_TRACE_SERVER_ROUTE] = "server.route",
  [WIRECALL_TRACE_SERVER_SCHEDULE] = "server.schedule",
  [WIRECALL_TRACE_SERVER_WRITE] = "server.write",
  [WIRECALL_TRACE_SCHED_SUBMIT] = "sched.submit",
  [WIRECALL_TRACE_SCHED_DECODE] = "sched.decode",
  [WIRECALL_TRACE_SCHED_CORO_CREATE] = "sched.coro_create",
  [WIRECALL_TRACE_SCHED_RESUME] = "sched.resume",
  [WIRECALL_TRACE_PAYLOAD_DECODE] = "payload.decode",
  [WIRECALL_TRACE_ROUTE_LOOKUP] = "route.lookup",
};

static const int trace_is_time[WIRECALL_TRACE_COUNT] = {
  [WIRECALL_TRACE_CLIENT_CALL] = 1,        [WIRECALL_TRACE_CLIENT_SEND] = 1,     [WIRECALL_TRACE_CLIENT_RECV] = 1,
  [WIRECALL_TRACE_CLIENT_DECODE] = 1,      [WIRECALL_TRACE_SERVER_ACCEPT] = 1,   [WIRECALL_TRACE_SERVER_POLL_WAIT] = 1,
  [WIRECALL_TRACE_SERVER_LOOP_ACTIVE] = 1, [WIRECALL_TRACE_SERVER_READ] = 1,     [WIRECALL_TRACE_SERVER_PARSE] = 1,
  [WIRECALL_TRACE_SERVER_ROUTE] = 1,       [WIRECALL_TRACE_SERVER_SCHEDULE] = 1, [WIRECALL_TRACE_SERVER_WRITE] = 1,
  [WIRECALL_TRACE_SCHED_SUBMIT] = 1,       [WIRECALL_TRACE_SCHED_DECODE] = 1,    [WIRECALL_TRACE_SCHED_CORO_CREATE] = 1,
  [WIRECALL_TRACE_SCHED_RESUME] = 1,       [WIRECALL_TRACE_PAYLOAD_DECODE] = 1,  [WIRECALL_TRACE_ROUTE_LOOKUP] = 1,
};

static const char *trace_avg_units[WIRECALL_TRACE_COUNT] = {
  [WIRECALL_TRACE_SERVER_POLL_EVENTS] = "evt",
};

static const char *trace_max_units[WIRECALL_TRACE_COUNT] = {
  [WIRECALL_TRACE_SERVER_POLL_EVENTS] = "events",
};

static wirecall_trace_counter counters[WIRECALL_TRACE_COUNT];
static wirecall_trace_counter worker_counters[WIRECALL_TRACE_MAX_WORKERS][WIRECALL_TRACE_WORKER_COUNT];

int wirecall_trace_enabled = 0;

static void format_duration(uint64_t ns, char out[16]) {
  if (ns < 1000ull) {
    snprintf(out, 16, "%llu ns", (unsigned long long)ns);
  } else if (ns < 1000000ull) {
    snprintf(out, 16, "%.2f us", (double)ns / 1000.0);
  } else if (ns < 1000000000ull) {
    snprintf(out, 16, "%.2f ms", (double)ns / 1000000.0);
  } else {
    snprintf(out, 16, "%.3f s", (double)ns / 1000000000.0);
  }
}

void wirecall_trace_set_enabled(int enabled) {
#if defined(__GNUC__) || defined(__clang__)
  __atomic_store_n(&wirecall_trace_enabled, enabled ? 1 : 0, __ATOMIC_RELAXED);
#else
  wirecall_trace_enabled = enabled ? 1 : 0;
#endif
}

uint64_t wirecall_trace_begin_slow(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void wirecall_trace_end_slow(wirecall_trace_metric metric, uint64_t start_ns) {
  wirecall_trace_add_slow(metric, wirecall_trace_begin_slow() - start_ns);
}

void wirecall_trace_add_slow(wirecall_trace_metric metric, uint64_t value) {
  if ((unsigned)metric >= WIRECALL_TRACE_COUNT) { return; }
  wirecall_trace_counter *counter = &counters[metric];
  atomic_fetch_add_explicit(&counter->count, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&counter->total_ns, value, memory_order_relaxed);

  uint64_t old = atomic_load_explicit(&counter->max_ns, memory_order_relaxed);
  while (old < value && !atomic_compare_exchange_weak_explicit(&counter->max_ns, &old, value, memory_order_relaxed,
                                                               memory_order_relaxed)) {}
}

static void trace_counter_add(wirecall_trace_counter *counter, uint64_t value) {
  atomic_fetch_add_explicit(&counter->count, 1, memory_order_relaxed);
  atomic_fetch_add_explicit(&counter->total_ns, value, memory_order_relaxed);

  uint64_t old = atomic_load_explicit(&counter->max_ns, memory_order_relaxed);
  while (old < value && !atomic_compare_exchange_weak_explicit(&counter->max_ns, &old, value, memory_order_relaxed,
                                                               memory_order_relaxed)) {}
}

void wirecall_trace_worker_end_slow(uint32_t worker, wirecall_trace_worker_metric metric, uint64_t start_ns) {
  wirecall_trace_worker_add_slow(worker, metric, wirecall_trace_begin_slow() - start_ns);
}

void wirecall_trace_worker_add_slow(uint32_t worker, wirecall_trace_worker_metric metric, uint64_t value) {
  if (worker >= WIRECALL_TRACE_MAX_WORKERS || (unsigned)metric >= WIRECALL_TRACE_WORKER_COUNT) { return; }
  trace_counter_add(&worker_counters[worker][metric], value);
}

void wirecall_trace_reset(void) {
  for (size_t i = 0; i < WIRECALL_TRACE_COUNT; ++i) {
    atomic_store_explicit(&counters[i].count, 0, memory_order_relaxed);
    atomic_store_explicit(&counters[i].total_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&counters[i].max_ns, 0, memory_order_relaxed);
  }
  for (size_t worker = 0; worker < WIRECALL_TRACE_MAX_WORKERS; ++worker) {
    for (size_t metric = 0; metric < WIRECALL_TRACE_WORKER_COUNT; ++metric) {
      atomic_store_explicit(&worker_counters[worker][metric].count, 0, memory_order_relaxed);
      atomic_store_explicit(&worker_counters[worker][metric].total_ns, 0, memory_order_relaxed);
      atomic_store_explicit(&worker_counters[worker][metric].max_ns, 0, memory_order_relaxed);
    }
  }
}

void wirecall_trace_snapshot(wirecall_trace_stat out[WIRECALL_TRACE_COUNT]) {
  for (size_t i = 0; i < WIRECALL_TRACE_COUNT; ++i) {
    out[i] = (wirecall_trace_stat){
      .name = trace_names[i],
      .count = atomic_load_explicit(&counters[i].count, memory_order_relaxed),
      .total = atomic_load_explicit(&counters[i].total_ns, memory_order_relaxed),
      .max = atomic_load_explicit(&counters[i].max_ns, memory_order_relaxed),
      .is_time = trace_is_time[i],
    };
  }
}

void wirecall_trace_dump(FILE *out) {
  wirecall_trace_stat stats[WIRECALL_TRACE_COUNT];
  wirecall_trace_snapshot(stats);
  if (!out) { out = stderr; }

  fprintf(out, "\ntrace:\n");
  fprintf(out, "  %-22s %12s %12s %12s\n", "metric", "count", "avg", "max");
  for (size_t i = 0; i < WIRECALL_TRACE_COUNT; ++i) {
    if (stats[i].count == 0) { continue; }
    if (stats[i].is_time) {
      char avg[16];
      char max[16];
      format_duration(stats[i].total / stats[i].count, avg);
      format_duration(stats[i].max, max);
      fprintf(out, "  %-22s %12llu %12s %12s\n", stats[i].name, (unsigned long long)stats[i].count, avg, max);
    } else {
      double avg = (double)stats[i].total / (double)stats[i].count;
      const char *avg_unit = trace_avg_units[i];
      const char *max_unit = trace_max_units[i];
      if (avg_unit && max_unit) {
        char avg_buf[24];
        char max_buf[24];
        snprintf(avg_buf, sizeof(avg_buf), "%.2f %s", avg, avg_unit);
        snprintf(max_buf, sizeof(max_buf), "%llu %s", (unsigned long long)stats[i].max, max_unit);
        fprintf(out, "  %-22s %12llu %12s %12s\n", stats[i].name, (unsigned long long)stats[i].count, avg_buf, max_buf);
      } else {
        fprintf(out, "  %-22s %12llu %12.2f %12llu\n", stats[i].name, (unsigned long long)stats[i].count, avg,
                (unsigned long long)stats[i].max);
      }
    }
  }

  int printed_workers = 0;
  for (size_t worker = 0; worker < WIRECALL_TRACE_MAX_WORKERS; ++worker) {
    wirecall_trace_counter *poll_events = &worker_counters[worker][WIRECALL_TRACE_WORKER_POLL_EVENTS];
    wirecall_trace_counter *accepts = &worker_counters[worker][WIRECALL_TRACE_WORKER_ACCEPTS];
    wirecall_trace_counter *reads = &worker_counters[worker][WIRECALL_TRACE_WORKER_READS];
    wirecall_trace_counter *rpcs = &worker_counters[worker][WIRECALL_TRACE_WORKER_RPCS];
    wirecall_trace_counter *writes = &worker_counters[worker][WIRECALL_TRACE_WORKER_WRITES];
    wirecall_trace_counter *active = &worker_counters[worker][WIRECALL_TRACE_WORKER_ACTIVE];

    uint64_t polls_count = atomic_load_explicit(&poll_events->count, memory_order_relaxed);
    uint64_t polls_total = atomic_load_explicit(&poll_events->total_ns, memory_order_relaxed);
    uint64_t polls_max = atomic_load_explicit(&poll_events->max_ns, memory_order_relaxed);
    uint64_t accept_total = atomic_load_explicit(&accepts->total_ns, memory_order_relaxed);
    uint64_t read_total = atomic_load_explicit(&reads->total_ns, memory_order_relaxed);
    uint64_t wirecall_total = atomic_load_explicit(&rpcs->total_ns, memory_order_relaxed);
    uint64_t write_total = atomic_load_explicit(&writes->total_ns, memory_order_relaxed);
    uint64_t active_count = atomic_load_explicit(&active->count, memory_order_relaxed);
    uint64_t active_total = atomic_load_explicit(&active->total_ns, memory_order_relaxed);
    uint64_t active_max = atomic_load_explicit(&active->max_ns, memory_order_relaxed);

    if (polls_count == 0 && accept_total == 0 && read_total == 0 && wirecall_total == 0 && write_total == 0 &&
        active_count == 0) {
      continue;
    }

    if (!printed_workers) {
      fprintf(out, "\nworker trace:\n");
      fprintf(out, "  %6s %8s %14s %10s %9s %9s %9s %9s %12s %12s\n", "worker", "polls", "evt", "max_ev", "accepts",
              "reads", "rpcs", "writes", "active_avg", "active_max");
      printed_workers = 1;
    }

    char active_avg_buf[16];
    char active_max_buf[16];
    format_duration(active_count ? active_total / active_count : 0, active_avg_buf);
    format_duration(active_max, active_max_buf);
    double events_per_poll = polls_count ? (double)polls_total / (double)polls_count : 0.0;

    fprintf(out, "  %6zu %8llu %14.2f %10llu %9llu %9llu %9llu %9llu %12s %12s\n", worker,
            (unsigned long long)polls_count, events_per_poll, (unsigned long long)polls_max,
            (unsigned long long)accept_total, (unsigned long long)read_total, (unsigned long long)wirecall_total,
            (unsigned long long)write_total, active_avg_buf, active_max_buf);
  }
}
