#include "wirecall/client.h"
#include "wirecall/trace.h"

#include "memory.h"
#include "proc.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifdef MSG_NOSIGNAL
#define WIRECALL_SEND_FLAGS MSG_NOSIGNAL
#else
#define WIRECALL_SEND_FLAGS 0
#endif

struct wirecall_client {
  int fd;
  uint64_t next_call_id;
  uint8_t *read_buf;
  size_t read_off;
  size_t read_len;
  size_t read_cap;
  uint32_t integrity;
  uint8_t mac_key[16];
  char error[160];
};

static void set_error(wirecall_client *client, const char *message) {
  if (client) { snprintf(client->error, sizeof(client->error), "%s", message); }
}

static int send_iov_full(int fd, const struct iovec *iov, int iov_count) {
  struct iovec local[2];
  if (iov_count <= 0 || iov_count > 2) { return -1; }
  memcpy(local, iov, (size_t)iov_count * sizeof(*iov));

  while (iov_count > 0) {
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = local;
    msg.msg_iovlen = (size_t)iov_count;

    ssize_t n = sendmsg(fd, &msg, WIRECALL_SEND_FLAGS);
    if (n > 0) {
      size_t sent = (size_t)n;
      while (iov_count > 0 && sent >= local[0].iov_len) {
        sent -= local[0].iov_len;
        local[0] = local[1];
        iov_count--;
      }
      if (iov_count > 0 && sent > 0) {
        local[0].iov_base = (uint8_t *)local[0].iov_base + sent;
        local[0].iov_len -= sent;
      }
      continue;
    }
    if (n < 0 && errno == EINTR) { continue; }
    return -1;
  }
  return 0;
}

static size_t client_read_available(const wirecall_client *client) {
  return client->read_len - client->read_off;
}

static int client_read_reserve(wirecall_client *client, size_t need) {
  if (client_read_available(client) >= need) { return 0; }
  if (client->read_off > 0) {
    memmove(client->read_buf, client->read_buf + client->read_off, client_read_available(client));
    client->read_len -= client->read_off;
    client->read_off = 0;
  }
  if (client->read_cap >= need) { return 0; }

  size_t next_cap = client->read_cap ? client->read_cap : 65536u;
  while (next_cap < need) {
    next_cap *= 2u;
  }
  uint8_t *next = wirecall_mem_realloc(client->read_buf, next_cap);
  if (!next) { return -1; }
  client->read_buf = next;
  client->read_cap = next_cap;
  return 0;
}

static int client_read_fill(wirecall_client *client, size_t need) {
  while (client_read_available(client) < need) {
    if (client_read_reserve(client, need) != 0) { return -1; }
    if (client->read_len == client->read_cap && client_read_reserve(client, client->read_cap + 1u) != 0) { return -1; }
    ssize_t n = recv(client->fd, client->read_buf + client->read_len, client->read_cap - client->read_len, 0);
    if (n > 0) {
      client->read_len += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) { continue; }
    return -1;
  }
  return 0;
}

static int send_packet(wirecall_client *client, wirecall_op op, uint64_t proc_id, uint64_t call_id,
                       const wirecall_writer *payload) {
  uint64_t trace = wirecall_trace_begin();
  wirecall_header header = {
    .op = op,
    .flags = WIRECALL_FLAG_NONE,
    .proc_id = proc_id,
    .size = payload ? (uint32_t)payload->len : 0,
    .call_id = call_id,
  };
  uint8_t header_buf[WIRECALL_HEADER_SIZE];

  if (wirecall_packet_sign_ex(&header, payload ? payload->data : NULL, payload ? payload->len : 0, client->integrity,
                              client->mac_key) != 0) {
    set_error(client, "send failed");
    wirecall_trace_end(WIRECALL_TRACE_CLIENT_SEND, trace);
    return -1;
  }
  if (wirecall_header_encode(&header, header_buf) != 0) {
    set_error(client, "send failed");
    wirecall_trace_end(WIRECALL_TRACE_CLIENT_SEND, trace);
    return -1;
  }

  struct iovec iov[2];
  int iov_count = 1;
  iov[0] = (struct iovec){
    .iov_base = header_buf,
    .iov_len = sizeof(header_buf),
  };
  if (payload && payload->len > 0) {
    iov[1] = (struct iovec){
      .iov_base = payload->data,
      .iov_len = payload->len,
    };
    iov_count = 2;
  }

  if (send_iov_full(client->fd, iov, iov_count) != 0) {
    set_error(client, "send failed");
    wirecall_trace_end(WIRECALL_TRACE_CLIENT_SEND, trace);
    return -1;
  }
  wirecall_trace_end(WIRECALL_TRACE_CLIENT_SEND, trace);
  return 0;
}

static int recv_packet(wirecall_client *client, wirecall_header *header, uint8_t **body) {
  uint64_t trace = wirecall_trace_begin();
  *body = NULL;

  if (client_read_fill(client, WIRECALL_HEADER_SIZE) != 0 ||
      wirecall_header_decode(client->read_buf + client->read_off, header) != 0 ||
      header->size > WIRECALL_MAX_PAYLOAD_SIZE) {
    set_error(client, "read failed");
    wirecall_trace_end(WIRECALL_TRACE_CLIENT_RECV, trace);
    return -1;
  }

  size_t packet_size = WIRECALL_HEADER_SIZE + (size_t)header->size;
  if (client_read_fill(client, packet_size) != 0) {
    set_error(client, "read failed");
    wirecall_trace_end(WIRECALL_TRACE_CLIENT_RECV, trace);
    return -1;
  }

  const uint8_t *payload = client->read_buf + client->read_off + WIRECALL_HEADER_SIZE;
  if (wirecall_packet_verify_ex(header, payload, header->size, client->integrity, client->mac_key) != 0) {
    set_error(client, "packet integrity failed");
    wirecall_trace_end(WIRECALL_TRACE_CLIENT_RECV, trace);
    return -1;
  }

  *body = wirecall_mem_alloc(header->size ? header->size : 1);
  if (!*body) {
    set_error(client, "out of memory");
    wirecall_trace_end(WIRECALL_TRACE_CLIENT_RECV, trace);
    return -1;
  }
  if (header->size > 0) { memcpy(*body, payload, header->size); }
  client->read_off += packet_size;
  if (client->read_off == client->read_len) {
    client->read_off = 0;
    client->read_len = 0;
  }
  wirecall_trace_end(WIRECALL_TRACE_CLIENT_RECV, trace);
  return 0;
}

static int client_send_call_id(wirecall_client *client, uint64_t proc_id, const wirecall_writer *args,
                               uint64_t *out_call_id);

int wirecall_client_connect(wirecall_client **out_client, const char *host, const char *port) {
  if (!out_client || !port) { return -1; }

  wirecall_client *client = wirecall_mem_calloc(1, sizeof(*client));
  if (!client) { return -1; }
  client->fd = -1;
  client->next_call_id = 1;
  client->integrity = WIRECALL_INTEGRITY_DEFAULT;

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  struct addrinfo *res = NULL;
  if (getaddrinfo(host, port, &hints, &res) != 0) {
    wirecall_mem_free(client);
    return -1;
  }

  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { continue; }
#ifdef SO_NOSIGPIPE
    int no_sigpipe = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      client->fd = fd;
      break;
    }
    close(fd);
  }
  freeaddrinfo(res);

  if (client->fd < 0) {
    wirecall_mem_free(client);
    return -1;
  }

  *out_client = client;
  return 0;
}

int wirecall_client_set_integrity(wirecall_client *client, uint32_t integrity, const uint8_t mac_key[16]) {
  if (!client || (integrity & ~(WIRECALL_INTEGRITY_CHECKSUM | WIRECALL_INTEGRITY_MAC)) ||
      ((integrity & WIRECALL_INTEGRITY_MAC) && !mac_key)) {
    return -1;
  }
  client->integrity = integrity;
  if (mac_key) { memcpy(client->mac_key, mac_key, sizeof(client->mac_key)); }
  return 0;
}

void wirecall_client_close(wirecall_client *client) {
  if (!client) { return; }
  if (client->fd >= 0) {
    (void)send_packet(client, WIRECALL_OP_DISCONNECT, 0, client->next_call_id++, NULL);
    close(client->fd);
  }
  wirecall_mem_free(client->read_buf);
  wirecall_mem_free(client);
}

int wirecall_client_ping(wirecall_client *client) {
  if (!client || client->fd < 0) { return -1; }
  uint64_t call_id = client->next_call_id++;
  if (send_packet(client, WIRECALL_OP_PING, 0, call_id, NULL) != 0) { return -1; }

  wirecall_header header;
  uint8_t *body = NULL;
  if (recv_packet(client, &header, &body) != 0) { return -1; }
  wirecall_mem_free(body);

  if (header.op != WIRECALL_OP_RESPONSE || header.call_id != call_id || header.size != 0) {
    set_error(client, "unexpected ping response");
    return -1;
  }
  return 0;
}

static int client_call_id(wirecall_client *client, uint64_t proc_id, const wirecall_writer *args,
                          wirecall_value **out_values, size_t *out_count) {
  uint64_t trace_call = wirecall_trace_begin();
  if (!client || client->fd < 0 || !out_values || !out_count) {
    wirecall_trace_end(WIRECALL_TRACE_CLIENT_CALL, trace_call);
    return -1;
  }
  *out_values = NULL;
  *out_count = 0;

  uint64_t call_id = 0;
  if (client_send_call_id(client, proc_id, args, &call_id) != 0) {
    wirecall_trace_end(WIRECALL_TRACE_CLIENT_CALL, trace_call);
    return -1;
  }

  uint64_t response_call_id = 0;
  int rc = wirecall_client_recv_response(client, &response_call_id, out_values, out_count);
  if (rc == 0 && response_call_id != call_id) {
    wirecall_values_free(*out_values);
    *out_values = NULL;
    *out_count = 0;
    set_error(client, "unexpected call id");
    rc = -1;
  }

  wirecall_trace_end(WIRECALL_TRACE_CLIENT_CALL, trace_call);
  return rc;
}

int wirecall_client_call_name(wirecall_client *client, const char *proc_name, const wirecall_writer *args,
                              wirecall_value **out_values, size_t *out_count) {
  return client_call_id(client, wirecall_proc_id(proc_name), args, out_values, out_count);
}

static int client_send_call_id(wirecall_client *client, uint64_t proc_id, const wirecall_writer *args,
                               uint64_t *out_call_id) {
  if (!client || client->fd < 0 || !out_call_id) { return -1; }
  uint64_t call_id = client->next_call_id++;
  if (send_packet(client, WIRECALL_OP_RPC, proc_id, call_id, args) != 0) { return -1; }
  *out_call_id = call_id;
  return 0;
}

int wirecall_client_send_call_name(wirecall_client *client, const char *proc_name, const wirecall_writer *args,
                                   uint64_t *out_call_id) {
  return client_send_call_id(client, wirecall_proc_id(proc_name), args, out_call_id);
}

int wirecall_client_recv_response(wirecall_client *client, uint64_t *out_call_id, wirecall_value **out_values,
                                  size_t *out_count) {
  if (!client || client->fd < 0 || !out_call_id || !out_values || !out_count) { return -1; }
  *out_call_id = 0;
  *out_values = NULL;
  *out_count = 0;

  wirecall_header header;
  uint8_t *body = NULL;
  if (recv_packet(client, &header, &body) != 0) { return -1; }

  int rc = -1;
  *out_call_id = header.call_id;
  if (header.op == WIRECALL_OP_RESPONSE) {
    uint64_t trace_decode = wirecall_trace_begin();
    if (wirecall_payload_decode(body, header.size, out_values, out_count) == 0) {
      rc = 0;
    } else {
      set_error(client, "malformed response payload");
    }
    wirecall_trace_end(WIRECALL_TRACE_CLIENT_DECODE, trace_decode);
  } else if (header.op == WIRECALL_OP_ERROR) {
    wirecall_value *values = NULL;
    size_t count = 0;
    if (wirecall_payload_decode(body, header.size, &values, &count) == 0 && count == 1 &&
        values[0].type == WIRECALL_TYPE_STRING) {
      size_t n = values[0].as.string.len;
      if (n >= sizeof(client->error)) { n = sizeof(client->error) - 1u; }
      memcpy(client->error, values[0].as.string.data, n);
      client->error[n] = '\0';
    } else {
      set_error(client, "server returned error");
    }
    wirecall_values_free(values);
  } else {
    set_error(client, "unexpected response op");
  }

  wirecall_mem_free(body);
  return rc;
}

const char *wirecall_client_error(const wirecall_client *client) {
  return client && client->error[0] ? client->error : "client error";
}
