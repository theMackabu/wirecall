#ifndef WIRECALL_BACKEND_H
#define WIRECALL_BACKEND_H

#include <stdint.h>

enum {
  WIRECALL_BACKEND_READ = 1u << 0u,
  WIRECALL_BACKEND_WRITE = 1u << 1u,
  WIRECALL_BACKEND_WAKE = 1u << 2u,
};

typedef struct wirecall_backend_event {
  int fd;
  uint32_t events;
  uintptr_t user;
} wirecall_backend_event;

typedef struct wirecall_backend wirecall_backend;

int wirecall_backend_create(wirecall_backend **out);
void wirecall_backend_destroy(wirecall_backend *backend);
int wirecall_backend_register(wirecall_backend *backend, int fd, uint32_t events, uintptr_t user);
int wirecall_backend_modify(wirecall_backend *backend, int fd, uint32_t events, uintptr_t user);
int wirecall_backend_remove(wirecall_backend *backend, int fd);
int wirecall_backend_wake(wirecall_backend *backend);
int wirecall_backend_poll(wirecall_backend *backend, wirecall_backend_event *events, int max_events, int timeout_ms);

#endif
