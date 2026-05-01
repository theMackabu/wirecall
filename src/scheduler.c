#include "scheduler.h"

#include "arena.h"
#include "memory.h"
#include "wirecall/trace.h"

#define MCO_USE_VMEM_ALLOCATOR
#define MCO_ZERO_MEMORY
#define MCO_DEFAULT_STACK_SIZE (1024 * 1024)
#define MCO_ALLOC(size) wirecall_mem_calloc(1, size)
#define MCO_DEALLOC(ptr, size) wirecall_mem_free(ptr)
#define MINICORO_IMPL
#include "minicoro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIRECALL_CALL_ARENA_CAPACITY 262144u

struct wirecall_scheduler {
  wirecall_call *head;
  wirecall_call *tail;
  wirecall_fixed_arena call_arena;
};

struct wirecall_call {
  mco_coro *co;
  wirecall_ctx ctx;
  wirecall_handler_fn handler;
  void *handler_data;
  uint8_t *payload;
  size_t payload_len;
  wirecall_value *args;
  size_t argc;
  wirecall_writer response;
  int result;
  char error[160];
  int completed;
  wirecall_call_done_fn done;
  void *done_data;
  wirecall_call *next_ready;
};

static void call_free(wirecall_scheduler *scheduler, wirecall_call *call) {
  if (!call) { return; }
  if (call->co) { (void)mco_destroy(call->co); }
  wirecall_values_free(call->args);
  wirecall_writer_free(&call->response);
  wirecall_mem_free(call->payload);
  wirecall_fixed_arena_free(&scheduler->call_arena, call);
}

static int enqueue(wirecall_scheduler *scheduler, wirecall_call *call) {
  call->next_ready = NULL;
  if (scheduler->tail) {
    scheduler->tail->next_ready = call;
  } else {
    scheduler->head = call;
  }
  scheduler->tail = call;
  return 0;
}

static wirecall_call *dequeue(wirecall_scheduler *scheduler) {
  wirecall_call *call = scheduler->head;
  if (!call) { return NULL; }
  scheduler->head = call->next_ready;
  if (!scheduler->head) { scheduler->tail = NULL; }
  call->next_ready = NULL;
  return call;
}

static void call_entry(mco_coro *co) {
  wirecall_call *call = mco_get_user_data(co);
  call->result = call->handler(&call->ctx, call->args, call->argc, &call->response, call->handler_data);
  call->completed = 1;
  if (call->result != 0 && call->error[0] == '\0') {
    snprintf(call->error, sizeof(call->error), "procedure returned %d", call->result);
  }
}

static void call_finish(wirecall_scheduler *scheduler, wirecall_call *call) {
  if (call->done) { call->done(call, call->done_data); }
  call_free(scheduler, call);
}

static int call_resume(wirecall_call *call) {
  uint64_t trace_resume = wirecall_trace_begin();
  mco_result rc = mco_resume(call->co);
  wirecall_trace_end(WIRECALL_TRACE_SCHED_RESUME, trace_resume);
  if (rc != MCO_SUCCESS) {
    snprintf(call->error, sizeof(call->error), "coroutine resume failed: %s", mco_result_description(rc));
    call->result = -1;
    call->completed = 1;
    return -1;
  }
  return 0;
}

int wirecall_scheduler_init(wirecall_scheduler **out) {
  if (!out) { return -1; }
  wirecall_scheduler *scheduler = wirecall_mem_calloc(1, sizeof(*scheduler));
  if (!scheduler) { return -1; }
  if (wirecall_fixed_arena_init(&scheduler->call_arena, sizeof(wirecall_call), WIRECALL_CALL_ARENA_CAPACITY) != 0) {
    wirecall_mem_free(scheduler);
    return -1;
  }
  *out = scheduler;
  return 0;
}

void wirecall_scheduler_destroy(wirecall_scheduler *scheduler) {
  if (!scheduler) { return; }
  wirecall_call *call = NULL;
  while ((call = dequeue(scheduler)) != NULL) {
    call_free(scheduler, call);
  }
  wirecall_fixed_arena_destroy(&scheduler->call_arena);
  wirecall_mem_free(scheduler);
}

int wirecall_scheduler_submit(wirecall_scheduler *scheduler, uint64_t call_id, uint64_t proc_id,
                              wirecall_handler_fn handler, void *handler_data, const uint8_t *payload,
                              size_t payload_len, wirecall_call_done_fn done, void *done_data) {
  uint64_t trace_submit = wirecall_trace_begin();
  if (!scheduler || !handler || (!payload && payload_len > 0)) {
    wirecall_trace_end(WIRECALL_TRACE_SCHED_SUBMIT, trace_submit);
    return -1;
  }

  wirecall_call *call = wirecall_fixed_arena_alloc(&scheduler->call_arena);
  if (!call) {
    wirecall_trace_end(WIRECALL_TRACE_SCHED_SUBMIT, trace_submit);
    return -1;
  }
  call->ctx.call_id = call_id;
  call->ctx.proc_id = proc_id;
  call->handler = handler;
  call->handler_data = handler_data;
  call->payload_len = payload_len;
  call->done = done;
  call->done_data = done_data;
  wirecall_writer_init(&call->response);

  if (payload_len > 0) {
    call->payload = wirecall_mem_alloc(payload_len);
    if (!call->payload) {
      call_free(scheduler, call);
      wirecall_trace_end(WIRECALL_TRACE_SCHED_SUBMIT, trace_submit);
      return -1;
    }
    memcpy(call->payload, payload, payload_len);
  }

  uint64_t trace_decode = wirecall_trace_begin();
  if (wirecall_payload_decode(call->payload, call->payload_len, &call->args, &call->argc) != 0) {
    wirecall_trace_end(WIRECALL_TRACE_SCHED_DECODE, trace_decode);
    snprintf(call->error, sizeof(call->error), "malformed payload");
    call->result = -1;
    call->completed = 1;
    done(call, done_data);
    call_free(scheduler, call);
    wirecall_trace_end(WIRECALL_TRACE_SCHED_SUBMIT, trace_submit);
    return 0;
  }
  wirecall_trace_end(WIRECALL_TRACE_SCHED_DECODE, trace_decode);

  mco_desc desc = mco_desc_init(call_entry, 0);
  desc.user_data = call;
  uint64_t trace_create = wirecall_trace_begin();
  mco_result rc = mco_create(&call->co, &desc);
  wirecall_trace_end(WIRECALL_TRACE_SCHED_CORO_CREATE, trace_create);
  if (rc != MCO_SUCCESS) {
    snprintf(call->error, sizeof(call->error), "coroutine create failed: %s", mco_result_description(rc));
    call->result = -1;
    call->completed = 1;
    done(call, done_data);
    call_free(scheduler, call);
    wirecall_trace_end(WIRECALL_TRACE_SCHED_SUBMIT, trace_submit);
    return 0;
  }

  if (enqueue(scheduler, call) != 0) {
    call_free(scheduler, call);
    wirecall_trace_end(WIRECALL_TRACE_SCHED_SUBMIT, trace_submit);
    return -1;
  }
  wirecall_trace_end(WIRECALL_TRACE_SCHED_SUBMIT, trace_submit);
  return 0;
}

void wirecall_scheduler_run_ready(wirecall_scheduler *scheduler) {
  if (!scheduler) { return; }

  wirecall_call *call = NULL;
  while ((call = dequeue(scheduler)) != NULL) {
    (void)call_resume(call);

    if (!call->completed && mco_status(call->co) == MCO_SUSPENDED) {
      if (enqueue(scheduler, call) == 0) { continue; }
      snprintf(call->error, sizeof(call->error), "scheduler enqueue failed");
      call->result = -1;
      call->completed = 1;
    }

    call_finish(scheduler, call);
  }
}

int wirecall_call_result(const wirecall_call *call) {
  return call ? call->result : -1;
}

const wirecall_writer *wirecall_call_response(const wirecall_call *call) {
  return call ? &call->response : NULL;
}

const char *wirecall_call_error(const wirecall_call *call) {
  return call && call->error[0] ? call->error : "procedure failed";
}

uint64_t wirecall_call_id(const wirecall_call *call) {
  return call ? call->ctx.call_id : 0;
}

uint64_t wirecall_call_proc_id(const wirecall_call *call) {
  return call ? call->ctx.proc_id : 0;
}

uint64_t wirecall_ctx_call_id(const wirecall_ctx *ctx) {
  return ctx ? ctx->call_id : 0;
}

uint64_t wirecall_ctx_proc_id(const wirecall_ctx *ctx) {
  return ctx ? ctx->proc_id : 0;
}

void wirecall_ctx_yield(wirecall_ctx *ctx) {
  (void)ctx;
  mco_coro *co = mco_running();
  if (co) { (void)mco_yield(co); }
}
