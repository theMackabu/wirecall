#include "rpc/client.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
  const char *host = argc > 1 ? argv[1] : "127.0.0.1";
  const char *port = argc > 2 ? argv[2] : "7001";
  int64_t a = argc > 3 ? strtoll(argv[3], NULL, 10) : 20;
  int64_t b = argc > 4 ? strtoll(argv[4], NULL, 10) : 22;

  rpc_client *client = NULL;
  if (rpc_client_connect(&client, host, port) != 0) {
    fprintf(stderr, "connect failed\n");
    return 1;
  }

  rpc_writer args;
  rpc_writer_init(&args);
  rpc_writer_i64(&args, a);
  rpc_writer_i64(&args, b);

  rpc_value *result = NULL;
  size_t result_count = 0;
  if (rpc_client_call_name(client, "add", &args, &result, &result_count) != 0) {
    fprintf(stderr, "async RPC error: %s\n", rpc_client_error(client));
    rpc_writer_free(&args);
    rpc_client_close(client);
    return 1;
  }

  if (result_count == 1 && result[0].type == RPC_TYPE_I64) {
    printf("async %lld + %lld = %lld\n", (long long)a, (long long)b, (long long)result[0].as.i64);
  } else {
    fprintf(stderr, "unexpected response\n");
  }

  rpc_values_free(result);
  rpc_writer_free(&args);
  rpc_client_close(client);
  return 0;
}
