#ifndef RPC_SERVER_H
#define RPC_SERVER_H

#include "rpc/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rpc_ctx rpc_ctx;
typedef struct rpc_server rpc_server;

typedef int (*rpc_handler_fn)(rpc_ctx *ctx, const rpc_value *args, size_t argc, rpc_writer *out, void *user_data);
typedef void (*rpc_route_finalizer_fn)(void *user_data);

uint64_t rpc_ctx_call_id(const rpc_ctx *ctx);
uint64_t rpc_ctx_proc_id(const rpc_ctx *ctx);
void rpc_ctx_yield(rpc_ctx *ctx);

int rpc_server_init(rpc_server **out_server);
int rpc_server_set_workers(rpc_server *server, uint32_t worker_count);
int rpc_server_bind(rpc_server *server, const char *host, const char *port);
uint16_t rpc_server_port(const rpc_server *server);
int rpc_server_listen(rpc_server *server);
int rpc_server_run(rpc_server *server);
void rpc_server_stop(rpc_server *server);
void rpc_server_destroy(rpc_server *server);

int rpc_server_add_route_name(rpc_server *server, const char *proc_name, rpc_handler_fn handler, void *user_data);
int rpc_server_add_async_route_name(rpc_server *server, const char *proc_name, rpc_handler_fn handler,
                                    void *user_data);
int rpc_server_add_route_name_ex(rpc_server *server, const char *proc_name, rpc_handler_fn handler, void *user_data,
                                 rpc_route_finalizer_fn finalizer);
int rpc_server_add_async_route_name_ex(rpc_server *server, const char *proc_name, rpc_handler_fn handler,
                                       void *user_data, rpc_route_finalizer_fn finalizer);
int rpc_server_remove_route_name(rpc_server *server, const char *proc_name);

#ifdef __cplusplus
}
#endif

#endif
