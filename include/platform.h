#ifndef WIRECALL_PLATFORM_H
#define WIRECALL_PLATFORM_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <errno.h>
#include <limits.h>
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#if !defined(__MINGW32__) && !defined(__MINGW64__)
typedef SSIZE_T ssize_t;
typedef int socklen_t;
#endif

typedef CRITICAL_SECTION wirecall_mutex_t;
typedef HANDLE wirecall_thread_t;
typedef void *(*wirecall_thread_fn)(void *);

typedef struct wirecall_thread_start {
  wirecall_thread_fn fn;
  void *arg;
} wirecall_thread_start;

static inline int wirecall_mutex_init(wirecall_mutex_t *mutex) {
  InitializeCriticalSection(mutex);
  return 0;
}

static inline void wirecall_mutex_lock(wirecall_mutex_t *mutex) {
  EnterCriticalSection(mutex);
}

static inline void wirecall_mutex_unlock(wirecall_mutex_t *mutex) {
  LeaveCriticalSection(mutex);
}

static inline void wirecall_mutex_destroy(wirecall_mutex_t *mutex) {
  DeleteCriticalSection(mutex);
}

static unsigned __stdcall wirecall_thread_entry(void *arg) {
  wirecall_thread_start *start = arg;
  wirecall_thread_fn fn = start->fn;
  void *fn_arg = start->arg;
  free(start);
  (void)fn(fn_arg);
  return 0;
}

static inline int wirecall_thread_create(wirecall_thread_t *thread, wirecall_thread_fn fn, void *arg) {
  wirecall_thread_start *start = malloc(sizeof(*start));
  if (!start) { return -1; }
  start->fn = fn;
  start->arg = arg;
  uintptr_t handle = _beginthreadex(NULL, 0, wirecall_thread_entry, start, 0, NULL);
  if (handle == 0) {
    free(start);
    return -1;
  }
  *thread = (HANDLE)handle;
  return 0;
}

static inline int wirecall_thread_join(wirecall_thread_t thread) {
  if (!thread) { return -1; }
  DWORD wait = WaitForSingleObject(thread, INFINITE);
  CloseHandle(thread);
  return wait == WAIT_OBJECT_0 ? 0 : -1;
}

static inline uint32_t wirecall_cpu_count(void) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwNumberOfProcessors ? (uint32_t)info.dwNumberOfProcessors : 1u;
}

static inline int wirecall_socket_startup(WSADATA *wsa) {
  return WSAStartup(MAKEWORD(2, 2), wsa);
}

static inline void wirecall_socket_cleanup(void) {
  WSACleanup();
}

static inline int wirecall_socket_error(void) {
  return WSAGetLastError();
}

static inline int wirecall_socket_interrupted(int error) {
  return error == WSAEINTR;
}

static inline int wirecall_socket_would_block(int error) {
  return error == WSAEWOULDBLOCK;
}

static inline int wirecall_socket_close(int fd) {
  return closesocket((SOCKET)(uintptr_t)fd);
}

static inline int wirecall_socket_set_nonblock(int fd) {
  u_long enabled = 1;
  return ioctlsocket((SOCKET)(uintptr_t)fd, FIONBIO, &enabled);
}

static inline int wirecall_socket_create(int family, int socktype, int protocol) {
  SOCKET socket_fd = socket(family, socktype, protocol);
  return socket_fd == INVALID_SOCKET ? -1 : (int)(uintptr_t)socket_fd;
}

static inline int wirecall_socket_accept(int fd, struct sockaddr *addr, socklen_t *addr_len) {
  SOCKET socket_fd = accept((SOCKET)(uintptr_t)fd, addr, addr_len);
  return socket_fd == INVALID_SOCKET ? -1 : (int)(uintptr_t)socket_fd;
}

static inline int wirecall_socket_connect(int fd, const struct sockaddr *addr, socklen_t addr_len) {
  return connect((SOCKET)(uintptr_t)fd, addr, addr_len);
}

static inline int wirecall_socket_bind(int fd, const struct sockaddr *addr, socklen_t addr_len) {
  return bind((SOCKET)(uintptr_t)fd, addr, addr_len);
}

static inline int wirecall_socket_getsockname(int fd, struct sockaddr *addr, socklen_t *addr_len) {
  return getsockname((SOCKET)(uintptr_t)fd, addr, addr_len);
}

static inline int wirecall_socket_listen(int fd, int backlog) {
  return listen((SOCKET)(uintptr_t)fd, backlog);
}

static inline int wirecall_socket_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) {
  return setsockopt((SOCKET)(uintptr_t)fd, level, optname, (const char *)optval, optlen);
}

static inline ssize_t wirecall_socket_recv(int fd, void *buf, size_t len, int flags) {
  int chunk = len > INT_MAX ? INT_MAX : (int)len;
  return recv((SOCKET)(uintptr_t)fd, (char *)buf, chunk, flags);
}

static inline ssize_t wirecall_socket_send(int fd, const void *buf, size_t len, int flags) {
  int chunk = len > INT_MAX ? INT_MAX : (int)len;
  return send((SOCKET)(uintptr_t)fd, (const char *)buf, chunk, flags);
}

static inline void *wirecall_pages_alloc(size_t bytes) {
  return VirtualAlloc(NULL, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static inline void wirecall_pages_free(void *ptr, size_t bytes) {
  (void)bytes;
  if (ptr) { VirtualFree(ptr, 0, MEM_RELEASE); }
}

static inline uint64_t wirecall_time_ns(void) {
  LARGE_INTEGER counter;
  LARGE_INTEGER frequency;
  if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency)) { return 0; }
  uint64_t seconds = (uint64_t)(counter.QuadPart / frequency.QuadPart);
  uint64_t remainder = (uint64_t)(counter.QuadPart % frequency.QuadPart);
  return seconds * 1000000000ull + (remainder * 1000000000ull) / (uint64_t)frequency.QuadPart;
}

#else

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef pthread_mutex_t wirecall_mutex_t;
typedef pthread_t wirecall_thread_t;
typedef void *(*wirecall_thread_fn)(void *);

static inline int wirecall_mutex_init(wirecall_mutex_t *mutex) {
  return pthread_mutex_init(mutex, NULL);
}

static inline void wirecall_mutex_lock(wirecall_mutex_t *mutex) {
  pthread_mutex_lock(mutex);
}

static inline void wirecall_mutex_unlock(wirecall_mutex_t *mutex) {
  pthread_mutex_unlock(mutex);
}

static inline void wirecall_mutex_destroy(wirecall_mutex_t *mutex) {
  pthread_mutex_destroy(mutex);
}

static inline int wirecall_thread_create(wirecall_thread_t *thread, wirecall_thread_fn fn, void *arg) {
  return pthread_create(thread, NULL, fn, arg);
}

static inline int wirecall_thread_join(wirecall_thread_t thread) {
  return pthread_join(thread, NULL);
}

static inline uint32_t wirecall_cpu_count(void) {
  long n = sysconf(_SC_NPROCESSORS_ONLN);
  return n > 0 ? (uint32_t)n : 1u;
}

static inline int wirecall_socket_error(void) {
  return errno;
}

static inline int wirecall_socket_interrupted(int error) {
  return error == EINTR;
}

static inline int wirecall_socket_would_block(int error) {
  return error == EAGAIN || error == EWOULDBLOCK;
}

static inline int wirecall_socket_close(int fd) {
  return close(fd);
}

static inline int wirecall_socket_set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) { return -1; }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static inline int wirecall_socket_create(int family, int socktype, int protocol) {
  return socket(family, socktype, protocol);
}

static inline int wirecall_socket_accept(int fd, struct sockaddr *addr, socklen_t *addr_len) {
  return accept(fd, addr, addr_len);
}

static inline int wirecall_socket_connect(int fd, const struct sockaddr *addr, socklen_t addr_len) {
  return connect(fd, addr, addr_len);
}

static inline int wirecall_socket_bind(int fd, const struct sockaddr *addr, socklen_t addr_len) {
  return bind(fd, addr, addr_len);
}

static inline int wirecall_socket_getsockname(int fd, struct sockaddr *addr, socklen_t *addr_len) {
  return getsockname(fd, addr, addr_len);
}

static inline int wirecall_socket_listen(int fd, int backlog) {
  return listen(fd, backlog);
}

static inline int wirecall_socket_setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) {
  return setsockopt(fd, level, optname, optval, optlen);
}

static inline ssize_t wirecall_socket_recv(int fd, void *buf, size_t len, int flags) {
  return recv(fd, buf, len, flags);
}

static inline ssize_t wirecall_socket_send(int fd, const void *buf, size_t len, int flags) {
  return send(fd, buf, len, flags);
}

static inline void *wirecall_pages_alloc(size_t bytes) {
  void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  return p == MAP_FAILED ? NULL : p;
}

static inline void wirecall_pages_free(void *ptr, size_t bytes) {
  if (ptr) { munmap(ptr, bytes); }
}

static inline uint64_t wirecall_time_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

#endif

#endif
