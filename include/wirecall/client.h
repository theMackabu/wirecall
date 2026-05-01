#ifndef WIRECALL_CLIENT_H
#define WIRECALL_CLIENT_H

#include "wirecall/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wirecall_client wirecall_client;

int wirecall_client_connect(wirecall_client **out_client, const char *host, const char *port);
int wirecall_client_set_integrity(wirecall_client *client, uint32_t integrity, const uint8_t mac_key[16]);
void wirecall_client_close(wirecall_client *client);

int wirecall_client_ping(wirecall_client *client);
int wirecall_client_call_name(wirecall_client *client, const char *proc_name, const wirecall_writer *args,
                              wirecall_value **out_values, size_t *out_count);
int wirecall_client_send_call_name(wirecall_client *client, const char *proc_name, const wirecall_writer *args,
                                   uint64_t *out_call_id);
int wirecall_client_recv_response(wirecall_client *client, uint64_t *out_call_id, wirecall_value **out_values,
                                  size_t *out_count);

const char *wirecall_client_error(const wirecall_client *client);

#ifdef __cplusplus
}
#endif

#endif
