#include "rpc/server.h"

#include "arena.h"
#include "backend.h"
#include "routes.h"
#include "scheduler.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RPC_MAX_LISTENERS 8
#define RPC_READ_CHUNK 4096
#define RPC_EVENT_BATCH 64
#define RPC_CONNECTION_ARENA_CAPACITY 65536u

typedef struct rpc_listener {
  int fd;
  rpc_server *server;
} rpc_listener;

typedef struct rpc_connection {
  int fd;
  rpc_server *server;
  uint8_t *read_buf;
  size_t read_len;
  size_t read_cap;
  uint8_t *write_buf;
  size_t write_len;
  size_t write_cap;
  size_t write_off;
  int closing;
  struct rpc_connection *next;
} rpc_connection;

struct rpc_server {
  rpc_backend *backend;
  rpc_routes routes;
  rpc_scheduler *scheduler;
  rpc_fixed_arena connection_arena;
  rpc_listener listeners[RPC_MAX_LISTENERS];
  size_t listener_count;
  rpc_connection *connections;
  atomic_bool stopping;
  int listening;
  int routes_ready;
  uint16_t port;
};

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int append_bytes(uint8_t **buf, size_t *len, size_t *cap,
                        const void *data, size_t data_len) {
  if (data_len == 0) {
    return 0;
  }
  if (*len > SIZE_MAX - data_len) {
    return -1;
  }
  size_t need = *len + data_len;
  if (need > *cap) {
    size_t next_cap = *cap ? *cap : 4096u;
    while (next_cap < need) {
      next_cap *= 2u;
    }
    uint8_t *next = realloc(*buf, next_cap);
    if (!next) {
      return -1;
    }
    *buf = next;
    *cap = next_cap;
  }
  if (data) {
    memcpy(*buf + *len, data, data_len);
  }
  *len += data_len;
  return 0;
}

static int reserve_bytes(uint8_t **buf, size_t *cap, size_t need) {
  if (need <= *cap) {
    return 0;
  }
  size_t next_cap = *cap ? *cap : 4096u;
  while (next_cap < need) {
    next_cap *= 2u;
  }
  uint8_t *next = realloc(*buf, next_cap);
  if (!next) {
    return -1;
  }
  *buf = next;
  *cap = next_cap;
  return 0;
}

static int queue_packet(rpc_connection *conn, rpc_op op, uint8_t flags,
                        uint32_t proc_id, uint64_t call_id,
                        const uint8_t *payload, size_t payload_len) {
  if (payload_len > RPC_MAX_PAYLOAD_SIZE) {
    return -1;
  }
  uint8_t header_buf[RPC_HEADER_SIZE];
  rpc_header header = {
      .op = op,
      .flags = flags,
      .proc_id = proc_id,
      .size = (uint32_t)payload_len,
      .call_id = call_id,
  };
  if (rpc_header_encode(&header, header_buf) != 0) {
    return -1;
  }
  if (append_bytes(&conn->write_buf, &conn->write_len, &conn->write_cap,
                   header_buf, sizeof(header_buf)) != 0) {
    return -1;
  }
  if (append_bytes(&conn->write_buf, &conn->write_len, &conn->write_cap, payload,
                   payload_len) != 0) {
    return -1;
  }
  return rpc_backend_modify(conn->server->backend, conn->fd,
                            RPC_BACKEND_READ | RPC_BACKEND_WRITE,
                            (uintptr_t)conn);
}

static int queue_string_error(rpc_connection *conn, uint32_t proc_id,
                              uint64_t call_id, const char *message) {
  rpc_writer writer;
  rpc_writer_init(&writer);
  int rc = rpc_writer_string(&writer, message, (uint32_t)strlen(message));
  if (rc == 0) {
    rc = queue_packet(conn, RPC_OP_ERROR, RPC_FLAG_NONE, proc_id, call_id,
                      writer.data, writer.len);
  }
  rpc_writer_free(&writer);
  return rc;
}

static void connection_destroy(rpc_connection *conn) {
  if (!conn) {
    return;
  }
  rpc_server *server = conn->server;
  (void)rpc_backend_remove(server->backend, conn->fd);
  close(conn->fd);
  free(conn->read_buf);
  free(conn->write_buf);

  rpc_connection **link = &server->connections;
  while (*link && *link != conn) {
    link = &(*link)->next;
  }
  if (*link == conn) {
    *link = conn->next;
  }
  rpc_fixed_arena_free(&server->connection_arena, conn);
}

static void maybe_close(rpc_connection *conn) {
  if (conn->closing && conn->write_len == conn->write_off) {
    connection_destroy(conn);
  }
}

static void on_call_done(rpc_call *call, void *user_data) {
  rpc_connection *conn = user_data;
  const rpc_writer *response = rpc_call_response(call);
  if (rpc_call_result(call) == 0) {
    (void)queue_packet(conn, RPC_OP_RESPONSE, RPC_FLAG_NONE,
                       rpc_call_proc_id(call), rpc_call_id(call), response->data,
                       response->len);
  } else {
    (void)queue_string_error(conn, rpc_call_proc_id(call), rpc_call_id(call),
                             rpc_call_error(call));
  }
}

static int handle_packet(rpc_connection *conn, const rpc_header *header,
                         const uint8_t *payload) {
  static const void *dispatch[] = {
      [RPC_OP_RPC] = &&op_rpc,
      [RPC_OP_PING] = &&op_ping,
      [RPC_OP_DISCONNECT] = &&op_disconnect,
      [RPC_OP_RESPONSE] = &&op_unsupported,
      [RPC_OP_ERROR] = &&op_unsupported,
  };

  if ((size_t)header->op >= sizeof(dispatch) / sizeof(*dispatch) ||
      !dispatch[header->op]) {
    goto op_unsupported;
  }
  goto *dispatch[header->op];

op_ping:
    return queue_packet(conn, RPC_OP_RESPONSE, RPC_FLAG_NONE, 0, header->call_id,
                        NULL, 0);

op_disconnect:
    conn->closing = 1;
    return 0;

op_rpc: {
    rpc_route route;
    if (rpc_routes_lookup(&conn->server->routes, header->proc_id, &route) != 0) {
      return queue_string_error(conn, header->proc_id, header->call_id,
                                "unknown procedure");
    }
    if (rpc_scheduler_submit(conn->server->scheduler, header->call_id,
                             header->proc_id, route.handler, route.user_data,
                             payload, header->size, on_call_done, conn) != 0) {
      return queue_string_error(conn, header->proc_id, header->call_id,
                                "scheduler failure");
    }
    rpc_scheduler_run_ready(conn->server->scheduler);
    return 0;
  }

op_unsupported:
  return queue_string_error(conn, header->proc_id, header->call_id,
                            "unsupported operation");
}

static void parse_available(rpc_connection *conn) {
  size_t off = 0;
  while (conn->read_len - off >= RPC_HEADER_SIZE) {
    rpc_header header;
    if (rpc_header_decode(conn->read_buf + off, &header) != 0) {
      conn->closing = 1;
      break;
    }
    if (conn->read_len - off - RPC_HEADER_SIZE < header.size) {
      break;
    }
    const uint8_t *payload = conn->read_buf + off + RPC_HEADER_SIZE;
    if (handle_packet(conn, &header, payload) != 0) {
      conn->closing = 1;
      break;
    }
    off += RPC_HEADER_SIZE + header.size;
  }

  if (off > 0) {
    memmove(conn->read_buf, conn->read_buf + off, conn->read_len - off);
    conn->read_len -= off;
  }
}

static void connection_read(rpc_connection *conn) {
  for (;;) {
    if (conn->read_cap - conn->read_len < RPC_READ_CHUNK) {
      if (reserve_bytes(&conn->read_buf, &conn->read_cap,
                        conn->read_len + RPC_READ_CHUNK) != 0) {
        conn->closing = 1;
        return;
      }
    }
    ssize_t n = recv(conn->fd, conn->read_buf + conn->read_len,
                     conn->read_cap - conn->read_len, 0);
    if (n > 0) {
      conn->read_len += (size_t)n;
      parse_available(conn);
      if (conn->closing) {
        return;
      }
      continue;
    }
    if (n == 0) {
      conn->closing = 1;
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      return;
    }
    if (errno == EINTR) {
      continue;
    }
    conn->closing = 1;
    return;
  }
}

static void connection_write(rpc_connection *conn) {
  while (conn->write_off < conn->write_len) {
    ssize_t n = send(conn->fd, conn->write_buf + conn->write_off,
                     conn->write_len - conn->write_off, 0);
    if (n > 0) {
      conn->write_off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return;
    }
    conn->closing = 1;
    return;
  }

  conn->write_len = 0;
  conn->write_off = 0;
  (void)rpc_backend_modify(conn->server->backend, conn->fd, RPC_BACKEND_READ,
                           (uintptr_t)conn);
}

static void accept_ready(rpc_listener *listener) {
  for (;;) {
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    int fd = accept(listener->fd, (struct sockaddr *)&addr, &addr_len);
    if (fd < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    if (set_nonblock(fd) != 0) {
      close(fd);
      continue;
    }
    rpc_connection *conn = rpc_fixed_arena_alloc(&listener->server->connection_arena);
    if (!conn) {
      close(fd);
      continue;
    }
    conn->fd = fd;
    conn->server = listener->server;
    conn->next = listener->server->connections;
    listener->server->connections = conn;
    if (rpc_backend_register(listener->server->backend, fd, RPC_BACKEND_READ,
                             (uintptr_t)conn) != 0) {
      connection_destroy(conn);
    }
  }
}

int rpc_server_init(rpc_server **out_server) {
  if (!out_server) {
    return -1;
  }
  rpc_server *server = calloc(1, sizeof(*server));
  if (!server) {
    return -1;
  }
  for (size_t i = 0; i < RPC_MAX_LISTENERS; ++i) {
    server->listeners[i].fd = -1;
  }
  atomic_init(&server->stopping, false);
  if (rpc_backend_kqueue_create(&server->backend) != 0) {
    rpc_server_destroy(server);
    return -1;
  }
  if (rpc_fixed_arena_init(&server->connection_arena, sizeof(rpc_connection),
                           RPC_CONNECTION_ARENA_CAPACITY) != 0) {
    rpc_server_destroy(server);
    return -1;
  }
  if (rpc_routes_init(&server->routes) != 0) {
    rpc_server_destroy(server);
    return -1;
  }
  server->routes_ready = 1;
  if (rpc_scheduler_init(&server->scheduler) != 0) {
    rpc_server_destroy(server);
    return -1;
  }
  *out_server = server;
  return 0;
}

int rpc_server_bind(rpc_server *server, const char *host, const char *port) {
  if (!server || !port || server->listener_count >= RPC_MAX_LISTENERS) {
    return -1;
  }

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo *res = NULL;
  if (getaddrinfo(host, port, &hints, &res) != 0) {
    return -1;
  }

  int ok = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    if (server->listener_count >= RPC_MAX_LISTENERS) {
      break;
    }
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) {
      continue;
    }
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (set_nonblock(fd) != 0 ||
        bind(fd, ai->ai_addr, ai->ai_addrlen) != 0) {
      close(fd);
      continue;
    }

    rpc_listener *listener = &server->listeners[server->listener_count++];
    listener->fd = fd;
    listener->server = server;
    if (server->port == 0) {
      struct sockaddr_storage bound;
      socklen_t bound_len = sizeof(bound);
      if (getsockname(fd, (struct sockaddr *)&bound, &bound_len) == 0) {
        if (bound.ss_family == AF_INET) {
          server->port = ntohs(((struct sockaddr_in *)&bound)->sin_port);
        } else if (bound.ss_family == AF_INET6) {
          server->port = ntohs(((struct sockaddr_in6 *)&bound)->sin6_port);
        }
      }
    }
    ok = 0;
  }

  freeaddrinfo(res);
  return ok;
}

uint16_t rpc_server_port(const rpc_server *server) {
  return server ? server->port : 0;
}

int rpc_server_listen(rpc_server *server) {
  if (!server) {
    return -1;
  }
  for (size_t i = 0; i < server->listener_count; ++i) {
    rpc_listener *listener = &server->listeners[i];
    if (listen(listener->fd, SOMAXCONN) != 0) {
      return -1;
    }
    uintptr_t user = ((uintptr_t)listener) | 1u;
    if (rpc_backend_register(server->backend, listener->fd, RPC_BACKEND_READ,
                             user) != 0) {
      return -1;
    }
  }
  server->listening = 1;
  return 0;
}

int rpc_server_run(rpc_server *server) {
  if (!server || !server->listening) {
    return -1;
  }
  rpc_backend_event events[RPC_EVENT_BATCH];
  while (!atomic_load_explicit(&server->stopping, memory_order_acquire)) {
    int n = rpc_backend_poll(server->backend, events, RPC_EVENT_BATCH, -1);
    if (n < 0) {
      return -1;
    }
    for (int i = 0; i < n; ++i) {
      if (events[i].events & RPC_BACKEND_WAKE) {
        continue;
      }
      if (events[i].user & 1u) {
        rpc_listener *listener = (rpc_listener *)(events[i].user & ~(uintptr_t)1u);
        accept_ready(listener);
      } else {
        rpc_connection *conn = (rpc_connection *)events[i].user;
        if ((events[i].events & RPC_BACKEND_READ) && !conn->closing) {
          connection_read(conn);
        }
        if ((events[i].events & RPC_BACKEND_WRITE) && !conn->closing) {
          connection_write(conn);
        }
        maybe_close(conn);
      }
    }
    rpc_scheduler_run_ready(server->scheduler);
  }
  return 0;
}

void rpc_server_stop(rpc_server *server) {
  if (!server) {
    return;
  }
  atomic_store_explicit(&server->stopping, true, memory_order_release);
  (void)rpc_backend_wake(server->backend);
}

void rpc_server_destroy(rpc_server *server) {
  if (!server) {
    return;
  }
  rpc_connection *conn = server->connections;
  while (conn) {
    rpc_connection *next = conn->next;
    connection_destroy(conn);
    conn = next;
  }
  for (size_t i = 0; i < server->listener_count; ++i) {
    if (server->listeners[i].fd >= 0) {
      (void)rpc_backend_remove(server->backend, server->listeners[i].fd);
      close(server->listeners[i].fd);
    }
  }
  rpc_scheduler_destroy(server->scheduler);
  if (server->routes_ready) {
    rpc_routes_destroy(&server->routes);
  }
  rpc_fixed_arena_destroy(&server->connection_arena);
  rpc_backend_destroy(server->backend);
  free(server);
}

int rpc_server_add_route(rpc_server *server, uint32_t proc_id,
                         rpc_handler_fn handler, void *user_data) {
  return server ? rpc_routes_add(&server->routes, proc_id, handler, user_data)
                : -1;
}

int rpc_server_remove_route(rpc_server *server, uint32_t proc_id) {
  return server ? rpc_routes_remove(&server->routes, proc_id) : -1;
}
