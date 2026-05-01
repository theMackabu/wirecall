#include "rpc/client.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

struct rpc_client {
  int fd;
  uint64_t next_call_id;
  char error[160];
};

static void set_error(rpc_client *client, const char *message) {
  if (client) {
    snprintf(client->error, sizeof(client->error), "%s", message);
  }
}

static int write_full(int fd, const void *data, size_t len) {
  const uint8_t *p = data;
  while (len > 0) {
    ssize_t n = send(fd, p, len, 0);
    if (n > 0) {
      p += n;
      len -= (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return -1;
  }
  return 0;
}

static int read_full(int fd, void *data, size_t len) {
  uint8_t *p = data;
  while (len > 0) {
    ssize_t n = recv(fd, p, len, 0);
    if (n > 0) {
      p += n;
      len -= (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return -1;
  }
  return 0;
}

static int send_packet(rpc_client *client, rpc_op op, uint32_t proc_id,
                       uint64_t call_id, const rpc_writer *payload) {
  rpc_header header = {
      .op = op,
      .flags = RPC_FLAG_NONE,
      .proc_id = proc_id,
      .size = payload ? (uint32_t)payload->len : 0,
      .call_id = call_id,
  };
  uint8_t header_buf[RPC_HEADER_SIZE];

  if (rpc_header_encode(&header, header_buf) != 0 ||
      write_full(client->fd, header_buf, sizeof(header_buf)) != 0) {
    set_error(client, "send failed");
    return -1;
  }
  if (payload && payload->len > 0 &&
      write_full(client->fd, payload->data, payload->len) != 0) {
    set_error(client, "send failed");
    return -1;
  }
  return 0;
}

static int recv_packet(rpc_client *client, rpc_header *header, uint8_t **body) {
  uint8_t header_buf[RPC_HEADER_SIZE];
  *body = NULL;

  if (read_full(client->fd, header_buf, sizeof(header_buf)) != 0 ||
      rpc_header_decode(header_buf, header) != 0 ||
      header->size > RPC_MAX_PAYLOAD_SIZE) {
    set_error(client, "read failed");
    return -1;
  }

  *body = malloc(header->size ? header->size : 1);
  if (!*body) {
    set_error(client, "out of memory");
    return -1;
  }
  if (read_full(client->fd, *body, header->size) != 0) {
    free(*body);
    *body = NULL;
    set_error(client, "read failed");
    return -1;
  }
  return 0;
}

int rpc_client_connect(rpc_client **out_client, const char *host,
                       const char *port) {
  if (!out_client || !port) {
    return -1;
  }

  rpc_client *client = calloc(1, sizeof(*client));
  if (!client) {
    return -1;
  }
  client->fd = -1;
  client->next_call_id = 1;

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = NULL;
  if (getaddrinfo(host, port, &hints, &res) != 0) {
    free(client);
    return -1;
  }

  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      continue;
    }
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      client->fd = fd;
      break;
    }
    close(fd);
  }
  freeaddrinfo(res);

  if (client->fd < 0) {
    free(client);
    return -1;
  }

  *out_client = client;
  return 0;
}

void rpc_client_close(rpc_client *client) {
  if (!client) {
    return;
  }
  if (client->fd >= 0) {
    (void)send_packet(client, RPC_OP_DISCONNECT, 0, client->next_call_id++, NULL);
    close(client->fd);
  }
  free(client);
}

int rpc_client_ping(rpc_client *client) {
  if (!client || client->fd < 0) {
    return -1;
  }
  uint64_t call_id = client->next_call_id++;
  if (send_packet(client, RPC_OP_PING, 0, call_id, NULL) != 0) {
    return -1;
  }

  rpc_header header;
  uint8_t *body = NULL;
  if (recv_packet(client, &header, &body) != 0) {
    return -1;
  }
  free(body);

  if (header.op != RPC_OP_RESPONSE || header.call_id != call_id ||
      header.size != 0) {
    set_error(client, "unexpected ping response");
    return -1;
  }
  return 0;
}

int rpc_client_call(rpc_client *client, uint32_t proc_id,
                    const rpc_writer *args, rpc_value **out_values,
                    size_t *out_count) {
  if (!client || client->fd < 0 || !out_values || !out_count) {
    return -1;
  }
  *out_values = NULL;
  *out_count = 0;

  uint64_t call_id = client->next_call_id++;
  if (send_packet(client, RPC_OP_RPC, proc_id, call_id, args) != 0) {
    return -1;
  }

  rpc_header header;
  uint8_t *body = NULL;
  if (recv_packet(client, &header, &body) != 0) {
    return -1;
  }

  int rc = -1;
  if (header.call_id != call_id) {
    set_error(client, "unexpected call id");
  } else if (header.op == RPC_OP_RESPONSE) {
    if (rpc_payload_decode(body, header.size, out_values, out_count) == 0) {
      rc = 0;
    } else {
      set_error(client, "malformed response payload");
    }
  } else if (header.op == RPC_OP_ERROR) {
    rpc_value *values = NULL;
    size_t count = 0;
    if (rpc_payload_decode(body, header.size, &values, &count) == 0 &&
        count == 1 && values[0].type == RPC_TYPE_STRING) {
      size_t n = values[0].as.string.len;
      if (n >= sizeof(client->error)) {
        n = sizeof(client->error) - 1u;
      }
      memcpy(client->error, values[0].as.string.data, n);
      client->error[n] = '\0';
    } else {
      set_error(client, "server returned error");
    }
    rpc_values_free(values);
  } else {
    set_error(client, "unexpected response op");
  }

  free(body);
  return rc;
}

const char *rpc_client_error(const rpc_client *client) {
  return client && client->error[0] ? client->error : "client error";
}
