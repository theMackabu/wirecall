#ifndef WIRECALL_PROTOCOL_H
#define WIRECALL_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIRECALL_HEADER_SIZE 34u
#define WIRECALL_MAX_PAYLOAD_SIZE (1024u * 1024u)

#define WIRECALL_INTEGRITY_NONE 0u
#define WIRECALL_INTEGRITY_CHECKSUM (1u << 0u)
#define WIRECALL_INTEGRITY_MAC (1u << 1u)
#define WIRECALL_INTEGRITY_DEFAULT WIRECALL_INTEGRITY_CHECKSUM

typedef enum wirecall_op {
  WIRECALL_OP_RPC = 1,
  WIRECALL_OP_PING = 2,
  WIRECALL_OP_DISCONNECT = 3,
  WIRECALL_OP_RESPONSE = 4,
  WIRECALL_OP_ERROR = 5,
} wirecall_op;

typedef enum wirecall_flag {
  WIRECALL_FLAG_NONE = 0u,
  WIRECALL_FLAG_MORE = 1u << 0u,
} wirecall_flag;

typedef enum wirecall_type {
  WIRECALL_TYPE_NULL = 0,
  WIRECALL_TYPE_BOOL = 1,
  WIRECALL_TYPE_I64 = 2,
  WIRECALL_TYPE_U64 = 3,
  WIRECALL_TYPE_F64 = 4,
  WIRECALL_TYPE_BYTES = 5,
  WIRECALL_TYPE_STRING = 6,
} wirecall_type;

typedef struct wirecall_value {
  wirecall_type type;
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
} wirecall_value;

typedef struct wirecall_allocator {
  void *ctx;
  void *(*alloc)(void *ctx, size_t size);
  void *(*realloc)(void *ctx, void *ptr, size_t size);
  void (*free)(void *ctx, void *ptr);
} wirecall_allocator;

typedef struct wirecall_header {
  wirecall_op op;
  uint8_t flags;
  uint64_t proc_id;
  uint32_t size;
  uint64_t call_id;
  uint32_t checksum;
  uint64_t mac;
} wirecall_header;

typedef struct wirecall_writer {
  uint8_t *data;
  size_t len;
  size_t cap;
} wirecall_writer;

/* Process-wide allocator hook. Set before creating/freeing Wirecall objects or payload buffers; do not swap it live. */
int wirecall_set_allocator(const wirecall_allocator *allocator);
void wirecall_get_allocator(wirecall_allocator *out_allocator);

int wirecall_header_encode(const wirecall_header *header, uint8_t out[WIRECALL_HEADER_SIZE]);
int wirecall_header_decode(const uint8_t in[WIRECALL_HEADER_SIZE], wirecall_header *out);
uint32_t wirecall_packet_checksum(const wirecall_header *header, const uint8_t *payload, size_t payload_len);
uint64_t wirecall_packet_mac(const wirecall_header *header, const uint8_t *payload, size_t payload_len,
                             const uint8_t key[16]);
int wirecall_packet_sign(wirecall_header *header, const uint8_t *payload, size_t payload_len);
int wirecall_packet_verify(const wirecall_header *header, const uint8_t *payload, size_t payload_len);
int wirecall_packet_sign_ex(wirecall_header *header, const uint8_t *payload, size_t payload_len, uint32_t integrity,
                            const uint8_t mac_key[16]);
int wirecall_packet_verify_ex(const wirecall_header *header, const uint8_t *payload, size_t payload_len,
                              uint32_t integrity, const uint8_t mac_key[16]);

void wirecall_writer_init(wirecall_writer *writer);
void wirecall_writer_reset(wirecall_writer *writer);
void wirecall_writer_free(wirecall_writer *writer);
int wirecall_writer_null(wirecall_writer *writer);
int wirecall_writer_bool(wirecall_writer *writer, bool value);
int wirecall_writer_i64(wirecall_writer *writer, int64_t value);
int wirecall_writer_u64(wirecall_writer *writer, uint64_t value);
int wirecall_writer_f64(wirecall_writer *writer, double value);
int wirecall_writer_bytes(wirecall_writer *writer, const void *data, uint32_t len);
int wirecall_writer_string(wirecall_writer *writer, const char *data, uint32_t len);

int wirecall_payload_decode(const uint8_t *data, size_t len, wirecall_value **out_values, size_t *out_count);
void wirecall_values_free(wirecall_value *values);

#ifdef __cplusplus
}
#endif

#endif
