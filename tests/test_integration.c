#include "wirecall/client.h"
#include "wirecall/server.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>

static int add_handler(wirecall_ctx *ctx, const wirecall_value *args, size_t argc, wirecall_writer *out,
                       void *user_data) {
  (void)user_data;
  if (argc != 2 || args[0].type != WIRECALL_TYPE_I64 || args[1].type != WIRECALL_TYPE_I64) { return -1; }
  wirecall_ctx_yield(ctx);
  return wirecall_writer_i64(out, args[0].as.i64 + args[1].as.i64);
}

static void *server_thread(void *arg) {
  wirecall_server_run(arg);
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

  wirecall_client *client = NULL;
  assert(wirecall_client_connect(&client, "127.0.0.1", port) == 0);

  wirecall_writer payload;
  wirecall_writer_init(&payload);
  wirecall_writer_i64(&payload, job->index);
  wirecall_writer_i64(&payload, 10);

  wirecall_value *values = NULL;
  size_t count = 0;
  assert(wirecall_client_call_name(client, "add", &payload, &values, &count) == 0);
  assert(count == 1);
  assert(values[0].as.i64 == job->index + 10);

  wirecall_values_free(values);
  wirecall_writer_free(&payload);
  wirecall_client_close(client);
  return NULL;
}

int main(void) {
  wirecall_server *server = NULL;
  assert(wirecall_server_init(&server) == 0);
  assert(wirecall_server_add_async_route_name(server, "add", add_handler, NULL) == 0);
  assert(wirecall_server_bind(server, "127.0.0.1", "0") == 0);
  assert(wirecall_server_listen(server) == 0);
  uint16_t port = wirecall_server_port(server);
  assert(port != 0);

  pthread_t thread;
  assert(pthread_create(&thread, NULL, server_thread, server) == 0);

  char port_buf[16];
  port_string(port, port_buf);

  wirecall_client *client = NULL;
  assert(wirecall_client_connect(&client, "127.0.0.1", port_buf) == 0);
  assert(wirecall_client_ping(client) == 0);

  wirecall_writer payload;
  wirecall_writer_init(&payload);
  wirecall_writer_i64(&payload, 5);
  wirecall_writer_i64(&payload, 6);

  wirecall_value *values = NULL;
  size_t count = 0;
  assert(wirecall_client_call_name(client, "add", &payload, &values, &count) == 0);
  assert(count == 1 && values[0].type == WIRECALL_TYPE_I64 && values[0].as.i64 == 11);
  wirecall_values_free(values);
  wirecall_writer_reset(&payload);

  assert(wirecall_client_call_name(client, "missing", &payload, &values, &count) != 0);

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

  assert(wirecall_server_remove_route_name(server, "add") == 0);
  assert(wirecall_client_call_name(client, "add", &payload, &values, &count) != 0);
  wirecall_writer_free(&payload);
  wirecall_client_close(client);

  wirecall_server_stop(server);
  assert(pthread_join(thread, NULL) == 0);
  wirecall_server_destroy(server);
  return 0;
}
