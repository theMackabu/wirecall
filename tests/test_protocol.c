#include "rpc/protocol.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct alloc_stats {
  size_t allocs;
  size_t reallocs;
  size_t frees;
} alloc_stats;

static void *test_alloc(void *ctx, size_t size) {
  alloc_stats *stats = ctx;
  stats->allocs++;
  return malloc(size);
}

static void *test_realloc(void *ctx, void *ptr, size_t size) {
  alloc_stats *stats = ctx;
  stats->reallocs++;
  return realloc(ptr, size);
}

static void test_free(void *ctx, void *ptr) {
  alloc_stats *stats = ctx;
  stats->frees++;
  free(ptr);
}

int main(void) {
  uint8_t buf[RPC_HEADER_SIZE];
  rpc_header h = {
      .op = RPC_OP_RPC,
      .flags = RPC_FLAG_MORE,
      .proc_id = 0x1122334455667788ull,
      .size = 9,
      .call_id = 0x0102030405060708ull,
  };
  uint8_t packet_payload[9] = {RPC_TYPE_I64, 0, 0, 0, 0, 0, 0, 0, 42};
  uint8_t mac_key[16] = {
      0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  };
  assert(rpc_packet_sign(&h, packet_payload, sizeof(packet_payload)) == 0);
  assert(h.checksum != 0);
  assert(h.mac == 0);
  assert(rpc_header_encode(&h, buf) == 0);
  rpc_header out;
  assert(rpc_header_decode(buf, &out) == 0);
  assert(out.op == h.op);
  assert(out.flags == h.flags);
  assert(out.proc_id == h.proc_id);
  assert(out.size == h.size);
  assert(out.call_id == h.call_id);
  assert(out.checksum == h.checksum);
  assert(out.mac == h.mac);
  assert(rpc_packet_verify(&out, packet_payload, sizeof(packet_payload)) == 0);
  packet_payload[8] = 43;
  assert(rpc_packet_verify(&out, packet_payload, sizeof(packet_payload)) != 0);
  packet_payload[8] = 42;
  assert(rpc_packet_sign_ex(&h, packet_payload, sizeof(packet_payload), RPC_INTEGRITY_CHECKSUM | RPC_INTEGRITY_MAC,
                            mac_key) == 0);
  assert(h.checksum != 0);
  assert(h.mac != 0);
  assert(rpc_packet_verify_ex(&h, packet_payload, sizeof(packet_payload), RPC_INTEGRITY_CHECKSUM | RPC_INTEGRITY_MAC,
                              mac_key) == 0);
  h.mac ^= 1u;
  assert(rpc_packet_verify_ex(&h, packet_payload, sizeof(packet_payload), RPC_INTEGRITY_CHECKSUM | RPC_INTEGRITY_MAC,
                              mac_key) != 0);
  h.mac ^= 1u;

  buf[0] = 99;
  assert(rpc_header_decode(buf, &out) != 0);

  alloc_stats stats = {0};
  rpc_allocator allocator = {
      .ctx = &stats,
      .alloc = test_alloc,
      .realloc = test_realloc,
      .free = test_free,
  };
  assert(rpc_set_allocator(&allocator) == 0);

  rpc_writer w;
  rpc_writer_init(&w);
  assert(rpc_writer_null(&w) == 0);
  assert(rpc_writer_bool(&w, true) == 0);
  assert(rpc_writer_i64(&w, -42) == 0);
  assert(rpc_writer_u64(&w, 42) == 0);
  assert(rpc_writer_f64(&w, 3.5) == 0);
  assert(rpc_writer_bytes(&w, "abc", 3) == 0);
  assert(rpc_writer_string(&w, "hello", 5) == 0);

  rpc_value *values = NULL;
  size_t count = 0;
  assert(rpc_payload_decode(w.data, w.len, &values, &count) == 0);
  assert(count == 7);
  assert(values[0].type == RPC_TYPE_NULL);
  assert(values[1].as.boolean);
  assert(values[2].as.i64 == -42);
  assert(values[3].as.u64 == 42);
  assert(fabs(values[4].as.f64 - 3.5) < 0.001);
  assert(values[5].as.bytes.len == 3);
  assert(memcmp(values[5].as.bytes.data, "abc", 3) == 0);
  assert(values[6].as.string.len == 5);
  assert(memcmp(values[6].as.string.data, "hello", 5) == 0);
  rpc_values_free(values);

  uint8_t malformed[] = {RPC_TYPE_STRING, 0, 0, 0, 10, 'x'};
  assert(rpc_payload_decode(malformed, sizeof(malformed), &values, &count) != 0);
  rpc_writer_free(&w);
  assert(stats.reallocs > 0);
  assert(stats.frees > 0);
  assert(rpc_set_allocator(NULL) == 0);
  return 0;
}
