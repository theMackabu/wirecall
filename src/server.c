#include "rpc/server.h"
#include "rpc/trace.h"

#include "arena.h"
#include "backend.h"
#include "memory.h"
#include "proc.h"
#include "routes.h"
#include "scheduler.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RPC_MAX_LISTENERS 8
#define RPC_READ_CHUNK 4096
#define RPC_EVENT_BATCH 1024
#define RPC_CONNECTION_ARENA_CAPACITY 65536u
#define RPC_MAX_WORKERS 128u

typedef struct rpc_worker rpc_worker;

typedef struct rpc_listener {
  int fd;
  rpc_worker *worker;
} rpc_listener;

typedef struct rpc_connection {
  int fd;
  rpc_worker *worker;
  uint8_t *read_buf;
  size_t read_len;
  size_t read_cap;
  uint8_t *write_buf;
  size_t write_len;
  size_t write_cap;
  size_t write_off;
  uint32_t interests;
  int closing;
  int write_queued;
  struct rpc_connection *next;
  struct rpc_connection *write_next;
} rpc_connection;

typedef struct rpc_pending_fd {
  int fd;
  struct rpc_pending_fd *next;
} rpc_pending_fd;

struct rpc_worker {
  rpc_server *server;
  rpc_backend *backend;
  rpc_scheduler *scheduler;
  rpc_fixed_arena connection_arena;
  rpc_listener listeners[RPC_MAX_LISTENERS];
  size_t listener_count;
  rpc_connection *connections;
  rpc_connection *pending_writes;
  pthread_mutex_t pending_mutex;
  rpc_pending_fd *pending_head;
  rpc_pending_fd *pending_tail;
  int pending_mutex_ready;
  pthread_t thread;
  int thread_started;
  uint32_t index;
};

struct rpc_server {
  rpc_routes routes;
  rpc_worker *workers;
  uint32_t worker_count;
  uint32_t workers_ready;
  atomic_uint next_worker;
  atomic_bool stopping;
  int listening;
  int routes_ready;
  uint16_t port;
};

static void connection_write(rpc_connection *conn);
static int worker_add_connection(rpc_worker *worker, int fd);
static void worker_queue_write(rpc_worker *worker, rpc_connection *conn);
static void worker_flush_writes(rpc_worker *worker);

static int connection_set_interests(rpc_connection *conn, uint32_t interests) {
  if (conn->interests == interests) { return 0; }
  if (rpc_backend_modify(conn->worker->backend, conn->fd, interests, (uintptr_t)conn) != 0) { return -1; }
  conn->interests = interests;
  return 0;
}

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) { return -1; }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint32_t cpu_count(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  if (n <= 0) { return 1; }
  if ((unsigned long)n > RPC_MAX_WORKERS) { return RPC_MAX_WORKERS; }
  return (uint32_t)n;
}

static void sockaddr_set_port(struct sockaddr *addr, uint16_t port) {
  if (addr->sa_family == AF_INET) {
    ((struct sockaddr_in *)addr)->sin_port = htons(port);
  } else if (addr->sa_family == AF_INET6) {
    ((struct sockaddr_in6 *)addr)->sin6_port = htons(port);
  }
}

static uint16_t sockaddr_get_port(const struct sockaddr *addr) {
  if (addr->sa_family == AF_INET) { return ntohs(((const struct sockaddr_in *)addr)->sin_port); }
  if (addr->sa_family == AF_INET6) { return ntohs(((const struct sockaddr_in6 *)addr)->sin6_port); }
  return 0;
}

static int append_bytes(uint8_t **buf, size_t *len, size_t *cap, const void *data, size_t data_len) {
  if (data_len == 0) { return 0; }
  if (*len > SIZE_MAX - data_len) { return -1; }
  size_t need = *len + data_len;
  if (need > *cap) {
    size_t next_cap = *cap ? *cap : 4096u;
    while (next_cap < need) {
      next_cap *= 2u;
    }
    uint8_t *next = rpc_mem_realloc(*buf, next_cap);
    if (!next) { return -1; }
    *buf = next;
    *cap = next_cap;
  }
  if (data) { memcpy(*buf + *len, data, data_len); }
  *len += data_len;
  return 0;
}

static int reserve_bytes(uint8_t **buf, size_t *cap, size_t need) {
  if (need <= *cap) { return 0; }
  size_t next_cap = *cap ? *cap : 4096u;
  while (next_cap < need) {
    next_cap *= 2u;
  }
  uint8_t *next = rpc_mem_realloc(*buf, next_cap);
  if (!next) { return -1; }
  *buf = next;
  *cap = next_cap;
  return 0;
}

static int queue_packet(rpc_connection *conn, rpc_op op, uint8_t flags, uint64_t proc_id, uint64_t call_id,
                        const uint8_t *payload, size_t payload_len) {
  if (payload_len > RPC_MAX_PAYLOAD_SIZE) { return -1; }
  uint8_t header_buf[RPC_HEADER_SIZE];
  rpc_header header = {
      .op = op,
      .flags = flags,
      .proc_id = proc_id,
      .size = (uint32_t)payload_len,
      .call_id = call_id,
  };
  if (rpc_header_encode(&header, header_buf) != 0) { return -1; }
  if (append_bytes(&conn->write_buf, &conn->write_len, &conn->write_cap, header_buf, sizeof(header_buf)) != 0) {
    return -1;
  }
  if (append_bytes(&conn->write_buf, &conn->write_len, &conn->write_cap, payload, payload_len) != 0) { return -1; }

  worker_queue_write(conn->worker, conn);
  return 0;
}

static int queue_string_error(rpc_connection *conn, uint64_t proc_id, uint64_t call_id, const char *message) {
  rpc_writer writer;
  rpc_writer_init(&writer);
  int rc = rpc_writer_string(&writer, message, (uint32_t)strlen(message));
  if (rc == 0) { rc = queue_packet(conn, RPC_OP_ERROR, RPC_FLAG_NONE, proc_id, call_id, writer.data, writer.len); }
  rpc_writer_free(&writer);
  return rc;
}

static void connection_destroy(rpc_connection *conn) {
  if (!conn) { return; }
  rpc_worker *worker = conn->worker;
  (void)rpc_backend_remove(worker->backend, conn->fd);
  close(conn->fd);
  rpc_mem_free(conn->read_buf);
  rpc_mem_free(conn->write_buf);

  rpc_connection **link = &worker->connections;
  while (*link && *link != conn) {
    link = &(*link)->next;
  }
  if (*link == conn) { *link = conn->next; }
  if (conn->write_queued) {
    rpc_connection **write_link = &worker->pending_writes;
    while (*write_link && *write_link != conn) {
      write_link = &(*write_link)->write_next;
    }
    if (*write_link == conn) { *write_link = conn->write_next; }
    conn->write_queued = 0;
    conn->write_next = NULL;
  }
  rpc_fixed_arena_free(&worker->connection_arena, conn);
}

static void maybe_close(rpc_connection *conn) {
  if (conn->closing && conn->write_len == conn->write_off) { connection_destroy(conn); }
}

static void worker_queue_write(rpc_worker *worker, rpc_connection *conn) {
  if (conn->write_queued) { return; }
  conn->write_queued = 1;
  conn->write_next = worker->pending_writes;
  worker->pending_writes = conn;
}

static void worker_flush_writes(rpc_worker *worker) {
  rpc_connection *conn = worker->pending_writes;
  worker->pending_writes = NULL;
  while (conn) {
    rpc_connection *next = conn->write_next;
    conn->write_next = NULL;
    conn->write_queued = 0;
    if (conn->write_off < conn->write_len && !conn->closing) {
      connection_write(conn);
    } else if (conn->write_off < conn->write_len) {
      connection_write(conn);
    }
    maybe_close(conn);
    conn = next;
  }
}

static void on_call_done(rpc_call *call, void *user_data) {
  rpc_connection *conn = user_data;
  const rpc_writer *response = rpc_call_response(call);
  if (rpc_call_result(call) == 0) {
    (void)queue_packet(conn, RPC_OP_RESPONSE, RPC_FLAG_NONE, rpc_call_proc_id(call), rpc_call_id(call), response->data,
                       response->len);
  } else {
    (void)queue_string_error(conn, rpc_call_proc_id(call), rpc_call_id(call), rpc_call_error(call));
  }
}

static int handle_sync_call(rpc_connection *conn, const rpc_header *header, rpc_route *route, const uint8_t *payload) {
  rpc_value *args = NULL;
  size_t argc = 0;
  if (rpc_payload_decode(payload, header->size, &args, &argc) != 0) {
    return queue_string_error(conn, header->proc_id, header->call_id, "malformed payload");
  }

  rpc_ctx ctx = {
      .call_id = header->call_id,
      .proc_id = header->proc_id,
  };
  rpc_writer response;
  rpc_writer_init(&response);
  int result = route->handler(&ctx, args, argc, &response, route->user_data);
  int rc = 0;
  if (result == 0) {
    rc = queue_packet(conn, RPC_OP_RESPONSE, RPC_FLAG_NONE, header->proc_id, header->call_id, response.data,
                      response.len);
  } else {
    rc = queue_string_error(conn, header->proc_id, header->call_id, "procedure failed");
  }
  rpc_writer_free(&response);
  rpc_values_free(args);
  return rc;
}

static int handle_packet(rpc_connection *conn, const rpc_header *header, const uint8_t *payload) {
  static const void *dispatch[] = {
      [RPC_OP_RPC] = &&op_rpc,
      [RPC_OP_PING] = &&op_ping,
      [RPC_OP_DISCONNECT] = &&op_disconnect,
      [RPC_OP_RESPONSE] = &&op_unsupported,
      [RPC_OP_ERROR] = &&op_unsupported,
  };

  if ((size_t)header->op >= sizeof(dispatch) / sizeof(*dispatch) || !dispatch[header->op]) { goto op_unsupported; }
  goto *dispatch[header->op];

op_ping:
  return queue_packet(conn, RPC_OP_RESPONSE, RPC_FLAG_NONE, 0, header->call_id, NULL, 0);

op_disconnect:
  conn->closing = 1;
  return 0;

op_rpc: {
  rpc_trace_worker_add(conn->worker->index, RPC_TRACE_WORKER_RPCS, 1);
  rpc_route route;
  uint64_t trace_route = rpc_trace_begin();
  if (rpc_routes_lookup(&conn->worker->server->routes, header->proc_id, &route) != 0) {
    rpc_trace_end(RPC_TRACE_SERVER_ROUTE, trace_route);
    return queue_string_error(conn, header->proc_id, header->call_id, "unknown procedure");
  }
  rpc_trace_end(RPC_TRACE_SERVER_ROUTE, trace_route);

  if (!route.is_async) { return handle_sync_call(conn, header, &route, payload); }

  uint64_t trace_schedule = rpc_trace_begin();
  if (rpc_scheduler_submit(conn->worker->scheduler, header->call_id, header->proc_id, route.handler, route.user_data,
                           payload, header->size, on_call_done, conn) != 0) {
    rpc_trace_end(RPC_TRACE_SERVER_SCHEDULE, trace_schedule);
    return queue_string_error(conn, header->proc_id, header->call_id, "scheduler failure");
  }
  rpc_trace_end(RPC_TRACE_SERVER_SCHEDULE, trace_schedule);
  return 0;
}

op_unsupported:
  return queue_string_error(conn, header->proc_id, header->call_id, "unsupported operation");
}

static void parse_available(rpc_connection *conn) {
  uint64_t trace = rpc_trace_begin();
  size_t off = 0;
  while (conn->read_len - off >= RPC_HEADER_SIZE) {
    rpc_header header;
    if (rpc_header_decode(conn->read_buf + off, &header) != 0) {
      conn->closing = 1;
      break;
    }
    if (conn->read_len - off - RPC_HEADER_SIZE < header.size) { break; }
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
  rpc_trace_end(RPC_TRACE_SERVER_PARSE, trace);
}

static void connection_read(rpc_connection *conn) {
  rpc_trace_worker_add(conn->worker->index, RPC_TRACE_WORKER_READS, 1);
  uint64_t trace = rpc_trace_begin();
  for (;;) {
    if (conn->read_cap - conn->read_len < RPC_READ_CHUNK) {
      if (reserve_bytes(&conn->read_buf, &conn->read_cap, conn->read_len + RPC_READ_CHUNK) != 0) {
        conn->closing = 1;
        rpc_trace_end(RPC_TRACE_SERVER_READ, trace);
        return;
      }
    }
    ssize_t n = recv(conn->fd, conn->read_buf + conn->read_len, conn->read_cap - conn->read_len, 0);
    if (n > 0) {
      conn->read_len += (size_t)n;
      parse_available(conn);
      if (conn->closing) {
        rpc_trace_end(RPC_TRACE_SERVER_READ, trace);
        return;
      }
      continue;
    }
    if (n == 0) {
      conn->closing = 1;
      rpc_trace_end(RPC_TRACE_SERVER_READ, trace);
      return;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
      rpc_trace_end(RPC_TRACE_SERVER_READ, trace);
      return;
    }
    if (errno == EINTR) { continue; }
    conn->closing = 1;
    rpc_trace_end(RPC_TRACE_SERVER_READ, trace);
    return;
  }
}

static void connection_write(rpc_connection *conn) {
  rpc_trace_worker_add(conn->worker->index, RPC_TRACE_WORKER_WRITES, 1);
  uint64_t trace = rpc_trace_begin();
  while (conn->write_off < conn->write_len) {
    ssize_t n = send(conn->fd, conn->write_buf + conn->write_off, conn->write_len - conn->write_off, 0);
    if (n > 0) {
      conn->write_off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) { continue; }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      (void)connection_set_interests(conn, RPC_BACKEND_READ | RPC_BACKEND_WRITE);
      rpc_trace_end(RPC_TRACE_SERVER_WRITE, trace);
      return;
    }
    conn->closing = 1;
    rpc_trace_end(RPC_TRACE_SERVER_WRITE, trace);
    return;
  }

  conn->write_len = 0;
  conn->write_off = 0;
  (void)connection_set_interests(conn, RPC_BACKEND_READ);
  rpc_trace_end(RPC_TRACE_SERVER_WRITE, trace);
}

static int worker_add_connection(rpc_worker *worker, int fd) {
  rpc_connection *conn = rpc_fixed_arena_alloc(&worker->connection_arena);
  if (!conn) {
    close(fd);
    return -1;
  }
  conn->fd = fd;
  conn->worker = worker;
  conn->next = worker->connections;
  worker->connections = conn;
  if (rpc_backend_register(worker->backend, fd, RPC_BACKEND_READ, (uintptr_t)conn) != 0) {
    connection_destroy(conn);
    return -1;
  }
  conn->interests = RPC_BACKEND_READ;
  rpc_trace_worker_add(worker->index, RPC_TRACE_WORKER_ACCEPTS, 1);
  return 0;
}

static void worker_enqueue_connection(rpc_worker *worker, int fd) {
  rpc_pending_fd *pending = rpc_mem_alloc(sizeof(*pending));
  if (!pending) {
    close(fd);
    return;
  }
  pending->fd = fd;
  pending->next = NULL;

  pthread_mutex_lock(&worker->pending_mutex);
  if (worker->pending_tail) {
    worker->pending_tail->next = pending;
  } else {
    worker->pending_head = pending;
  }
  worker->pending_tail = pending;
  pthread_mutex_unlock(&worker->pending_mutex);

  (void)rpc_backend_wake(worker->backend);
}

static void worker_drain_pending(rpc_worker *worker) {
  pthread_mutex_lock(&worker->pending_mutex);
  rpc_pending_fd *pending = worker->pending_head;
  worker->pending_head = NULL;
  worker->pending_tail = NULL;
  pthread_mutex_unlock(&worker->pending_mutex);

  while (pending) {
    rpc_pending_fd *next = pending->next;
    (void)worker_add_connection(worker, pending->fd);
    rpc_mem_free(pending);
    pending = next;
  }
}

static void accept_ready(rpc_listener *listener) {
  uint64_t trace = rpc_trace_begin();
  rpc_worker *worker = listener->worker;
  rpc_server *server = worker->server;
  for (;;) {
    struct sockaddr_storage addr;
    socklen_t addr_len = sizeof(addr);
    int fd = accept(listener->fd, (struct sockaddr *)&addr, &addr_len);
    if (fd < 0) {
      if (errno == EINTR) { continue; }
      rpc_trace_end(RPC_TRACE_SERVER_ACCEPT, trace);
      return;
    }
    if (set_nonblock(fd) != 0) {
      close(fd);
      continue;
    }
    uint32_t idx = atomic_fetch_add_explicit(&server->next_worker, 1, memory_order_relaxed) % server->worker_count;
    worker_enqueue_connection(&server->workers[idx], fd);
  }
}

static int worker_init(rpc_server *server, rpc_worker *worker, uint32_t index) {
  memset(worker, 0, sizeof(*worker));
  worker->server = server;
  worker->index = index;
  for (size_t i = 0; i < RPC_MAX_LISTENERS; ++i) {
    worker->listeners[i].fd = -1;
  }
  if (pthread_mutex_init(&worker->pending_mutex, NULL) != 0) { return -1; }
  worker->pending_mutex_ready = 1;
  if (rpc_backend_kqueue_create(&worker->backend) != 0) { return -1; }
  if (rpc_fixed_arena_init(&worker->connection_arena, sizeof(rpc_connection), RPC_CONNECTION_ARENA_CAPACITY) != 0) {
    return -1;
  }
  if (rpc_scheduler_init(&worker->scheduler) != 0) { return -1; }
  return 0;
}

static void worker_destroy(rpc_worker *worker) {
  if (!worker) { return; }
  rpc_connection *conn = worker->connections;
  while (conn) {
    rpc_connection *next = conn->next;
    connection_destroy(conn);
    conn = next;
  }
  for (size_t i = 0; i < worker->listener_count; ++i) {
    if (worker->listeners[i].fd >= 0) {
      (void)rpc_backend_remove(worker->backend, worker->listeners[i].fd);
      close(worker->listeners[i].fd);
      worker->listeners[i].fd = -1;
    }
  }
  if (worker->pending_mutex_ready) {
    pthread_mutex_lock(&worker->pending_mutex);
    rpc_pending_fd *pending = worker->pending_head;
    worker->pending_head = NULL;
    worker->pending_tail = NULL;
    pthread_mutex_unlock(&worker->pending_mutex);
    while (pending) {
      rpc_pending_fd *next = pending->next;
      close(pending->fd);
      rpc_mem_free(pending);
      pending = next;
    }
    pthread_mutex_destroy(&worker->pending_mutex);
  }
  rpc_scheduler_destroy(worker->scheduler);
  rpc_fixed_arena_destroy(&worker->connection_arena);
  rpc_backend_destroy(worker->backend);
  memset(worker, 0, sizeof(*worker));
}

static int server_ensure_workers(rpc_server *server) {
  if (server->workers_ready) { return 0; }
  if (server->worker_count == 0) { server->worker_count = cpu_count(); }
  server->workers = rpc_mem_calloc(server->worker_count, sizeof(*server->workers));
  if (!server->workers) { return -1; }
  for (uint32_t i = 0; i < server->worker_count; ++i) {
    if (worker_init(server, &server->workers[i], i) != 0) {
      for (uint32_t j = 0; j <= i; ++j) {
        worker_destroy(&server->workers[j]);
      }
      rpc_mem_free(server->workers);
      server->workers = NULL;
      return -1;
    }
  }
  server->workers_ready = 1;
  return 0;
}

int rpc_server_init(rpc_server **out_server) {
  if (!out_server) { return -1; }
  rpc_server *server = rpc_mem_calloc(1, sizeof(*server));
  if (!server) { return -1; }
  atomic_init(&server->stopping, false);
  atomic_init(&server->next_worker, 0);
  server->worker_count = cpu_count();
  if (rpc_routes_init(&server->routes) != 0) {
    rpc_server_destroy(server);
    return -1;
  }
  server->routes_ready = 1;
  *out_server = server;
  return 0;
}

int rpc_server_set_workers(rpc_server *server, uint32_t worker_count) {
  if (!server || server->workers_ready || server->listening || worker_count == 0 || worker_count > RPC_MAX_WORKERS) {
    return -1;
  }
  server->worker_count = worker_count;
  return 0;
}

int rpc_server_bind(rpc_server *server, const char *host, const char *port) {
  if (!server || !port || server_ensure_workers(server) != 0) { return -1; }

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  struct addrinfo *res = NULL;
  if (getaddrinfo(host, port, &hints, &res) != 0) { return -1; }

  int ok = -1;
  for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
    if (server->workers[0].listener_count >= RPC_MAX_LISTENERS) { break; }

    rpc_worker *worker = &server->workers[0];
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) { continue; }
    int yes = 1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_storage addr;
    memcpy(&addr, ai->ai_addr, ai->ai_addrlen);
    if (server->port != 0) { sockaddr_set_port((struct sockaddr *)&addr, server->port); }

    if (set_nonblock(fd) != 0 || bind(fd, (struct sockaddr *)&addr, ai->ai_addrlen) != 0) {
      close(fd);
      continue;
    }

    if (server->port == 0) {
      struct sockaddr_storage bound;
      socklen_t bound_len = sizeof(bound);
      if (getsockname(fd, (struct sockaddr *)&bound, &bound_len) == 0) {
        server->port = sockaddr_get_port((struct sockaddr *)&bound);
      }
    }

    rpc_listener *listener = &worker->listeners[worker->listener_count++];
    listener->fd = fd;
    listener->worker = worker;
    ok = 0;
  }

  freeaddrinfo(res);
  return ok;
}

uint16_t rpc_server_port(const rpc_server *server) {
  return server ? server->port : 0;
}

int rpc_server_listen(rpc_server *server) {
  if (!server) { return -1; }
  for (uint32_t wi = 0; wi < server->worker_count; ++wi) {
    rpc_worker *worker = &server->workers[wi];
    for (size_t i = 0; i < worker->listener_count; ++i) {
      rpc_listener *listener = &worker->listeners[i];
      if (listen(listener->fd, SOMAXCONN) != 0) { return -1; }
      uintptr_t user = ((uintptr_t)listener) | 1u;
      if (rpc_backend_register(worker->backend, listener->fd, RPC_BACKEND_READ, user) != 0) { return -1; }
    }
  }
  server->listening = 1;
  return 0;
}

static void *worker_run_main(void *arg);

static int worker_run(rpc_worker *worker) {
  rpc_server *server = worker->server;
  rpc_backend_event events[RPC_EVENT_BATCH];
  while (!atomic_load_explicit(&server->stopping, memory_order_acquire)) {
    uint64_t trace_poll = rpc_trace_begin();
    int n = rpc_backend_poll(worker->backend, events, RPC_EVENT_BATCH, -1);
    rpc_trace_end(RPC_TRACE_SERVER_POLL_WAIT, trace_poll);
    if (n < 0) { return -1; }
    rpc_trace_add(RPC_TRACE_SERVER_POLL_EVENTS, (uint64_t)n);
    rpc_trace_worker_add(worker->index, RPC_TRACE_WORKER_POLL_EVENTS, (uint64_t)n);
    uint64_t trace_active = rpc_trace_begin();
    for (int i = 0; i < n; ++i) {
      if (events[i].events & RPC_BACKEND_WAKE) {
        worker_drain_pending(worker);
        continue;
      }
      if (events[i].user & 1u) {
        rpc_listener *listener = (rpc_listener *)(events[i].user & ~(uintptr_t)1u);
        accept_ready(listener);
      } else {
        rpc_connection *conn = (rpc_connection *)events[i].user;
        if ((events[i].events & RPC_BACKEND_READ) && !conn->closing) { connection_read(conn); }
        if ((events[i].events & RPC_BACKEND_WRITE) && !conn->closing) { connection_write(conn); }
        maybe_close(conn);
      }
    }
    uint64_t trace_schedule = rpc_trace_begin();
    rpc_scheduler_run_ready(worker->scheduler);
    rpc_trace_end(RPC_TRACE_SERVER_SCHEDULE, trace_schedule);
    worker_flush_writes(worker);
    rpc_trace_worker_end(worker->index, RPC_TRACE_WORKER_ACTIVE, trace_active);
    rpc_trace_end(RPC_TRACE_SERVER_LOOP_ACTIVE, trace_active);
  }
  return 0;
}

static void *worker_run_main(void *arg) {
  (void)worker_run(arg);
  return NULL;
}

int rpc_server_run(rpc_server *server) {
  if (!server || !server->listening) { return -1; }

  if (server->worker_count == 1) { return worker_run(&server->workers[0]); }

  for (uint32_t i = 0; i < server->worker_count; ++i) {
    if (pthread_create(&server->workers[i].thread, NULL, worker_run_main, &server->workers[i]) != 0) {
      rpc_server_stop(server);
      for (uint32_t j = 0; j < i; ++j) {
        pthread_join(server->workers[j].thread, NULL);
        server->workers[j].thread_started = 0;
      }
      return -1;
    }
    server->workers[i].thread_started = 1;
  }

  for (uint32_t i = 0; i < server->worker_count; ++i) {
    pthread_join(server->workers[i].thread, NULL);
    server->workers[i].thread_started = 0;
  }
  return 0;
}

void rpc_server_stop(rpc_server *server) {
  if (!server) { return; }
  atomic_store_explicit(&server->stopping, true, memory_order_release);
  for (uint32_t i = 0; i < server->worker_count; ++i) {
    if (server->workers && server->workers[i].backend) { (void)rpc_backend_wake(server->workers[i].backend); }
  }
}

void rpc_server_destroy(rpc_server *server) {
  if (!server) { return; }
  if (server->workers) {
    rpc_server_stop(server);
    for (uint32_t i = 0; i < server->worker_count; ++i) {
      if (server->workers[i].thread_started) {
        pthread_join(server->workers[i].thread, NULL);
        server->workers[i].thread_started = 0;
      }
      worker_destroy(&server->workers[i]);
    }
    rpc_mem_free(server->workers);
  }
  if (server->routes_ready) { rpc_routes_destroy(&server->routes); }
  rpc_mem_free(server);
}

static int server_add_route_id(rpc_server *server, uint64_t proc_id, rpc_handler_fn handler, void *user_data,
                               rpc_route_finalizer_fn finalizer) {
  return server ? rpc_routes_add_ex(&server->routes, proc_id, handler, user_data, finalizer, 0) : -1;
}

int rpc_server_add_route_name(rpc_server *server, const char *proc_name, rpc_handler_fn handler, void *user_data) {
  return rpc_server_add_route_name_ex(server, proc_name, handler, user_data, NULL);
}

int rpc_server_add_route_name_ex(rpc_server *server, const char *proc_name, rpc_handler_fn handler, void *user_data,
                                 rpc_route_finalizer_fn finalizer) {
  return server_add_route_id(server, rpc_proc_id(proc_name), handler, user_data, finalizer);
}

static int server_add_async_route_id(rpc_server *server, uint64_t proc_id, rpc_handler_fn handler, void *user_data,
                                     rpc_route_finalizer_fn finalizer) {
  return server ? rpc_routes_add_ex(&server->routes, proc_id, handler, user_data, finalizer, 1) : -1;
}

int rpc_server_add_async_route_name(rpc_server *server, const char *proc_name, rpc_handler_fn handler,
                                    void *user_data) {
  return rpc_server_add_async_route_name_ex(server, proc_name, handler, user_data, NULL);
}

int rpc_server_add_async_route_name_ex(rpc_server *server, const char *proc_name, rpc_handler_fn handler,
                                       void *user_data, rpc_route_finalizer_fn finalizer) {
  return server_add_async_route_id(server, rpc_proc_id(proc_name), handler, user_data, finalizer);
}

static int server_remove_route_id(rpc_server *server, uint64_t proc_id) {
  return server ? rpc_routes_remove(&server->routes, proc_id) : -1;
}

int rpc_server_remove_route_name(rpc_server *server, const char *proc_name) {
  return server_remove_route_id(server, rpc_proc_id(proc_name));
}
