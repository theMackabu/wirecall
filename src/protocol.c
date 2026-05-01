#include "wirecall/protocol.h"

#include "proc.h"

#include <string.h>

uint64_t wirecall_proc_id(const char *name) {
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

static uint64_t get_u64_native(const uint8_t *in, size_t len) {
  uint64_t value = 0;
  memcpy(&value, in, len);
  return value;
}

static uint64_t rotl64(uint64_t value, unsigned shift) {
  return (value << shift) | (value >> (64u - shift));
}

static uint64_t mix64(uint64_t value) {
  value ^= value >> 30u;
  value *= 0xbf58476d1ce4e5b9ull;
  value ^= value >> 27u;
  value *= 0x94d049bb133111ebull;
  value ^= value >> 31u;
  return value;
}

static uint64_t packet_seed(const wirecall_header *header) {
  uint64_t meta =
    ((uint64_t)(uint8_t)header->op << 56u) | ((uint64_t)header->flags << 48u) | ((uint64_t)header->size << 16u);
  return mix64(0x9e3779b97f4a7c15ull ^ meta ^ rotl64(header->proc_id, 17u) ^ rotl64(header->call_id, 41u));
}

static uint64_t cheap_hash_update(uint64_t hash, const uint8_t *data, size_t len) {
  while (len >= 8u) {
    hash ^= mix64(get_u64_native(data, 8) + 0x9e3779b97f4a7c15ull);
    hash = rotl64(hash, 27u) * 0x3c79ac492ba7b653ull + 0x1c69b3f74ac4ae35ull;
    data += 8u;
    len -= 8u;
  }
  if (len > 0) {
    hash ^= mix64(get_u64_native(data, len) ^ ((uint64_t)len << 56u));
    hash = rotl64(hash, 27u) * 0x3c79ac492ba7b653ull + 0x1c69b3f74ac4ae35ull;
  }
  return mix64(hash);
}

static uint64_t sip_load64(const uint8_t in[8]) {
  uint64_t value;
  memcpy(&value, in, sizeof(value));
  return value;
}

static void sip_round(uint64_t *v0, uint64_t *v1, uint64_t *v2, uint64_t *v3) {
  *v0 += *v1;
  *v1 = rotl64(*v1, 13u);
  *v1 ^= *v0;
  *v0 = rotl64(*v0, 32u);
  *v2 += *v3;
  *v3 = rotl64(*v3, 16u);
  *v3 ^= *v2;
  *v0 += *v3;
  *v3 = rotl64(*v3, 21u);
  *v3 ^= *v0;
  *v2 += *v1;
  *v1 = rotl64(*v1, 17u);
  *v1 ^= *v2;
  *v2 = rotl64(*v2, 32u);
}

static void sip_compress(uint64_t *v0, uint64_t *v1, uint64_t *v2, uint64_t *v3, uint64_t m) {
  *v3 ^= m;
  sip_round(v0, v1, v2, v3);
  sip_round(v0, v1, v2, v3);
  *v0 ^= m;
}

static void sip_update(uint64_t *v0, uint64_t *v1, uint64_t *v2, uint64_t *v3, const uint8_t *data, size_t len,
                       uint64_t *total_len, uint64_t *tail, unsigned *tail_len) {
  *total_len += len;
  if (*tail_len != 0) {
    while (len > 0 && *tail_len < 8u) {
      *tail |= (uint64_t)*data++ << (*tail_len * 8u);
      (*tail_len)++;
      len--;
    }
    if (*tail_len == 8u) {
      sip_compress(v0, v1, v2, v3, *tail);
      *tail = 0;
      *tail_len = 0;
    }
  }
  while (len >= 8u) {
    sip_compress(v0, v1, v2, v3, sip_load64(data));
    data += 8u;
    len -= 8u;
  }
  while (len > 0) {
    *tail |= (uint64_t)*data++ << (*tail_len * 8u);
    (*tail_len)++;
    len--;
  }
}

static int valid_op(uint8_t op) {
  return op == WIRECALL_OP_RPC || op == WIRECALL_OP_PING || op == WIRECALL_OP_DISCONNECT ||
         op == WIRECALL_OP_RESPONSE || op == WIRECALL_OP_ERROR;
}

int wirecall_header_encode(const wirecall_header *header, uint8_t out[WIRECALL_HEADER_SIZE]) {
  if (!header || !out || !valid_op((uint8_t)header->op) || header->size > WIRECALL_MAX_PAYLOAD_SIZE) { return -1; }

  out[0] = (uint8_t)header->op;
  out[1] = header->flags;
  put_u64(out + 2, header->proc_id);
  put_u32(out + 10, header->size);
  put_u64(out + 14, header->call_id);
  put_u32(out + 22, header->checksum);
  put_u64(out + 26, header->mac);
  return 0;
}

int wirecall_header_decode(const uint8_t in[WIRECALL_HEADER_SIZE], wirecall_header *out) {
  if (!in || !out || !valid_op(in[0])) { return -1; }

  memset(out, 0, sizeof(*out));
  out->op = (wirecall_op)in[0];
  out->flags = in[1];
  out->proc_id = get_u64(in + 2);
  out->size = get_u32(in + 10);
  out->call_id = get_u64(in + 14);
  out->checksum = get_u32(in + 22);
  out->mac = get_u64(in + 26);
  if (out->size > WIRECALL_MAX_PAYLOAD_SIZE) { return -1; }
  return 0;
}

uint32_t wirecall_packet_checksum(const wirecall_header *header, const uint8_t *payload, size_t payload_len) {
  if (!header || (!payload && payload_len > 0) || payload_len > WIRECALL_MAX_PAYLOAD_SIZE) { return 0; }

  uint64_t hash = packet_seed(header) ^ 0xa0761d6478bd642full;
  hash = cheap_hash_update(hash, payload, payload_len);
  return (uint32_t)(hash ^ (hash >> 32u));
}

uint64_t wirecall_packet_mac(const wirecall_header *header, const uint8_t *payload, size_t payload_len,
                             const uint8_t key[16]) {
  if (!header || !key || (!payload && payload_len > 0) || payload_len > WIRECALL_MAX_PAYLOAD_SIZE) { return 0; }

  uint64_t k0 = sip_load64(key);
  uint64_t k1 = sip_load64(key + 8);
  uint64_t v0 = 0x736f6d6570736575ull ^ k0;
  uint64_t v1 = 0x646f72616e646f6dull ^ k1;
  uint64_t v2 = 0x6c7967656e657261ull ^ k0;
  uint64_t v3 = 0x7465646279746573ull ^ k1;
  uint64_t total_len = 0;
  uint64_t tail = 0;
  unsigned tail_len = 0;

  uint8_t meta[30];
  meta[0] = (uint8_t)header->op;
  meta[1] = header->flags;
  put_u64(meta + 2, header->proc_id);
  put_u32(meta + 10, header->size);
  put_u64(meta + 14, header->call_id);
  put_u32(meta + 22, header->checksum);
  put_u32(meta + 26, (uint32_t)payload_len);

  sip_update(&v0, &v1, &v2, &v3, meta, sizeof(meta), &total_len, &tail, &tail_len);
  sip_update(&v0, &v1, &v2, &v3, payload, payload_len, &total_len, &tail, &tail_len);
  sip_compress(&v0, &v1, &v2, &v3, tail | (total_len << 56u));
  v2 ^= 0xffu;
  sip_round(&v0, &v1, &v2, &v3);
  sip_round(&v0, &v1, &v2, &v3);
  sip_round(&v0, &v1, &v2, &v3);
  sip_round(&v0, &v1, &v2, &v3);
  return v0 ^ v1 ^ v2 ^ v3;
}

int wirecall_packet_sign(wirecall_header *header, const uint8_t *payload, size_t payload_len) {
  return wirecall_packet_sign_ex(header, payload, payload_len, WIRECALL_INTEGRITY_DEFAULT, NULL);
}

int wirecall_packet_verify(const wirecall_header *header, const uint8_t *payload, size_t payload_len) {
  return wirecall_packet_verify_ex(header, payload, payload_len, WIRECALL_INTEGRITY_DEFAULT, NULL);
}

int wirecall_packet_sign_ex(wirecall_header *header, const uint8_t *payload, size_t payload_len, uint32_t integrity,
                            const uint8_t mac_key[16]) {
  if (!header || (!payload && payload_len > 0) || payload_len > WIRECALL_MAX_PAYLOAD_SIZE ||
      header->size != payload_len || ((integrity & WIRECALL_INTEGRITY_MAC) && !mac_key)) {
    return -1;
  }
  header->checksum =
    (integrity & WIRECALL_INTEGRITY_CHECKSUM) ? wirecall_packet_checksum(header, payload, payload_len) : 0;
  header->mac = (integrity & WIRECALL_INTEGRITY_MAC) ? wirecall_packet_mac(header, payload, payload_len, mac_key) : 0;
  return 0;
}

int wirecall_packet_verify_ex(const wirecall_header *header, const uint8_t *payload, size_t payload_len,
                              uint32_t integrity, const uint8_t mac_key[16]) {
  if (!header || (!payload && payload_len > 0) || payload_len > WIRECALL_MAX_PAYLOAD_SIZE ||
      header->size != payload_len || ((integrity & WIRECALL_INTEGRITY_MAC) && !mac_key)) {
    return -1;
  }
  if ((integrity & WIRECALL_INTEGRITY_CHECKSUM) &&
      wirecall_packet_checksum(header, payload, payload_len) != header->checksum) {
    return -1;
  }
  if ((integrity & WIRECALL_INTEGRITY_MAC) &&
      wirecall_packet_mac(header, payload, payload_len, mac_key) != header->mac) {
    return -1;
  }
  return 0;
}
