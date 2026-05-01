#include "wirecall/server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static wirecall_server *g_server;

static void on_signal(int signo) {
  (void)signo;
  if (g_server) { wirecall_server_stop(g_server); }
}

static int add_i64(wirecall_ctx *ctx, const wirecall_value *args, size_t argc, wirecall_writer *out, void *user_data) {
  (void)ctx;
  (void)user_data;
  if (argc != 2 || args[0].type != WIRECALL_TYPE_I64 || args[1].type != WIRECALL_TYPE_I64) { return -1; }
  return wirecall_writer_i64(out, args[0].as.i64 + args[1].as.i64);
}

static int echo_string(wirecall_ctx *ctx, const wirecall_value *args, size_t argc, wirecall_writer *out,
                       void *user_data) {
  (void)user_data;
  if (argc != 1 || args[0].type != WIRECALL_TYPE_STRING) { return -1; }
  wirecall_ctx_yield(ctx);
  return wirecall_writer_string(out, args[0].as.string.data, args[0].as.string.len);
}

int main(int argc, char **argv) {
  const char *host = argc > 1 ? argv[1] : "127.0.0.1";
  const char *port = argc > 2 ? argv[2] : "7000";
  uint32_t workers = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 0;

  if (wirecall_server_init(&g_server) != 0 || (workers != 0 && wirecall_server_set_workers(g_server, workers) != 0) ||
      wirecall_server_add_route_name(g_server, "add", add_i64, NULL) != 0 ||
      wirecall_server_add_async_route_name(g_server, "echo", echo_string, NULL) != 0 ||
      wirecall_server_bind(g_server, host, port) != 0 || wirecall_server_listen(g_server) != 0) {
    fprintf(stderr, "failed to start Wirecall server\n");
    wirecall_server_destroy(g_server);
    return 1;
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  printf("rpc demo server listening on %s:%u\n", host, wirecall_server_port(g_server));
  int rc = wirecall_server_run(g_server);
  wirecall_server_destroy(g_server);
  return rc == 0 ? 0 : 1;
}
