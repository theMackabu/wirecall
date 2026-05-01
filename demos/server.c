#include "rpc/server.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static rpc_server *g_server;

static void on_signal(int signo) {
  (void)signo;
  if (g_server) {
    rpc_server_stop(g_server);
  }
}

static int add_i64(rpc_ctx *ctx, const rpc_value *args, size_t argc,
                   rpc_writer *out, void *user_data) {
  (void)ctx;
  (void)user_data;
  if (argc != 2 || args[0].type != RPC_TYPE_I64 || args[1].type != RPC_TYPE_I64) {
    return -1;
  }
  return rpc_writer_i64(out, args[0].as.i64 + args[1].as.i64);
}

static int echo_string(rpc_ctx *ctx, const rpc_value *args, size_t argc,
                       rpc_writer *out, void *user_data) {
  (void)user_data;
  if (argc != 1 || args[0].type != RPC_TYPE_STRING) {
    return -1;
  }
  rpc_ctx_yield(ctx);
  return rpc_writer_string(out, args[0].as.string.data, args[0].as.string.len);
}

int main(int argc, char **argv) {
  const char *host = argc > 1 ? argv[1] : "127.0.0.1";
  const char *port = argc > 2 ? argv[2] : "7000";
  uint32_t workers = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 0;

  if (rpc_server_init(&g_server) != 0 ||
      (workers != 0 && rpc_server_set_workers(g_server, workers) != 0) ||
      rpc_server_add_route(g_server, 1, add_i64, NULL) != 0 ||
      rpc_server_add_route(g_server, 2, echo_string, NULL) != 0 ||
      rpc_server_bind(g_server, host, port) != 0 ||
      rpc_server_listen(g_server) != 0) {
    fprintf(stderr, "failed to start RPC server\n");
    rpc_server_destroy(g_server);
    return 1;
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  printf("rpc demo server listening on %s:%u\n", host, rpc_server_port(g_server));
  int rc = rpc_server_run(g_server);
  rpc_server_destroy(g_server);
  return rc == 0 ? 0 : 1;
}
