#ifndef RPC_SCHEDULER_H
#define RPC_SCHEDULER_H

#include "rpc/server.h"

#include <stdint.h>

typedef struct rpc_scheduler rpc_scheduler;
typedef struct rpc_call rpc_call;
typedef void (*rpc_call_done_fn)(rpc_call *call, void *user_data);

struct rpc_ctx {
  uint64_t call_id;
  uint64_t proc_id;
  void *server;
};

int rpc_scheduler_init(rpc_scheduler **out);
void rpc_scheduler_destroy(rpc_scheduler *scheduler);
int rpc_scheduler_submit(rpc_scheduler *scheduler, uint64_t call_id, uint64_t proc_id, rpc_handler_fn handler,
                         void *handler_data, const uint8_t *payload, size_t payload_len, rpc_call_done_fn done,
                         void *done_data);
void rpc_scheduler_run_ready(rpc_scheduler *scheduler);

int rpc_call_result(const rpc_call *call);
const rpc_writer *rpc_call_response(const rpc_call *call);
const char *rpc_call_error(const rpc_call *call);
uint64_t rpc_call_id(const rpc_call *call);
uint64_t rpc_call_proc_id(const rpc_call *call);

#endif
