#ifndef WIRECALL_SERVER_H
#define WIRECALL_SERVER_H

#include "wirecall/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wirecall_ctx wirecall_ctx;
typedef struct wirecall_server wirecall_server;

typedef int (*wirecall_handler_fn)(wirecall_ctx *ctx, const wirecall_value *args, size_t argc, wirecall_writer *out,
                                   void *user_data);
typedef void (*wirecall_route_finalizer_fn)(void *user_data);

uint64_t wirecall_ctx_call_id(const wirecall_ctx *ctx);
uint64_t wirecall_ctx_proc_id(const wirecall_ctx *ctx);
void wirecall_ctx_yield(wirecall_ctx *ctx);

int wirecall_server_init(wirecall_server **out_server);
int wirecall_server_set_workers(wirecall_server *server, uint32_t worker_count);
int wirecall_server_set_integrity(wirecall_server *server, uint32_t integrity, const uint8_t mac_key[16]);
int wirecall_server_bind(wirecall_server *server, const char *host, const char *port);
uint16_t wirecall_server_port(const wirecall_server *server);
int wirecall_server_listen(wirecall_server *server);
int wirecall_server_run(wirecall_server *server);
void wirecall_server_stop(wirecall_server *server);
void wirecall_server_destroy(wirecall_server *server);

int wirecall_server_add_route_name(wirecall_server *server, const char *proc_name, wirecall_handler_fn handler,
                                   void *user_data);
int wirecall_server_add_async_route_name(wirecall_server *server, const char *proc_name, wirecall_handler_fn handler,
                                         void *user_data);
int wirecall_server_add_route_name_ex(wirecall_server *server, const char *proc_name, wirecall_handler_fn handler,
                                      void *user_data, wirecall_route_finalizer_fn finalizer);
int wirecall_server_add_async_route_name_ex(wirecall_server *server, const char *proc_name, wirecall_handler_fn handler,
                                            void *user_data, wirecall_route_finalizer_fn finalizer);
int wirecall_server_remove_route_name(wirecall_server *server, const char *proc_name);

#ifdef __cplusplus
}
#endif

#endif
