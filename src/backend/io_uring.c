#if defined(__linux__)

#include "memory.h"
#include "backend.h"

#include <errno.h>
#include <linux/io_uring.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define RPC_BACKEND_NAME "io_uring"
#define RPC_URING_ENTRIES 1024u
#ifndef MAP_POPULATE
#define MAP_POPULATE 0
#endif
#ifndef POLLRDHUP
#define POLLRDHUP 0x2000
#endif

typedef struct rpc_backend_watch {
  int fd;
  uint32_t events;
  uintptr_t user;
  int active;
  int armed;
  struct rpc_backend_watch *next;
} rpc_backend_watch;

struct rpc_backend {
  int ring_fd;
  int wake_fd;
  struct io_uring_params params;
  struct io_uring_sqe *sqes;
  void *sq_ptr;
  size_t sq_map_sz;
  void *cq_ptr;
  size_t cq_map_sz;
  unsigned *sq_head;
  unsigned *sq_tail;
  unsigned *sq_ring_mask;
  unsigned *sq_ring_entries;
  unsigned *sq_flags;
  unsigned *sq_array;
  unsigned *cq_head;
  unsigned *cq_tail;
  unsigned *cq_ring_mask;
  struct io_uring_cqe *cqes;
  rpc_backend_watch *watches;
  rpc_backend_watch wake_watch;
};

static int uring_setup(unsigned entries, struct io_uring_params *params) {
  return (int)syscall(__NR_io_uring_setup, entries, params);
}

static int uring_enter(int fd, unsigned to_submit, unsigned min_complete, unsigned flags) {
  return (int)syscall(__NR_io_uring_enter, fd, to_submit, min_complete, flags, NULL, 0);
}

static rpc_backend_watch *find_watch(rpc_backend *backend, int fd) {
  for (rpc_backend_watch *watch = backend->watches; watch; watch = watch->next) {
    if (watch->fd == fd) return watch;
  }
  return NULL;
}

static unsigned poll_mask(uint32_t events) {
  unsigned mask = 0;
  if (events & RPC_BACKEND_READ) { mask |= POLLIN | POLLRDHUP; }
  if (events & RPC_BACKEND_WRITE) { mask |= POLLOUT; }
  return mask;
}

static int sqe_push(rpc_backend *backend, struct io_uring_sqe **out_sqe) {
  unsigned head = __atomic_load_n(backend->sq_head, __ATOMIC_ACQUIRE);
  unsigned tail = *backend->sq_tail;
  if (tail - head >= *backend->sq_ring_entries) {
    if (uring_enter(backend->ring_fd, 0, 0, IORING_ENTER_GETEVENTS) < 0 && errno != EINTR) return -1;
    head = __atomic_load_n(backend->sq_head, __ATOMIC_ACQUIRE);
    if (tail - head >= *backend->sq_ring_entries) return -1;
  }

  unsigned index = tail & *backend->sq_ring_mask;
  struct io_uring_sqe *sqe = &backend->sqes[index];
  memset(sqe, 0, sizeof(*sqe));
  backend->sq_array[index] = index;
  *backend->sq_tail = tail + 1u;
  *out_sqe = sqe;
  return 0;
}

static int submit_pending(rpc_backend *backend) {
  unsigned head = __atomic_load_n(backend->sq_head, __ATOMIC_ACQUIRE);
  unsigned tail = *backend->sq_tail;
  unsigned pending = tail - head;
  if (pending == 0) return 0;
  return uring_enter(backend->ring_fd, pending, 0, 0) < 0 ? -1 : 0;
}

static int arm_watch(rpc_backend *backend, rpc_backend_watch *watch) {
  if (!watch->active || watch->armed || watch->events == 0) return 0;
  struct io_uring_sqe *sqe = NULL;
  if (sqe_push(backend, &sqe) != 0) return -1;
  sqe->opcode = IORING_OP_POLL_ADD;
  sqe->fd = watch->fd;
  sqe->poll_events = (__u16)poll_mask(watch->events);
  sqe->user_data = (uint64_t)(uintptr_t)watch;
  watch->armed = 1;
  return submit_pending(backend);
}

static void cancel_watch(rpc_backend *backend, rpc_backend_watch *watch) {
  if (!watch->armed) return;
  struct io_uring_sqe *sqe = NULL;
  if (sqe_push(backend, &sqe) != 0) return;
  sqe->opcode = IORING_OP_POLL_REMOVE;
  sqe->addr = (uint64_t)(uintptr_t)watch;
  sqe->user_data = 0;
  (void)submit_pending(backend);
  watch->armed = 0;
}

static int map_rings(rpc_backend *backend) {
  struct io_uring_params *p = &backend->params;
  backend->sq_map_sz = p->sq_off.array + p->sq_entries * sizeof(unsigned);
  backend->cq_map_sz = p->cq_off.cqes + p->cq_entries * sizeof(struct io_uring_cqe);
  if (p->features & IORING_FEAT_SINGLE_MMAP && backend->cq_map_sz > backend->sq_map_sz) {
    backend->sq_map_sz = backend->cq_map_sz;
  }

  backend->sq_ptr = mmap(NULL, backend->sq_map_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                         backend->ring_fd, IORING_OFF_SQ_RING);
  if (backend->sq_ptr == MAP_FAILED) return -1;

  if (p->features & IORING_FEAT_SINGLE_MMAP) {
    backend->cq_ptr = backend->sq_ptr;
  } else {
    backend->cq_ptr = mmap(NULL, backend->cq_map_sz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE,
                           backend->ring_fd, IORING_OFF_CQ_RING);
    if (backend->cq_ptr == MAP_FAILED) return -1;
  }

  backend->sqes = mmap(NULL, p->sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_POPULATE, backend->ring_fd, IORING_OFF_SQES);
  if (backend->sqes == MAP_FAILED) return -1;

  backend->sq_head = (unsigned *)((uint8_t *)backend->sq_ptr + p->sq_off.head);
  backend->sq_tail = (unsigned *)((uint8_t *)backend->sq_ptr + p->sq_off.tail);
  backend->sq_ring_mask = (unsigned *)((uint8_t *)backend->sq_ptr + p->sq_off.ring_mask);
  backend->sq_ring_entries = (unsigned *)((uint8_t *)backend->sq_ptr + p->sq_off.ring_entries);
  backend->sq_flags = (unsigned *)((uint8_t *)backend->sq_ptr + p->sq_off.flags);
  backend->sq_array = (unsigned *)((uint8_t *)backend->sq_ptr + p->sq_off.array);
  backend->cq_head = (unsigned *)((uint8_t *)backend->cq_ptr + p->cq_off.head);
  backend->cq_tail = (unsigned *)((uint8_t *)backend->cq_ptr + p->cq_off.tail);
  backend->cq_ring_mask = (unsigned *)((uint8_t *)backend->cq_ptr + p->cq_off.ring_mask);
  backend->cqes = (struct io_uring_cqe *)((uint8_t *)backend->cq_ptr + p->cq_off.cqes);
  return 0;
}

int rpc_backend_create(rpc_backend **out) {
  if (!out) return -1;
  rpc_backend *backend = rpc_mem_calloc(1, sizeof(*backend));
  if (!backend) return -1;
  backend->ring_fd = -1;
  backend->wake_fd = -1;

  backend->ring_fd = uring_setup(RPC_URING_ENTRIES, &backend->params);
  if (backend->ring_fd < 0 || map_rings(backend) != 0) {
    rpc_backend_destroy(backend);
    return -1;
  }

  backend->wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (backend->wake_fd < 0) {
    rpc_backend_destroy(backend);
    return -1;
  }

  backend->wake_watch = (rpc_backend_watch){
      .fd = backend->wake_fd,
      .events = RPC_BACKEND_READ,
      .active = 1,
  };
  if (arm_watch(backend, &backend->wake_watch) != 0) {
    rpc_backend_destroy(backend);
    return -1;
  }

  *out = backend;
  return 0;
}

void rpc_backend_destroy(rpc_backend *backend) {
  if (!backend) return;
  for (rpc_backend_watch *watch = backend->watches; watch;) {
    rpc_backend_watch *next = watch->next;
    rpc_mem_free(watch);
    watch = next;
  }
  if (backend->wake_fd >= 0) close(backend->wake_fd);
  if (backend->sqes && backend->sqes != MAP_FAILED) {
    munmap(backend->sqes, backend->params.sq_entries * sizeof(struct io_uring_sqe));
  }
  if (backend->cq_ptr && backend->cq_ptr != MAP_FAILED && backend->cq_ptr != backend->sq_ptr) {
    munmap(backend->cq_ptr, backend->cq_map_sz);
  }
  if (backend->sq_ptr && backend->sq_ptr != MAP_FAILED) munmap(backend->sq_ptr, backend->sq_map_sz);
  if (backend->ring_fd >= 0) close(backend->ring_fd);
  rpc_mem_free(backend);
}

int rpc_backend_register(rpc_backend *backend, int fd, uint32_t events, uintptr_t user) {
  if (!backend || fd < 0) return -1;
  rpc_backend_watch *watch = find_watch(backend, fd);
  if (!watch) {
    watch = rpc_mem_calloc(1, sizeof(*watch));
    if (!watch) return -1;
    watch->fd = fd;
    watch->next = backend->watches;
    backend->watches = watch;
  } else {
    cancel_watch(backend, watch);
  }
  watch->events = events;
  watch->user = user;
  watch->active = 1;
  return arm_watch(backend, watch);
}

int rpc_backend_modify(rpc_backend *backend, int fd, uint32_t events, uintptr_t user) {
  return rpc_backend_register(backend, fd, events, user);
}

int rpc_backend_remove(rpc_backend *backend, int fd) {
  if (!backend || fd < 0) return -1;
  rpc_backend_watch *watch = find_watch(backend, fd);
  if (!watch) return 0;
  watch->active = 0;
  cancel_watch(backend, watch);
  return 0;
}

int rpc_backend_wake(rpc_backend *backend) {
  if (!backend || backend->wake_fd < 0) return -1;
  uint64_t one = 1;
  ssize_t n = write(backend->wake_fd, &one, sizeof(one));
  return n == (ssize_t)sizeof(one) || errno == EAGAIN ? 0 : -1;
}

int rpc_backend_poll(rpc_backend *backend, rpc_backend_event *events, int max_events, int timeout_ms) {
  if (!backend || !events || max_events <= 0) return -1;
  unsigned want = timeout_ms == 0 ? 0u : 1u;
  int rc = uring_enter(backend->ring_fd, 0, want, IORING_ENTER_GETEVENTS);
  if (rc < 0) {
    if (errno == EINTR) return 0;
    return -1;
  }

  int out = 0;
  unsigned head = *backend->cq_head;
  unsigned tail = __atomic_load_n(backend->cq_tail, __ATOMIC_ACQUIRE);
  while (head != tail && out < max_events) {
    struct io_uring_cqe *cqe = &backend->cqes[head & *backend->cq_ring_mask];
    rpc_backend_watch *watch = (rpc_backend_watch *)(uintptr_t)cqe->user_data;
    if (watch) {
      watch->armed = 0;
      if (watch == &backend->wake_watch) {
        uint64_t value;
        while (read(backend->wake_fd, &value, sizeof(value)) == (ssize_t)sizeof(value)) {}
        events[out++] = (rpc_backend_event){.fd = backend->wake_fd, .events = RPC_BACKEND_WAKE, .user = 0};
        (void)arm_watch(backend, watch);
      } else if (watch->active && cqe->res >= 0) {
        uint32_t ev = 0;
        if ((uint32_t)cqe->res & (POLLIN | POLLRDHUP | POLLHUP | POLLERR)) { ev |= RPC_BACKEND_READ; }
        if ((uint32_t)cqe->res & (POLLOUT | POLLHUP | POLLERR)) { ev |= RPC_BACKEND_WRITE; }
        if (ev != 0) { events[out++] = (rpc_backend_event){.fd = watch->fd, .events = ev, .user = watch->user}; }
        (void)arm_watch(backend, watch);
      }
    }
    head++;
  }
  __atomic_store_n(backend->cq_head, head, __ATOMIC_RELEASE);
  (void)submit_pending(backend);
  return out;
}

#endif
