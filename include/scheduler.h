#ifndef WIRECALL_SCHEDULER_H
#define WIRECALL_SCHEDULER_H

#include "wirecall/server.h"

#include <stdint.h>

typedef struct wirecall_scheduler wirecall_scheduler;
typedef struct wirecall_call wirecall_call;
typedef void (*wirecall_call_done_fn)(wirecall_call *call, void *user_data);

struct wirecall_ctx {
  uint64_t call_id;
  uint64_t proc_id;
  void *server;
};

int wirecall_scheduler_init(wirecall_scheduler **out);
void wirecall_scheduler_destroy(wirecall_scheduler *scheduler);
int wirecall_scheduler_submit(wirecall_scheduler *scheduler, uint64_t call_id, uint64_t proc_id,
                              wirecall_handler_fn handler, void *handler_data, const uint8_t *payload,
                              size_t payload_len, wirecall_call_done_fn done, void *done_data);
void wirecall_scheduler_run_ready(wirecall_scheduler *scheduler);

int wirecall_call_result(const wirecall_call *call);
const wirecall_writer *wirecall_call_response(const wirecall_call *call);
const char *wirecall_call_error(const wirecall_call *call);
uint64_t wirecall_call_id(const wirecall_call *call);
uint64_t wirecall_call_proc_id(const wirecall_call *call);

#endif
