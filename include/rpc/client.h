#ifndef RPC_CLIENT_H
#define RPC_CLIENT_H

#include "rpc/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rpc_client rpc_client;

int rpc_client_connect(rpc_client **out_client, const char *host, const char *port);
void rpc_client_close(rpc_client *client);

int rpc_client_ping(rpc_client *client);
int rpc_client_call_name(rpc_client *client, const char *proc_name, const rpc_writer *args, rpc_value **out_values,
                         size_t *out_count);
int rpc_client_send_call_name(rpc_client *client, const char *proc_name, const rpc_writer *args, uint64_t *out_call_id);
int rpc_client_recv_response(rpc_client *client, uint64_t *out_call_id, rpc_value **out_values, size_t *out_count);

const char *rpc_client_error(const rpc_client *client);

#ifdef __cplusplus
}
#endif

#endif
