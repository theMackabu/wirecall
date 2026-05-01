#ifndef RPC_BACKEND_H
#define RPC_BACKEND_H

#include <stdint.h>

enum {
  RPC_BACKEND_READ = 1u << 0u,
  RPC_BACKEND_WRITE = 1u << 1u,
  RPC_BACKEND_WAKE = 1u << 2u,
};

typedef struct rpc_backend_event {
  int fd;
  uint32_t events;
  uintptr_t user;
} rpc_backend_event;

typedef struct rpc_backend rpc_backend;

int rpc_backend_kqueue_create(rpc_backend **out);
void rpc_backend_destroy(rpc_backend *backend);
int rpc_backend_register(rpc_backend *backend, int fd, uint32_t events,
                         uintptr_t user);
int rpc_backend_modify(rpc_backend *backend, int fd, uint32_t events,
                       uintptr_t user);
int rpc_backend_remove(rpc_backend *backend, int fd);
int rpc_backend_wake(rpc_backend *backend);
int rpc_backend_poll(rpc_backend *backend, rpc_backend_event *events, int max_events,
                     int timeout_ms);

#endif
