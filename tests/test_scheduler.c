#include "scheduler.h"

#include <assert.h>

static int done_count;

static int yielding_handler(wirecall_ctx *ctx, const wirecall_value *args, size_t argc, wirecall_writer *out,
                            void *user_data) {
  int *seen = user_data;
  assert(argc == 1);
  assert(args[0].type == WIRECALL_TYPE_I64);
  *seen += 1;
  wirecall_ctx_yield(ctx);
  *seen += 1;
  return wirecall_writer_i64(out, args[0].as.i64 + 1);
}

static void on_done(wirecall_call *call, void *user_data) {
  (void)user_data;
  done_count++;
  assert(wirecall_call_result(call) == 0);
  const wirecall_writer *w = wirecall_call_response(call);
  wirecall_value *values = NULL;
  size_t count = 0;
  assert(wirecall_payload_decode(w->data, w->len, &values, &count) == 0);
  assert(count == 1);
  assert(values[0].type == WIRECALL_TYPE_I64);
  assert(values[0].as.i64 == 42);
  wirecall_values_free(values);
}

int main(void) {
  wirecall_scheduler *scheduler = NULL;
  int seen = 0;
  wirecall_writer payload;
  wirecall_writer_init(&payload);
  assert(wirecall_writer_i64(&payload, 41) == 0);
  assert(wirecall_scheduler_init(&scheduler) == 0);
  assert(
    wirecall_scheduler_submit(scheduler, 7, 9, yielding_handler, &seen, payload.data, payload.len, on_done, NULL) == 0);
  wirecall_scheduler_run_ready(scheduler);
  assert(seen == 2);
  assert(done_count == 1);
  wirecall_scheduler_destroy(scheduler);
  wirecall_writer_free(&payload);
  return 0;
}
