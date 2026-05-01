#if defined(_WIN32)

#include "backend.h"
#include "memory.h"

#define WIN32_LEAN_AND_MEAN
#include <string.h>
#include <windows.h>
#include <winsock2.h>

#define WIRECALL_BACKEND_NAME "windows"
#define WIRECALL_WINDOWS_MAX_WATCHES 1024u

typedef struct wirecall_backend_watch {
  SOCKET socket;
  WSAEVENT event;
  uint32_t interests;
  uintptr_t user;
  int active;
} wirecall_backend_watch;

struct wirecall_backend {
  WSAEVENT wake_event;
  wirecall_backend_watch watches[WIRECALL_WINDOWS_MAX_WATCHES];
  WSADATA wsa;
  int wsa_ready;
};

static long network_events(uint32_t interests) {
  long events = FD_CLOSE;
  if (interests & WIRECALL_BACKEND_READ) { events |= FD_ACCEPT | FD_READ; }
  if (interests & WIRECALL_BACKEND_WRITE) { events |= FD_WRITE; }
  return events;
}

static wirecall_backend_watch *find_watch(wirecall_backend *backend, int fd) {
  SOCKET socket = (SOCKET)(uintptr_t)fd;
  for (size_t i = 0; i < WIRECALL_WINDOWS_MAX_WATCHES; ++i) {
    if (backend->watches[i].active && backend->watches[i].socket == socket) { return &backend->watches[i]; }
  }
  return NULL;
}

static wirecall_backend_watch *alloc_watch(wirecall_backend *backend) {
  for (size_t i = 0; i < WIRECALL_WINDOWS_MAX_WATCHES; ++i) {
    if (!backend->watches[i].active) { return &backend->watches[i]; }
  }
  return NULL;
}

int wirecall_backend_create(wirecall_backend **out) {
  if (!out) { return -1; }
  wirecall_backend *backend = wirecall_mem_calloc(1, sizeof(*backend));
  if (!backend) { return -1; }
  if (WSAStartup(MAKEWORD(2, 2), &backend->wsa) != 0) {
    wirecall_mem_free(backend);
    return -1;
  }
  backend->wsa_ready = 1;
  backend->wake_event = WSACreateEvent();
  if (backend->wake_event == WSA_INVALID_EVENT) {
    wirecall_backend_destroy(backend);
    return -1;
  }
  *out = backend;
  return 0;
}

void wirecall_backend_destroy(wirecall_backend *backend) {
  if (!backend) { return; }
  for (size_t i = 0; i < WIRECALL_WINDOWS_MAX_WATCHES; ++i) {
    if (backend->watches[i].active) {
      (void)WSAEventSelect(backend->watches[i].socket, NULL, 0);
      WSACloseEvent(backend->watches[i].event);
    }
  }
  if (backend->wake_event != WSA_INVALID_EVENT) { WSACloseEvent(backend->wake_event); }
  if (backend->wsa_ready) { WSACleanup(); }
  wirecall_mem_free(backend);
}

int wirecall_backend_register(wirecall_backend *backend, int fd, uint32_t events, uintptr_t user) {
  if (!backend || fd < 0) { return -1; }
  wirecall_backend_watch *watch = find_watch(backend, fd);
  if (!watch) {
    watch = alloc_watch(backend);
    if (!watch) { return -1; }
    watch->event = WSACreateEvent();
    if (watch->event == WSA_INVALID_EVENT) { return -1; }
    watch->socket = (SOCKET)(uintptr_t)fd;
    watch->active = 1;
  }
  watch->interests = events;
  watch->user = user;
  if (WSAEventSelect(watch->socket, watch->event, network_events(events)) != 0) { return -1; }
  return 0;
}

int wirecall_backend_modify(wirecall_backend *backend, int fd, uint32_t events, uintptr_t user) {
  return wirecall_backend_register(backend, fd, events, user);
}

int wirecall_backend_remove(wirecall_backend *backend, int fd) {
  if (!backend || fd < 0) { return -1; }
  wirecall_backend_watch *watch = find_watch(backend, fd);
  if (!watch) { return 0; }
  (void)WSAEventSelect(watch->socket, NULL, 0);
  WSACloseEvent(watch->event);
  memset(watch, 0, sizeof(*watch));
  return 0;
}

int wirecall_backend_wake(wirecall_backend *backend) {
  return backend && WSASetEvent(backend->wake_event) ? 0 : -1;
}

int wirecall_backend_poll(wirecall_backend *backend, wirecall_backend_event *events, int max_events, int timeout_ms) {
  if (!backend || !events || max_events <= 0) { return -1; }

  WSAEVENT handles[MAXIMUM_WAIT_OBJECTS];
  wirecall_backend_watch *watch_for_handle[MAXIMUM_WAIT_OBJECTS];
  DWORD count = 1;
  handles[0] = backend->wake_event;
  watch_for_handle[0] = NULL;

  for (size_t i = 0; i < WIRECALL_WINDOWS_MAX_WATCHES && count < MAXIMUM_WAIT_OBJECTS; ++i) {
    if (!backend->watches[i].active) { continue; }
    handles[count] = backend->watches[i].event;
    watch_for_handle[count] = &backend->watches[i];
    count++;
  }

  DWORD timeout = timeout_ms < 0 ? WSA_INFINITE : (DWORD)timeout_ms;
  DWORD index = WSAWaitForMultipleEvents(count, handles, FALSE, timeout, FALSE);
  if (index == WSA_WAIT_TIMEOUT) { return 0; }
  if (index == WSA_WAIT_FAILED) { return -1; }

  int out = 0;
  DWORD start = index - WSA_WAIT_EVENT_0;
  for (DWORD i = start; i < count && out < max_events; ++i) {
    if (WSAWaitForMultipleEvents(1, &handles[i], TRUE, 0, FALSE) != WSA_WAIT_EVENT_0) { continue; }
    if (i == 0) {
      WSAResetEvent(backend->wake_event);
      events[out++] = (wirecall_backend_event){.fd = -1, .events = WIRECALL_BACKEND_WAKE, .user = 0};
      continue;
    }

    wirecall_backend_watch *watch = watch_for_handle[i];
    WSANETWORKEVENTS nevents;
    if (WSAEnumNetworkEvents(watch->socket, watch->event, &nevents) != 0) { continue; }
    uint32_t ev = 0;
    if (nevents.lNetworkEvents & (FD_ACCEPT | FD_READ | FD_CLOSE)) { ev |= WIRECALL_BACKEND_READ; }
    if (nevents.lNetworkEvents & FD_WRITE) { ev |= WIRECALL_BACKEND_WRITE; }
    if (ev != 0) {
      events[out++] = (wirecall_backend_event){.fd = (int)(uintptr_t)watch->socket, .events = ev, .user = watch->user};
    }
  }
  return out;
}

#endif
