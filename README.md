# wirecall

wirecall is a small C RPC library for making named procedure calls over TCP.
It gives you a compact binary protocol, typed primitive arguments, dynamic
routes, sync and coroutine-backed async handlers, and a socket event backend
that can use the native IO primitive for the host platform.

The goal is simple: your application should mostly read like this:

```text
server: init, add routes, bind, run
client: connect, call, close
```

**Project Status: young and usable.** wirecall is early, but it already has
the core pieces: TCP server/client support, typed payloads, name-based calls,
dynamic route replacement, packet integrity checks, tracing, benchmarks, and
platform backend selection. The API may still move while the library finds its
sharpest shape.

## Features

**Named RPC calls.** Procedures are registered by name and called by name. The
library maps names to stable internal IDs so the hot path stays table-friendly.

**Compact binary packets.** Each packet has an operation, flags, procedure ID,
payload size, call ID, checksum, and optional keyed MAC. Payloads are typed
primitive values.

**Typed primitive payloads.** wirecall supports `null`, `bool`, `i64`, `u64`,
`f64`, `bytes`, and `string`. Handlers validate the arity and types they expect.

**Sync and async handlers.** Fast sync handlers run without creating a
coroutine. Async handlers use vendored `minicoro` and can yield while external
work completes.

**Dynamic routes.** Routes can be added, replaced, and removed at runtime.
Route state can have finalizers, which makes embedding into runtimes less
awkward.

**Low-latency server loop.** Connections and calls use mmap-backed fixed
arenas, route lookup is direct and paged, response writes are batched, and
tracing is disabled by default behind an unlikely runtime flag.

**Native event backends.** macOS and BSD use `kqueue`, Linux uses an
`io_uring` backend, and Windows uses a WSA event backend behind the same
interface. The public API does not change with the backend.

**Embedding hooks.** A process-wide allocator hook lets a host runtime route
wirecall allocations through its own allocator, accounting, leak tracker, or
arena policy.

## Example

This pair registers an `add` procedure on the server and calls it from a
client.

<table>
<tr>
<td>Server</td>
<td>Client</td>
</tr>
<tr>
<td>

```c
#include "wirecall/server.h"

#include <stdio.h>

static int add(
  wirecall_ctx *ctx,
  const wirecall_value *args,
  size_t argc,
  wirecall_writer *out,
  void *user_data
) {
  if (argc != 2 ||
      args[0].type != WIRECALL_TYPE_I64 ||
      args[1].type != WIRECALL_TYPE_I64) {
    return -1;
  }

  return wirecall_writer_i64(out, args[0].as.i64 + args[1].as.i64);
}

int main(void) {
  wirecall_server *server = NULL;

  if (wirecall_server_init(&server) != 0 ||
      wirecall_server_add_route_name(server, "add", add, NULL) != 0 ||
      wirecall_server_bind(server, "127.0.0.1", "7000") != 0 ||
      wirecall_server_listen(server) != 0) {
    fprintf(stderr, "server setup failed\n");
    wirecall_server_destroy(server);
    return 1;
  }

  int rc = wirecall_server_run(server);
  wirecall_server_destroy(server);
  return rc == 0 ? 0 : 1;
}
```

</td>
<td>

```c
#include "wirecall/client.h"

#include <stdio.h>

int main(void) {
  wirecall_client *client = NULL;
  if (wirecall_client_connect(&client, "127.0.0.1", "7000") != 0) {
    return 1;
  }

  wirecall_writer args;
  wirecall_writer_init(&args);
  wirecall_writer_i64(&args, 20);
  wirecall_writer_i64(&args, 22);

  wirecall_value *result = NULL;
  size_t result_count = 0;
  int rc = wirecall_client_call_name(
    client,
    "add",
    &args,
    &result,
    &result_count
  );

  if (rc == 0 &&
      result_count == 1 &&
      result[0].type == WIRECALL_TYPE_I64) {
    printf("20 + 22 = %lld\n", (long long)result[0].as.i64);
  }

  wirecall_values_free(result);
  wirecall_writer_free(&args);
  wirecall_client_close(client);
  return rc == 0 ? 0 : 1;
}
```

</td>
</tr>
</table>

## Async Handlers

Async routes are opt-in. A sync route stays on the fast path and does not pay
for coroutine creation. An async route can yield:

```c
static int wait_for_job(
  wirecall_ctx *ctx,
  const wirecall_value *args,
  size_t argc,
  wirecall_writer *out,
  void *user_data
) {
  job *j = enqueue_job(user_data, args, argc);
  while (!job_done(j)) {
    wirecall_ctx_yield(ctx);
  }
  return wirecall_writer_i64(out, job_result(j));
}

wirecall_server_add_async_route_name(server, "job.add", wait_for_job, queue);
```

See `demos/async/server.c` for a complete queue-backed example.

## Packet Integrity

wirecall packets include integrity fields in the packet header. By default the
library uses a cheap checksum. A keyed MAC can be enabled when both sides share
a 16-byte key:

```c
uint8_t key[16] = {
  0x00, 0x01, 0x02, 0x03,
  0x04, 0x05, 0x06, 0x07,
  0x08, 0x09, 0x0a, 0x0b,
  0x0c, 0x0d, 0x0e, 0x0f,
};

wirecall_server_set_integrity(
  server,
  WIRECALL_INTEGRITY_CHECKSUM | WIRECALL_INTEGRITY_MAC,
  key
);

wirecall_client_set_integrity(
  client,
  WIRECALL_INTEGRITY_CHECKSUM | WIRECALL_INTEGRITY_MAC,
  key
);
```

Integrity can be disabled for raw benchmarking with `WIRECALL_INTEGRITY_NONE`.

## Embedding

wirecall is meant to be easy to drop into another runtime. The two main hooks
are allocator replacement and route finalizers:

```c
wirecall_allocator allocator = {
  .ctx = runtime,
  .alloc = runtime_alloc,
  .realloc = runtime_realloc,
  .free = runtime_free,
};

wirecall_set_allocator(&allocator);

wirecall_server_add_route_name_ex(
  server,
  "vm.call",
  vm_call,
  closure,
  closure_release
);
```

Set the allocator before creating wirecall objects and do not swap it while the
library is live. See `demos/embed.c` for a small embedding demo with allocation
stats and route state finalization.

## Build

wirecall uses Meson and C23 with GNU extensions.

```sh
meson setup build
meson compile -C build
```

To consume wirecall from another Meson project, copy `wirecall.wrap` into that
project's `subprojects/` directory and use:

```meson
wirecall = dependency('wirecall')

executable(
  'my-rpc-client',
  'main.c',
  dependencies: wirecall,
)
```

When wirecall is used as a subproject, demos and tests are not built by default.
They can be enabled explicitly:

```sh
meson setup build -Dwirecall:demos=enabled -Dwirecall:tests=enabled
```

The build produces:

```text
wirecall-server
wirecall-client
wirecall-async-server
wirecall-async-client
wirecall-embed
wirecall-bench
```

Run the test suite with:

```sh
meson test -C build --print-errorlogs
```

Format the code with:

```sh
meson compile -C build format
```

## Benchmark

The benchmark starts an in-process server and drives pipelined client traffic
against it:

```sh
./build/wirecall-bench 1000000 40 100 4 8
```

Arguments are:

```text
requests clients warmup server_workers pipeline_depth [trace] [integrity] [mac]
```

For example, this enables tracing:

```sh
./build/wirecall-bench 1000000 40 100 4 8 1
```

Trace output includes client send/receive timing, server read/parse/write
timing, route lookup, worker distribution, CPU time, context switches, RSS, and
virtual memory size.

## Backend Selection

Backend selection happens at build time:

```text
macOS, BSD  -> kqueue
Linux       -> io_uring
Windows     -> WSA event backend
```

The backend boundary is internal. Public code should include
`wirecall/client.h`, `wirecall/server.h`, `wirecall/protocol.h`, or
`wirecall/trace.h`.

## Design Notes

wirecall is intentionally small. The main tradeoff is that v1 uses a built-in
typed primitive format instead of user codecs or schema generation. That keeps
the library easy to embed and keeps the packet path predictable.

The fast path is tuned around a few rules:

- decode packets without allocating where practical
- keep sync handlers out of the coroutine scheduler
- batch response writes per connection
- use table-shaped route lookup
- keep tracing compiled in but cold until enabled
- isolate platform IO behind a strict backend interface

## Roadmap

Things that still deserve love:

- broader Linux validation for the `io_uring` backend
- deeper Windows backend testing
- TLS or transport hooks
- cancellation and deadline APIs
- stronger documentation for embedding into language runtimes
- fuzzing for packet and payload decoding
