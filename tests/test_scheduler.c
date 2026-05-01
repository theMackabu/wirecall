#include "scheduler.h"

#include <assert.h>

static int done_count;

static int yielding_handler(rpc_ctx *ctx, const rpc_value *args, size_t argc,
                            rpc_writer *out, void *user_data) {
  int *seen = user_data;
  assert(argc == 1);
  assert(args[0].type == RPC_TYPE_I64);
  *seen += 1;
  rpc_ctx_yield(ctx);
  *seen += 1;
  return rpc_writer_i64(out, args[0].as.i64 + 1);
}

static void on_done(rpc_call *call, void *user_data) {
  (void)user_data;
  done_count++;
  assert(rpc_call_result(call) == 0);
  const rpc_writer *w = rpc_call_response(call);
  rpc_value *values = NULL;
  size_t count = 0;
  assert(rpc_payload_decode(w->data, w->len, &values, &count) == 0);
  assert(count == 1);
  assert(values[0].type == RPC_TYPE_I64);
  assert(values[0].as.i64 == 42);
  rpc_values_free(values);
}

int main(void) {
  rpc_scheduler *scheduler = NULL;
  int seen = 0;
  rpc_writer payload;
  rpc_writer_init(&payload);
  assert(rpc_writer_i64(&payload, 41) == 0);
  assert(rpc_scheduler_init(&scheduler) == 0);
  assert(rpc_scheduler_submit(scheduler, 7, 9, yielding_handler, &seen,
                              payload.data, payload.len, on_done, NULL) == 0);
  rpc_scheduler_run_ready(scheduler);
  assert(seen == 2);
  assert(done_count == 1);
  rpc_scheduler_destroy(scheduler);
  rpc_writer_free(&payload);
  return 0;
}
