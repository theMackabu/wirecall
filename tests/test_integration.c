#include "rpc/client.h"
#include "rpc/server.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static int add_handler(rpc_ctx *ctx, const rpc_value *args, size_t argc,
                       rpc_writer *out, void *user_data) {
  (void)user_data;
  if (argc != 2 || args[0].type != RPC_TYPE_I64 || args[1].type != RPC_TYPE_I64) {
    return -1;
  }
  rpc_ctx_yield(ctx);
  return rpc_writer_i64(out, args[0].as.i64 + args[1].as.i64);
}

static void *server_thread(void *arg) {
  rpc_server_run(arg);
  return NULL;
}

static void port_string(uint16_t port, char out[16]) {
  char port_buf[16];
  snprintf(port_buf, sizeof(port_buf), "%u", port);
  snprintf(out, 16, "%s", port_buf);
}

typedef struct client_job {
  uint16_t port;
  int index;
} client_job;

static void *client_thread(void *arg) {
  client_job *job = arg;
  char port[16];
  port_string(job->port, port);

  rpc_client *client = NULL;
  assert(rpc_client_connect(&client, "127.0.0.1", port) == 0);

  rpc_writer payload;
  rpc_writer_init(&payload);
  rpc_writer_i64(&payload, job->index);
  rpc_writer_i64(&payload, 10);

  rpc_value *values = NULL;
  size_t count = 0;
  assert(rpc_client_call(client, 7, &payload, &values, &count) == 0);
  assert(count == 1);
  assert(values[0].as.i64 == job->index + 10);

  rpc_values_free(values);
  rpc_writer_free(&payload);
  rpc_client_close(client);
  return NULL;
}

int main(void) {
  rpc_server *server = NULL;
  assert(rpc_server_init(&server) == 0);
  assert(rpc_server_add_route(server, 7, add_handler, NULL) == 0);
  assert(rpc_server_bind(server, "127.0.0.1", "0") == 0);
  assert(rpc_server_listen(server) == 0);
  uint16_t port = rpc_server_port(server);
  assert(port != 0);

  pthread_t thread;
  assert(pthread_create(&thread, NULL, server_thread, server) == 0);

  char port_buf[16];
  port_string(port, port_buf);

  rpc_client *client = NULL;
  assert(rpc_client_connect(&client, "127.0.0.1", port_buf) == 0);
  assert(rpc_client_ping(client) == 0);

  rpc_writer payload;
  rpc_writer_init(&payload);
  rpc_writer_i64(&payload, 5);
  rpc_writer_i64(&payload, 6);

  rpc_value *values = NULL;
  size_t count = 0;
  assert(rpc_client_call(client, 7, &payload, &values, &count) == 0);
  assert(count == 1 && values[0].type == RPC_TYPE_I64 && values[0].as.i64 == 11);
  rpc_values_free(values);
  rpc_writer_reset(&payload);

  assert(rpc_client_call(client, 999, &payload, &values, &count) != 0);

  enum { CLIENTS = 4 };
  pthread_t clients[CLIENTS];
  client_job jobs[CLIENTS];
  for (int i = 0; i < CLIENTS; ++i) {
    jobs[i] = (client_job){.port = port, .index = i};
    assert(pthread_create(&clients[i], NULL, client_thread, &jobs[i]) == 0);
  }
  for (int i = 0; i < CLIENTS; ++i) {
    assert(pthread_join(clients[i], NULL) == 0);
  }

  assert(rpc_server_remove_route(server, 7) == 0);
  assert(rpc_client_call(client, 7, &payload, &values, &count) != 0);
  rpc_writer_free(&payload);
  rpc_client_close(client);

  rpc_server_stop(server);
  assert(pthread_join(thread, NULL) == 0);
  rpc_server_destroy(server);
  return 0;
}
