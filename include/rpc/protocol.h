#ifndef RPC_PROTOCOL_H
#define RPC_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RPC_HEADER_SIZE 18u
#define RPC_MAX_PAYLOAD_SIZE (1024u * 1024u)

typedef enum rpc_op {
  RPC_OP_RPC = 1,
  RPC_OP_PING = 2,
  RPC_OP_DISCONNECT = 3,
  RPC_OP_RESPONSE = 4,
  RPC_OP_ERROR = 5,
} rpc_op;

typedef enum rpc_flag {
  RPC_FLAG_NONE = 0u,
  RPC_FLAG_MORE = 1u << 0u,
} rpc_flag;

typedef enum rpc_type {
  RPC_TYPE_NULL = 0,
  RPC_TYPE_BOOL = 1,
  RPC_TYPE_I64 = 2,
  RPC_TYPE_U64 = 3,
  RPC_TYPE_F64 = 4,
  RPC_TYPE_BYTES = 5,
  RPC_TYPE_STRING = 6,
} rpc_type;

typedef struct rpc_value {
  rpc_type type;
  union {
    bool boolean;
    int64_t i64;
    uint64_t u64;
    double f64;
    struct {
      const uint8_t *data;
      uint32_t len;
    } bytes;
    struct {
      const char *data;
      uint32_t len;
    } string;
  } as;
} rpc_value;

typedef struct rpc_header {
  rpc_op op;
  uint8_t flags;
  uint32_t proc_id;
  uint32_t size;
  uint64_t call_id;
} rpc_header;

typedef struct rpc_writer {
  uint8_t *data;
  size_t len;
  size_t cap;
} rpc_writer;

int rpc_header_encode(const rpc_header *header, uint8_t out[RPC_HEADER_SIZE]);
int rpc_header_decode(const uint8_t in[RPC_HEADER_SIZE], rpc_header *out);

void rpc_writer_init(rpc_writer *writer);
void rpc_writer_reset(rpc_writer *writer);
void rpc_writer_free(rpc_writer *writer);
int rpc_writer_null(rpc_writer *writer);
int rpc_writer_bool(rpc_writer *writer, bool value);
int rpc_writer_i64(rpc_writer *writer, int64_t value);
int rpc_writer_u64(rpc_writer *writer, uint64_t value);
int rpc_writer_f64(rpc_writer *writer, double value);
int rpc_writer_bytes(rpc_writer *writer, const void *data, uint32_t len);
int rpc_writer_string(rpc_writer *writer, const char *data, uint32_t len);

int rpc_payload_decode(const uint8_t *data, size_t len, rpc_value **out_values,
                       size_t *out_count);
void rpc_values_free(rpc_value *values);

#ifdef __cplusplus
}
#endif

#endif
