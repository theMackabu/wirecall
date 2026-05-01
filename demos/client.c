#include "wirecall/client.h"

#include <stdio.h>
#include <stdlib.h>

typedef long long longer;

int main(int argc, char **argv) {
  const char *host = argc > 1 ? argv[1] : "127.0.0.1";
  const char *port = argc > 2 ? argv[2] : "7000";

  int64_t a = argc > 3 ? strtoll(argv[3], NULL, 10) : 20;
  int64_t b = argc > 4 ? strtoll(argv[4], NULL, 10) : 22;

  wirecall_client *client = NULL;
  if (wirecall_client_connect(&client, host, port) != 0) {
    fprintf(stderr, "connect failed\n");
    return 1;
  }

  wirecall_writer args;
  wirecall_writer_init(&args);
  wirecall_writer_i64(&args, a);
  wirecall_writer_i64(&args, b);

  wirecall_value *result = NULL;
  size_t result_count = 0;

  if (wirecall_client_call_name(client, "add", &args, &result, &result_count) != 0) {
    fprintf(stderr, "Wirecall error: %s\n", wirecall_client_error(client));
    wirecall_writer_free(&args);
    wirecall_client_close(client);
    return 1;
  }

  if (result_count == 1 && result[0].type == WIRECALL_TYPE_I64)
    printf("%lld + %lld = %lld\n", (longer)a, (longer)b, (longer)result[0].as.i64);
  else fprintf(stderr, "unexpected response\n");

  wirecall_values_free(result);
  wirecall_writer_free(&args);
  wirecall_client_close(client);

  return 0;
}
