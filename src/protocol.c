#include "rpc/protocol.h"

#include <string.h>

static void put_u32(uint8_t *out, uint32_t value) {
  out[0] = (uint8_t)(value >> 24u);
  out[1] = (uint8_t)(value >> 16u);
  out[2] = (uint8_t)(value >> 8u);
  out[3] = (uint8_t)value;
}

static void put_u64(uint8_t *out, uint64_t value) {
  for (int i = 7; i >= 0; --i) {
    out[7 - i] = (uint8_t)(value >> (unsigned)(i * 8));
  }
}

static uint32_t get_u32(const uint8_t *in) {
  return ((uint32_t)in[0] << 24u) | ((uint32_t)in[1] << 16u) | ((uint32_t)in[2] << 8u) | (uint32_t)in[3];
}

static uint64_t get_u64(const uint8_t *in) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value = (value << 8u) | (uint64_t)in[i];
  }
  return value;
}

static int valid_op(uint8_t op) {
  return op == RPC_OP_RPC || op == RPC_OP_PING || op == RPC_OP_DISCONNECT || op == RPC_OP_RESPONSE ||
         op == RPC_OP_ERROR;
}

int rpc_header_encode(const rpc_header *header, uint8_t out[RPC_HEADER_SIZE]) {
  if (!header || !out || !valid_op((uint8_t)header->op) || header->size > RPC_MAX_PAYLOAD_SIZE) { return -1; }

  out[0] = (uint8_t)header->op;
  out[1] = header->flags;
  put_u32(out + 2, header->proc_id);
  put_u32(out + 6, header->size);
  put_u64(out + 10, header->call_id);
  return 0;
}

int rpc_header_decode(const uint8_t in[RPC_HEADER_SIZE], rpc_header *out) {
  if (!in || !out || !valid_op(in[0])) { return -1; }

  memset(out, 0, sizeof(*out));
  out->op = (rpc_op)in[0];
  out->flags = in[1];
  out->proc_id = get_u32(in + 2);
  out->size = get_u32(in + 6);
  out->call_id = get_u64(in + 10);
  if (out->size > RPC_MAX_PAYLOAD_SIZE) { return -1; }
  return 0;
}
