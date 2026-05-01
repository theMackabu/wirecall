#ifndef WIRECALL_TRACE_H
#define WIRECALL_TRACE_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum wirecall_trace_metric {
  WIRECALL_TRACE_CLIENT_CALL,
  WIRECALL_TRACE_CLIENT_SEND,
  WIRECALL_TRACE_CLIENT_RECV,
  WIRECALL_TRACE_CLIENT_DECODE,
  WIRECALL_TRACE_SERVER_ACCEPT,
  WIRECALL_TRACE_SERVER_POLL_WAIT,
  WIRECALL_TRACE_SERVER_POLL_EVENTS,
  WIRECALL_TRACE_SERVER_LOOP_ACTIVE,
  WIRECALL_TRACE_SERVER_READ,
  WIRECALL_TRACE_SERVER_PARSE,
  WIRECALL_TRACE_SERVER_ROUTE,
  WIRECALL_TRACE_SERVER_SCHEDULE,
  WIRECALL_TRACE_SERVER_WRITE,
  WIRECALL_TRACE_SCHED_SUBMIT,
  WIRECALL_TRACE_SCHED_DECODE,
  WIRECALL_TRACE_SCHED_CORO_CREATE,
  WIRECALL_TRACE_SCHED_RESUME,
  WIRECALL_TRACE_PAYLOAD_DECODE,
  WIRECALL_TRACE_ROUTE_LOOKUP,
  WIRECALL_TRACE_COUNT,
} wirecall_trace_metric;

typedef enum wirecall_trace_worker_metric {
  WIRECALL_TRACE_WORKER_POLL_EVENTS,
  WIRECALL_TRACE_WORKER_ACCEPTS,
  WIRECALL_TRACE_WORKER_READS,
  WIRECALL_TRACE_WORKER_RPCS,
  WIRECALL_TRACE_WORKER_WRITES,
  WIRECALL_TRACE_WORKER_ACTIVE,
  WIRECALL_TRACE_WORKER_COUNT,
} wirecall_trace_worker_metric;

#define WIRECALL_TRACE_MAX_WORKERS 128u

typedef struct wirecall_trace_stat {
  const char *name;
  uint64_t count;
  uint64_t total;
  uint64_t max;
  int is_time;
} wirecall_trace_stat;

extern int wirecall_trace_enabled;

void wirecall_trace_set_enabled(int enabled);
uint64_t wirecall_trace_begin_slow(void);
void wirecall_trace_end_slow(wirecall_trace_metric metric, uint64_t start_ns);
void wirecall_trace_add_slow(wirecall_trace_metric metric, uint64_t value);
void wirecall_trace_worker_end_slow(uint32_t worker, wirecall_trace_worker_metric metric, uint64_t start_ns);
void wirecall_trace_worker_add_slow(uint32_t worker, wirecall_trace_worker_metric metric, uint64_t value);
void wirecall_trace_reset(void);
void wirecall_trace_snapshot(wirecall_trace_stat out[WIRECALL_TRACE_COUNT]);
void wirecall_trace_dump(FILE *out);

#if defined(__GNUC__) || defined(__clang__)
#define WIRECALL_TRACE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define WIRECALL_TRACE_UNLIKELY(x) (x)
#endif

static inline int wirecall_trace_is_enabled(void) {
#if defined(__GNUC__) || defined(__clang__)
  return WIRECALL_TRACE_UNLIKELY(__atomic_load_n(&wirecall_trace_enabled, __ATOMIC_RELAXED));
#else
  return WIRECALL_TRACE_UNLIKELY(wirecall_trace_enabled);
#endif
}

static inline uint64_t wirecall_trace_begin(void) {
  return wirecall_trace_is_enabled() ? wirecall_trace_begin_slow() : 0;
}

static inline void wirecall_trace_end(wirecall_trace_metric metric, uint64_t start_ns) {
  if (wirecall_trace_is_enabled() && start_ns != 0) { wirecall_trace_end_slow(metric, start_ns); }
}

static inline void wirecall_trace_add(wirecall_trace_metric metric, uint64_t value) {
  if (wirecall_trace_is_enabled()) { wirecall_trace_add_slow(metric, value); }
}

static inline void wirecall_trace_worker_end(uint32_t worker, wirecall_trace_worker_metric metric, uint64_t start_ns) {
  if (wirecall_trace_is_enabled() && start_ns != 0) { wirecall_trace_worker_end_slow(worker, metric, start_ns); }
}

static inline void wirecall_trace_worker_add(uint32_t worker, wirecall_trace_worker_metric metric, uint64_t value) {
  if (wirecall_trace_is_enabled()) { wirecall_trace_worker_add_slow(worker, metric, value); }
}

#ifdef __cplusplus
}
#endif

#endif
