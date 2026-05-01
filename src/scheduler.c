#include "scheduler.h"

#include "arena.h"

#define MCO_USE_VMEM_ALLOCATOR
#define MCO_ZERO_MEMORY
#define MCO_DEFAULT_STACK_SIZE (1024 * 1024)
#define MINICORO_IMPL
#include "minicoro.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RPC_CALL_ARENA_CAPACITY 262144u

struct rpc_scheduler {
  rpc_call *head;
  rpc_call *tail;
  rpc_fixed_arena call_arena;
};

struct rpc_call {
  mco_coro *co;
  rpc_ctx ctx;
  rpc_handler_fn handler;
  void *handler_data;
  uint8_t *payload;
  size_t payload_len;
  rpc_value *args;
  size_t argc;
  rpc_writer response;
  int result;
  char error[160];
  int completed;
  rpc_call_done_fn done;
  void *done_data;
  rpc_call *next_ready;
};

static void call_free(rpc_scheduler *scheduler, rpc_call *call) {
  if (!call) {
    return;
  }
  if (call->co) {
    (void)mco_destroy(call->co);
  }
  rpc_values_free(call->args);
  rpc_writer_free(&call->response);
  free(call->payload);
  rpc_fixed_arena_free(&scheduler->call_arena, call);
}

static int enqueue(rpc_scheduler *scheduler, rpc_call *call) {
  call->next_ready = NULL;
  if (scheduler->tail) {
    scheduler->tail->next_ready = call;
  } else {
    scheduler->head = call;
  }
  scheduler->tail = call;
  return 0;
}

static rpc_call *dequeue(rpc_scheduler *scheduler) {
  rpc_call *call = scheduler->head;
  if (!call) {
    return NULL;
  }
  scheduler->head = call->next_ready;
  if (!scheduler->head) {
    scheduler->tail = NULL;
  }
  call->next_ready = NULL;
  return call;
}

static void call_entry(mco_coro *co) {
  rpc_call *call = mco_get_user_data(co);
  call->result = call->handler(&call->ctx, call->args, call->argc,
                               &call->response, call->handler_data);
  call->completed = 1;
  if (call->result != 0 && call->error[0] == '\0') {
    snprintf(call->error, sizeof(call->error), "procedure returned %d",
             call->result);
  }
}

int rpc_scheduler_init(rpc_scheduler **out) {
  if (!out) {
    return -1;
  }
  rpc_scheduler *scheduler = calloc(1, sizeof(*scheduler));
  if (!scheduler) {
    return -1;
  }
  if (rpc_fixed_arena_init(&scheduler->call_arena, sizeof(rpc_call),
                           RPC_CALL_ARENA_CAPACITY) != 0) {
    free(scheduler);
    return -1;
  }
  *out = scheduler;
  return 0;
}

void rpc_scheduler_destroy(rpc_scheduler *scheduler) {
  if (!scheduler) {
    return;
  }
  rpc_call *call = NULL;
  while ((call = dequeue(scheduler)) != NULL) {
    call_free(scheduler, call);
  }
  rpc_fixed_arena_destroy(&scheduler->call_arena);
  free(scheduler);
}

int rpc_scheduler_submit(rpc_scheduler *scheduler, uint64_t call_id,
                         uint32_t proc_id, rpc_handler_fn handler,
                         void *handler_data, const uint8_t *payload,
                         size_t payload_len, rpc_call_done_fn done,
                         void *done_data) {
  if (!scheduler || !handler || (!payload && payload_len > 0)) {
    return -1;
  }

  rpc_call *call = rpc_fixed_arena_alloc(&scheduler->call_arena);
  if (!call) {
    return -1;
  }
  call->ctx.call_id = call_id;
  call->ctx.proc_id = proc_id;
  call->handler = handler;
  call->handler_data = handler_data;
  call->payload_len = payload_len;
  call->done = done;
  call->done_data = done_data;
  rpc_writer_init(&call->response);

  if (payload_len > 0) {
    call->payload = malloc(payload_len);
    if (!call->payload) {
      call_free(scheduler, call);
      return -1;
    }
    memcpy(call->payload, payload, payload_len);
  }

  if (rpc_payload_decode(call->payload, call->payload_len, &call->args,
                         &call->argc) != 0) {
    snprintf(call->error, sizeof(call->error), "malformed payload");
    call->result = -1;
    call->completed = 1;
    done(call, done_data);
    call_free(scheduler, call);
    return 0;
  }

  mco_desc desc = mco_desc_init(call_entry, 0);
  desc.user_data = call;
  mco_result rc = mco_create(&call->co, &desc);
  if (rc != MCO_SUCCESS) {
    snprintf(call->error, sizeof(call->error), "coroutine create failed: %s",
             mco_result_description(rc));
    call->result = -1;
    call->completed = 1;
    done(call, done_data);
    call_free(scheduler, call);
    return 0;
  }

  if (enqueue(scheduler, call) != 0) {
    call_free(scheduler, call);
    return -1;
  }
  return 0;
}

void rpc_scheduler_run_ready(rpc_scheduler *scheduler) {
  if (!scheduler) {
    return;
  }

  rpc_call *call = NULL;
  while ((call = dequeue(scheduler)) != NULL) {
    mco_result rc = mco_resume(call->co);
    if (rc != MCO_SUCCESS) {
      snprintf(call->error, sizeof(call->error), "coroutine resume failed: %s",
               mco_result_description(rc));
      call->result = -1;
      call->completed = 1;
    }

    if (!call->completed && mco_status(call->co) == MCO_SUSPENDED) {
      if (enqueue(scheduler, call) == 0) {
        continue;
      }
      snprintf(call->error, sizeof(call->error), "scheduler enqueue failed");
      call->result = -1;
      call->completed = 1;
    }

    if (call->done) {
      call->done(call, call->done_data);
    }
    call_free(scheduler, call);
  }
}

int rpc_call_result(const rpc_call *call) { return call ? call->result : -1; }

const rpc_writer *rpc_call_response(const rpc_call *call) {
  return call ? &call->response : NULL;
}

const char *rpc_call_error(const rpc_call *call) {
  return call && call->error[0] ? call->error : "procedure failed";
}

uint64_t rpc_call_id(const rpc_call *call) {
  return call ? call->ctx.call_id : 0;
}

uint32_t rpc_call_proc_id(const rpc_call *call) {
  return call ? call->ctx.proc_id : 0;
}

uint64_t rpc_ctx_call_id(const rpc_ctx *ctx) { return ctx ? ctx->call_id : 0; }

uint32_t rpc_ctx_proc_id(const rpc_ctx *ctx) { return ctx ? ctx->proc_id : 0; }

void rpc_ctx_yield(rpc_ctx *ctx) {
  (void)ctx;
  mco_coro *co = mco_running();
  if (co) {
    (void)mco_yield(co);
  }
}
