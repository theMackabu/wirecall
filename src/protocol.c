#include "rpc/protocol.h"

#include "proc.h"

#include <string.h>

uint64_t rpc_proc_id(const char *name) {
  uint64_t hash = 14695981039346656037ull;
  if (!name) { return 0; }
  while (*name) {
    hash ^= (uint8_t)*name++;
    hash *= 1099511628211ull;
  }
  return hash ? hash : 1u;
}

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

static uint32_t checksum_update(uint32_t crc, const uint8_t *data, size_t len) {
  static const uint32_t table[16] = {
      0x00000000u, 0x105ec76fu, 0x20bd8edeu, 0x30e349b1u, 0x417b1dbcu, 0x5125dad3u,
      0x61c69362u, 0x7198540du, 0x82f63b78u, 0x92a8fc17u, 0xa24bb5a6u, 0xb21572c9u,
      0xc38d26c4u, 0xd3d3e1abu, 0xe330a81au, 0xf36e6f75u,
  };

  for (size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    crc = (crc >> 4u) ^ table[crc & 0x0fu];
    crc = (crc >> 4u) ^ table[crc & 0x0fu];
  }
  return crc;
}

static int valid_op(uint8_t op) {
  return op == RPC_OP_RPC || op == RPC_OP_PING || op == RPC_OP_DISCONNECT || op == RPC_OP_RESPONSE ||
         op == RPC_OP_ERROR;
}

int rpc_header_encode(const rpc_header *header, uint8_t out[RPC_HEADER_SIZE]) {
  if (!header || !out || !valid_op((uint8_t)header->op) || header->size > RPC_MAX_PAYLOAD_SIZE) { return -1; }

  out[0] = (uint8_t)header->op;
  out[1] = header->flags;
  put_u64(out + 2, header->proc_id);
  put_u32(out + 10, header->size);
  put_u64(out + 14, header->call_id);
  put_u32(out + 22, header->checksum);
  return 0;
}

int rpc_header_decode(const uint8_t in[RPC_HEADER_SIZE], rpc_header *out) {
  if (!in || !out || !valid_op(in[0])) { return -1; }

  memset(out, 0, sizeof(*out));
  out->op = (rpc_op)in[0];
  out->flags = in[1];
  out->proc_id = get_u64(in + 2);
  out->size = get_u32(in + 10);
  out->call_id = get_u64(in + 14);
  out->checksum = get_u32(in + 22);
  if (out->size > RPC_MAX_PAYLOAD_SIZE) { return -1; }
  return 0;
}

uint32_t rpc_packet_checksum(const rpc_header *header, const uint8_t *payload, size_t payload_len) {
  if (!header || (!payload && payload_len > 0) || payload_len > RPC_MAX_PAYLOAD_SIZE) { return 0; }

  rpc_header signed_header = *header;
  signed_header.checksum = 0;

  uint8_t header_buf[RPC_HEADER_SIZE];
  if (rpc_header_encode(&signed_header, header_buf) != 0) { return 0; }

  uint32_t crc = 0xffffffffu;
  crc = checksum_update(crc, header_buf, sizeof(header_buf));
  crc = checksum_update(crc, payload, payload_len);
  return ~crc;
}

int rpc_packet_sign(rpc_header *header, const uint8_t *payload, size_t payload_len) {
  if (!header || (!payload && payload_len > 0) || payload_len > RPC_MAX_PAYLOAD_SIZE ||
      header->size != payload_len) {
    return -1;
  }
  header->checksum = rpc_packet_checksum(header, payload, payload_len);
  return 0;
}

int rpc_packet_verify(const rpc_header *header, const uint8_t *payload, size_t payload_len) {
  if (!header || (!payload && payload_len > 0) || payload_len > RPC_MAX_PAYLOAD_SIZE ||
      header->size != payload_len) {
    return -1;
  }
  return rpc_packet_checksum(header, payload, payload_len) == header->checksum ? 0 : -1;
}
