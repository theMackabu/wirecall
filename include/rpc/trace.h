#ifndef RPC_TRACE_H
#define RPC_TRACE_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum rpc_trace_metric {
  RPC_TRACE_CLIENT_CALL,
  RPC_TRACE_CLIENT_SEND,
  RPC_TRACE_CLIENT_RECV,
  RPC_TRACE_CLIENT_DECODE,
  RPC_TRACE_SERVER_ACCEPT,
  RPC_TRACE_SERVER_POLL_WAIT,
  RPC_TRACE_SERVER_POLL_EVENTS,
  RPC_TRACE_SERVER_LOOP_ACTIVE,
  RPC_TRACE_SERVER_READ,
  RPC_TRACE_SERVER_PARSE,
  RPC_TRACE_SERVER_ROUTE,
  RPC_TRACE_SERVER_SCHEDULE,
  RPC_TRACE_SERVER_WRITE,
  RPC_TRACE_SCHED_SUBMIT,
  RPC_TRACE_SCHED_DECODE,
  RPC_TRACE_SCHED_CORO_CREATE,
  RPC_TRACE_SCHED_RESUME,
  RPC_TRACE_PAYLOAD_DECODE,
  RPC_TRACE_ROUTE_LOOKUP,
  RPC_TRACE_COUNT,
} rpc_trace_metric;

typedef struct rpc_trace_stat {
  const char *name;
  uint64_t count;
  uint64_t total;
  uint64_t max;
  int is_time;
} rpc_trace_stat;

extern int rpc_trace_enabled;

void rpc_trace_set_enabled(int enabled);
uint64_t rpc_trace_begin_slow(void);
void rpc_trace_end_slow(rpc_trace_metric metric, uint64_t start_ns);
void rpc_trace_add_slow(rpc_trace_metric metric, uint64_t value);
void rpc_trace_reset(void);
void rpc_trace_snapshot(rpc_trace_stat out[RPC_TRACE_COUNT]);
void rpc_trace_dump(FILE *out);

#if defined(__GNUC__) || defined(__clang__)
#define RPC_TRACE_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define RPC_TRACE_UNLIKELY(x) (x)
#endif

static inline int rpc_trace_is_enabled(void) {
#if defined(__GNUC__) || defined(__clang__)
  return RPC_TRACE_UNLIKELY(
      __atomic_load_n(&rpc_trace_enabled, __ATOMIC_RELAXED));
#else
  return RPC_TRACE_UNLIKELY(rpc_trace_enabled);
#endif
}

static inline uint64_t rpc_trace_begin(void) {
  return rpc_trace_is_enabled() ? rpc_trace_begin_slow() : 0;
}

static inline void rpc_trace_end(rpc_trace_metric metric, uint64_t start_ns) {
  if (rpc_trace_is_enabled() && start_ns != 0) {
    rpc_trace_end_slow(metric, start_ns);
  }
}

static inline void rpc_trace_add(rpc_trace_metric metric, uint64_t value) {
  if (rpc_trace_is_enabled()) {
    rpc_trace_add_slow(metric, value);
  }
}

#ifdef __cplusplus
}
#endif

#endif
