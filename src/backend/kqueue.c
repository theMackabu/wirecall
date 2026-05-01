#include "backend.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#define RPC_KQUEUE_MAX_EVENTS 1024

struct rpc_backend {
  int kq;
};

static int apply_filter(rpc_backend *backend, int fd, int16_t filter, uint16_t flags, uintptr_t user) {
  struct kevent change;
  EV_SET(&change, (uintptr_t)fd, filter, flags, 0, 0, (void *)user);
  int rc = kevent(backend->kq, &change, 1, NULL, 0, NULL);
  if (rc != 0 && errno == ENOENT && (flags & EV_DELETE)) { return 0; }
  return rc;
}

static int set_interest(rpc_backend *backend, int fd, uint32_t events, uintptr_t user) {
  int rc = 0;
  if (events & RPC_BACKEND_READ) {
    rc |= apply_filter(backend, fd, EVFILT_READ, EV_ADD | EV_ENABLE, user);
  } else {
    rc |= apply_filter(backend, fd, EVFILT_READ, EV_DELETE, user);
  }
  if (events & RPC_BACKEND_WRITE) {
    rc |= apply_filter(backend, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, user);
  } else {
    rc |= apply_filter(backend, fd, EVFILT_WRITE, EV_DELETE, user);
  }
  return rc == 0 ? 0 : -1;
}

int rpc_backend_kqueue_create(rpc_backend **out) {
  if (!out) { return -1; }
  rpc_backend *backend = calloc(1, sizeof(*backend));
  if (!backend) { return -1; }
  backend->kq = kqueue();
  if (backend->kq < 0) {
    free(backend);
    return -1;
  }

  struct kevent wake;
  EV_SET(&wake, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
  if (kevent(backend->kq, &wake, 1, NULL, 0, NULL) != 0) {
    close(backend->kq);
    free(backend);
    return -1;
  }

  *out = backend;
  return 0;
}

void rpc_backend_destroy(rpc_backend *backend) {
  if (backend) {
    if (backend->kq >= 0) { close(backend->kq); }
    free(backend);
  }
}

int rpc_backend_register(rpc_backend *backend, int fd, uint32_t events, uintptr_t user) {
  if (!backend || fd < 0) { return -1; }
  return set_interest(backend, fd, events, user);
}

int rpc_backend_modify(rpc_backend *backend, int fd, uint32_t events, uintptr_t user) {
  return rpc_backend_register(backend, fd, events, user);
}

int rpc_backend_remove(rpc_backend *backend, int fd) {
  if (!backend || fd < 0) { return -1; }
  (void)apply_filter(backend, fd, EVFILT_READ, EV_DELETE, 0);
  (void)apply_filter(backend, fd, EVFILT_WRITE, EV_DELETE, 0);
  return 0;
}

int rpc_backend_wake(rpc_backend *backend) {
  if (!backend) { return -1; }
  struct kevent wake;
  EV_SET(&wake, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
  return kevent(backend->kq, &wake, 1, NULL, 0, NULL);
}

int rpc_backend_poll(rpc_backend *backend, rpc_backend_event *events, int max_events, int timeout_ms) {
  if (!backend || !events || max_events <= 0) { return -1; }

  struct timespec timeout;
  struct timespec *timeout_ptr = NULL;
  if (timeout_ms >= 0) {
    timeout.tv_sec = timeout_ms / 1000;
    timeout.tv_nsec = (timeout_ms % 1000) * 1000000L;
    timeout_ptr = &timeout;
  }

  struct kevent kev[RPC_KQUEUE_MAX_EVENTS];
  int limit = max_events < RPC_KQUEUE_MAX_EVENTS ? max_events : RPC_KQUEUE_MAX_EVENTS;
  int n = kevent(backend->kq, NULL, 0, kev, limit, timeout_ptr);
  if (n < 0) {
    if (errno == EINTR) { return 0; }
    return -1;
  }

  for (int i = 0; i < n; ++i) {
    events[i].fd = (int)kev[i].ident;
    events[i].events = 0;
    events[i].user = (uintptr_t)kev[i].udata;
    if (kev[i].filter == EVFILT_USER) { events[i].events |= RPC_BACKEND_WAKE; }
    if (kev[i].filter == EVFILT_READ) { events[i].events |= RPC_BACKEND_READ; }
    if (kev[i].filter == EVFILT_WRITE) { events[i].events |= RPC_BACKEND_WRITE; }
  }
  return n;
}
