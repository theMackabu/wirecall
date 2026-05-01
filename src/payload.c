#include "rpc/protocol.h"
#include "rpc/trace.h"

#include "memory.h"

#include <errno.h>
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

static int writer_reserve(rpc_writer *writer, size_t extra) {
  if (!writer || extra > RPC_MAX_PAYLOAD_SIZE || writer->len > RPC_MAX_PAYLOAD_SIZE - extra) { return -1; }
  size_t need = writer->len + extra;
  if (need <= writer->cap) { return 0; }

  size_t cap = writer->cap ? writer->cap : 64u;
  while (cap < need) {
    if (cap > RPC_MAX_PAYLOAD_SIZE / 2u) {
      cap = RPC_MAX_PAYLOAD_SIZE;
      break;
    }
    cap *= 2u;
  }
  uint8_t *next = rpc_mem_realloc(writer->data, cap);
  if (!next) { return -1; }
  writer->data = next;
  writer->cap = cap;
  return 0;
}

static int writer_push(rpc_writer *writer, const void *data, size_t len) {
  if (writer_reserve(writer, len) != 0) { return -1; }
  if (len > 0) { memcpy(writer->data + writer->len, data, len); }
  writer->len += len;
  return 0;
}

void rpc_writer_init(rpc_writer *writer) {
  if (writer) { memset(writer, 0, sizeof(*writer)); }
}

void rpc_writer_reset(rpc_writer *writer) {
  if (writer) { writer->len = 0; }
}

void rpc_writer_free(rpc_writer *writer) {
  if (writer) {
    rpc_mem_free(writer->data);
    memset(writer, 0, sizeof(*writer));
  }
}

int rpc_writer_null(rpc_writer *writer) {
  uint8_t type = RPC_TYPE_NULL;
  return writer_push(writer, &type, 1);
}

int rpc_writer_bool(rpc_writer *writer, bool value) {
  uint8_t data[2] = {RPC_TYPE_BOOL, value ? 1u : 0u};
  return writer_push(writer, data, sizeof(data));
}

int rpc_writer_i64(rpc_writer *writer, int64_t value) {
  uint8_t data[9];
  data[0] = RPC_TYPE_I64;
  put_u64(data + 1, (uint64_t)value);
  return writer_push(writer, data, sizeof(data));
}

int rpc_writer_u64(rpc_writer *writer, uint64_t value) {
  uint8_t data[9];
  data[0] = RPC_TYPE_U64;
  put_u64(data + 1, value);
  return writer_push(writer, data, sizeof(data));
}

int rpc_writer_f64(rpc_writer *writer, double value) {
  uint8_t data[9];
  uint64_t bits = 0;
  memcpy(&bits, &value, sizeof(bits));
  data[0] = RPC_TYPE_F64;
  put_u64(data + 1, bits);
  return writer_push(writer, data, sizeof(data));
}

int rpc_writer_bytes(rpc_writer *writer, const void *data, uint32_t len) {
  uint8_t prefix[5];
  if (len > 0 && !data) { return -1; }
  prefix[0] = RPC_TYPE_BYTES;
  put_u32(prefix + 1, len);
  if (writer_push(writer, prefix, sizeof(prefix)) != 0) { return -1; }
  return writer_push(writer, data, len);
}

int rpc_writer_string(rpc_writer *writer, const char *data, uint32_t len) {
  uint8_t prefix[5];
  if (len > 0 && !data) { return -1; }
  prefix[0] = RPC_TYPE_STRING;
  put_u32(prefix + 1, len);
  if (writer_push(writer, prefix, sizeof(prefix)) != 0) { return -1; }
  return writer_push(writer, data, len);
}

int rpc_payload_decode(const uint8_t *data, size_t len, rpc_value **out_values, size_t *out_count) {
  uint64_t trace = rpc_trace_begin();
  static const void *dispatch[] = {
      [RPC_TYPE_NULL] = &&type_null,     [RPC_TYPE_BOOL] = &&type_bool, [RPC_TYPE_I64] = &&type_i64,
      [RPC_TYPE_U64] = &&type_u64,       [RPC_TYPE_F64] = &&type_f64,   [RPC_TYPE_BYTES] = &&type_bytes,
      [RPC_TYPE_STRING] = &&type_string,
  };

  if ((!data && len > 0) || !out_values || !out_count) {
    rpc_trace_end(RPC_TRACE_PAYLOAD_DECODE, trace);
    return -1;
  }

  rpc_value *values = NULL;
  size_t count = 0;
  size_t cap = 0;
  size_t off = 0;

  while (off < len) {
    if (count == cap) {
      size_t next_cap = cap ? cap * 2u : 4u;
      rpc_value *next = rpc_mem_realloc(values, next_cap * sizeof(*values));
      if (!next) {
        rpc_mem_free(values);
        rpc_trace_end(RPC_TRACE_PAYLOAD_DECODE, trace);
        return -1;
      }
      values = next;
      cap = next_cap;
    }

    rpc_value value;
    memset(&value, 0, sizeof(value));
    value.type = (rpc_type)data[off++];

    if ((size_t)value.type >= sizeof(dispatch) / sizeof(*dispatch) || !dispatch[value.type]) { goto malformed; }
    goto *dispatch[value.type];

  type_null:
    goto store;

  type_bool:
    if (off + 1u > len || (data[off] != 0u && data[off] != 1u)) { goto malformed; }
    value.as.boolean = data[off++] != 0u;
    goto store;

  type_i64:
    if (off + 8u > len) { goto malformed; }
    value.as.i64 = (int64_t)get_u64(data + off);
    off += 8u;
    goto store;

  type_u64:
    if (off + 8u > len) { goto malformed; }
    value.as.u64 = get_u64(data + off);
    off += 8u;
    goto store;

  type_f64: {
    if (off + 8u > len) { goto malformed; }
    uint64_t bits = get_u64(data + off);
    memcpy(&value.as.f64, &bits, sizeof(bits));
    off += 8u;
    goto store;
  }

  type_bytes:
  type_string: {
    if (off + 4u > len) { goto malformed; }
    uint32_t value_len = get_u32(data + off);
    off += 4u;
    if (off + value_len > len) { goto malformed; }
    if (value.type == RPC_TYPE_BYTES) {
      value.as.bytes.data = data + off;
      value.as.bytes.len = value_len;
    } else {
      value.as.string.data = (const char *)(data + off);
      value.as.string.len = value_len;
    }
    off += value_len;
    goto store;
  }

  store:
    values[count++] = value;
    continue;

  malformed:
    rpc_mem_free(values);
    rpc_trace_end(RPC_TRACE_PAYLOAD_DECODE, trace);
    return -1;
  }

  *out_values = values;
  *out_count = count;
  rpc_trace_end(RPC_TRACE_PAYLOAD_DECODE, trace);
  return 0;
}

void rpc_values_free(rpc_value *values) {
  rpc_mem_free(values);
}
