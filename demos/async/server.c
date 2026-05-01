#include "rpc/server.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct async_job {
  int64_t a;
  int64_t b;
  int64_t result;
  atomic_bool done;
  struct async_job *next;
} async_job;

typedef struct async_queue {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  async_job *head;
  async_job *tail;
  int stopping;
  pthread_t thread;
} async_queue;

static rpc_server *g_server;
static async_queue g_queue;

static void queue_push(async_queue *queue, async_job *job) {
  pthread_mutex_lock(&queue->mutex);
  job->next = NULL;
  if (queue->tail) {
    queue->tail->next = job;
  } else {
    queue->head = job;
  }
  queue->tail = job;
  pthread_cond_signal(&queue->cond);
  pthread_mutex_unlock(&queue->mutex);
}

static async_job *queue_pop(async_queue *queue) {
  pthread_mutex_lock(&queue->mutex);
  while (!queue->head && !queue->stopping) {
    pthread_cond_wait(&queue->cond, &queue->mutex);
  }
  async_job *job = queue->head;
  if (job) {
    queue->head = job->next;
    if (!queue->head) {
      queue->tail = NULL;
    }
  }
  pthread_mutex_unlock(&queue->mutex);
  return job;
}

static void *queue_worker(void *arg) {
  async_queue *queue = arg;
  for (;;) {
    async_job *job = queue_pop(queue);
    if (!job) {
      return NULL;
    }

    struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 1000000,
    };
    nanosleep(&delay, NULL);
    job->result = job->a + job->b;
    atomic_store_explicit(&job->done, true, memory_order_release);
  }
}

static int queue_start(async_queue *queue) {
  memset(queue, 0, sizeof(*queue));
  if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
    return -1;
  }
  if (pthread_cond_init(&queue->cond, NULL) != 0) {
    pthread_mutex_destroy(&queue->mutex);
    return -1;
  }
  if (pthread_create(&queue->thread, NULL, queue_worker, queue) != 0) {
    pthread_cond_destroy(&queue->cond);
    pthread_mutex_destroy(&queue->mutex);
    return -1;
  }
  return 0;
}

static void queue_stop(async_queue *queue) {
  pthread_mutex_lock(&queue->mutex);
  queue->stopping = 1;
  pthread_cond_broadcast(&queue->cond);
  pthread_mutex_unlock(&queue->mutex);
  pthread_join(queue->thread, NULL);
  pthread_cond_destroy(&queue->cond);
  pthread_mutex_destroy(&queue->mutex);
}

static void on_signal(int signo) {
  (void)signo;
  if (g_server) {
    rpc_server_stop(g_server);
  }
}

static int async_add(rpc_ctx *ctx, const rpc_value *args, size_t argc,
                     rpc_writer *out, void *user_data) {
  async_queue *queue = user_data;
  if (argc != 2 || args[0].type != RPC_TYPE_I64 ||
      args[1].type != RPC_TYPE_I64) {
    return -1;
  }

  async_job job;
  memset(&job, 0, sizeof(job));
  job.a = args[0].as.i64;
  job.b = args[1].as.i64;
  atomic_init(&job.done, false);

  queue_push(queue, &job);
  while (!atomic_load_explicit(&job.done, memory_order_acquire)) {
    rpc_ctx_yield(ctx);
  }

  return rpc_writer_i64(out, job.result);
}

int main(int argc, char **argv) {
  const char *host = argc > 1 ? argv[1] : "127.0.0.1";
  const char *port = argc > 2 ? argv[2] : "7001";
  uint32_t workers = argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 1;

  if (queue_start(&g_queue) != 0) {
    fprintf(stderr, "failed to start async queue\n");
    return 1;
  }

  if (rpc_server_init(&g_server) != 0 ||
      rpc_server_set_workers(g_server, workers) != 0 ||
      rpc_server_add_async_route(g_server, 3, async_add, &g_queue) != 0 ||
      rpc_server_bind(g_server, host, port) != 0 ||
      rpc_server_listen(g_server) != 0) {
    fprintf(stderr, "failed to start async RPC server\n");
    rpc_server_destroy(g_server);
    queue_stop(&g_queue);
    return 1;
  }

  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  printf("async rpc demo server listening on %s:%u\n", host,
         rpc_server_port(g_server));

  int rc = rpc_server_run(g_server);
  rpc_server_destroy(g_server);
  queue_stop(&g_queue);
  return rc == 0 ? 0 : 1;
}
