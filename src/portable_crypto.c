#define _DEFAULT_SOURCE
#include "portable_crypto.h"

#ifdef DIAMOND_NO_CRYPTO

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#endif

enum { DIGEST_SHA256 = 1, DIGEST_SHA1 = 2 };
struct evp_md_st { int kind; size_t size; size_t block; };
static const EVP_MD sha256_descriptor = {DIGEST_SHA256, 32, 64};
static const EVP_MD sha1_descriptor = {DIGEST_SHA1, 20, 64};
const EVP_MD *EVP_sha256(void) { return &sha256_descriptor; }
const EVP_MD *EVP_sha1(void) { return &sha1_descriptor; }

static uint32_t rotate_right(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}
static uint32_t rotate_left(uint32_t value, unsigned count) {
    return (value << count) | (value >> (32u - count));
}

static void sha256_block(uint32_t state[8], const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (size_t i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    for (size_t i = 16; i < 64; i++) {
        const uint32_t s0 = rotate_right(w[i - 15], 7) ^ rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotate_right(w[i - 2], 17) ^ rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (size_t i = 0; i < 64; i++) {
        const uint32_t s1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        const uint32_t t1 = h + s1 + ((e & f) ^ (~e & g)) + k[i] + w[i];
        const uint32_t s0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        const uint32_t t2 = s0 + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void sha1_block(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (size_t i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    for (size_t i = 16; i < 80; i++)
        w[i] = rotate_left(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (size_t i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        const uint32_t next = rotate_left(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotate_left(b, 30); b = a; a = next;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

/* One-shot digest of `data` with Merkle-Damgard padding; both hashes are big-endian. */
static void digest_message(const EVP_MD *type, const uint8_t *data, size_t length,
        unsigned char *out) {
    const bool sha256 = type->kind == DIGEST_SHA256;
    uint32_t state[8];
    if (sha256) {
        static const uint32_t initial[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                            0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        memcpy(state, initial, sizeof initial);
    } else {
        static const uint32_t initial[5] = {0x67452301,0xefcdab89,0x98badcfe,0x10325476,0xc3d2e1f0};
        memcpy(state, initial, sizeof initial);
    }
    size_t offset = 0;
    for (; offset + 64 <= length; offset += 64) {
        if (sha256) sha256_block(state, data + offset); else sha1_block(state, data + offset);
    }
    uint8_t tail[128] = {0};
    const size_t remaining = length - offset;
    memcpy(tail, data + offset, remaining);
    tail[remaining] = 0x80;
    const size_t tail_length = remaining + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bits = (uint64_t)length * 8u;
    for (size_t i = 0; i < 8; i++) tail[tail_length - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t block = 0; block < tail_length; block += 64) {
        if (sha256) sha256_block(state, tail + block); else sha1_block(state, tail + block);
    }
    const size_t words = type->size / 4;
    for (size_t i = 0; i < words; i++) {
        out[i * 4] = (uint8_t)(state[i] >> 24); out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(state[i] >> 8); out[i * 4 + 3] = (uint8_t)state[i];
    }
}

int EVP_Digest(const void *data, size_t count, unsigned char *md, unsigned int *size,
               const EVP_MD *type, void *impl) {
    (void)impl;
    if (type == nullptr || md == nullptr) return 0;
    digest_message(type, data, count, md);
    if (size != nullptr) *size = (unsigned int)type->size;
    return 1;
}

unsigned char *HMAC(const EVP_MD *evp_md, const void *key, int key_len,
                    const unsigned char *data, size_t data_len, unsigned char *md,
                    unsigned int *md_len) {
    if (evp_md == nullptr || md == nullptr || key_len < 0) return nullptr;
    uint8_t block[64] = {0};
    if ((size_t)key_len > evp_md->block) digest_message(evp_md, key, (size_t)key_len, block);
    else memcpy(block, key, (size_t)key_len);
    uint8_t inner_pad[64], outer_pad[64];
    for (size_t i = 0; i < 64; i++) { inner_pad[i] = block[i] ^ 0x36; outer_pad[i] = block[i] ^ 0x5c; }
    uint8_t *message = malloc(64 + data_len);
    if (message == nullptr) return nullptr;
    memcpy(message, inner_pad, 64);
    if (data_len > 0) memcpy(message + 64, data, data_len);
    uint8_t inner[EVP_MAX_MD_SIZE];
    digest_message(evp_md, message, 64 + data_len, inner);
    free(message);
    uint8_t outer[64 + EVP_MAX_MD_SIZE];
    memcpy(outer, outer_pad, 64);
    memcpy(outer + 64, inner, evp_md->size);
    digest_message(evp_md, outer, 64 + evp_md->size, md);
    if (md_len != nullptr) *md_len = (unsigned int)evp_md->size;
    return md;
}

int RAND_bytes(unsigned char *buffer, int count) {
    if (count < 0) return 0;
    size_t done = 0;
    while (done < (size_t)count) {
        size_t chunk = (size_t)count - done;
        if (chunk > 256) chunk = 256;   /* getentropy's own limit */
        if (getentropy(buffer + done, chunk) != 0) return 0;
        done += chunk;
    }
    return 1;
}

int CRYPTO_memcmp(const void *a, const void *b, size_t length) {
    const volatile unsigned char *left = a, *right = b;
    unsigned char difference = 0;
    for (size_t i = 0; i < length; i++) difference |= (unsigned char)(left[i] ^ right[i]);
    return difference != 0;
}

static const char BASE64_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int EVP_EncodeBlock(unsigned char *out, const unsigned char *in, int length) {
    size_t written = 0;
    for (int i = 0; i < length; i += 3) {
        const uint32_t b0 = in[i];
        const uint32_t b1 = i + 1 < length ? in[i + 1] : 0;
        const uint32_t b2 = i + 2 < length ? in[i + 2] : 0;
        const uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out[written++] = (unsigned char)BASE64_ALPHABET[(triple >> 18) & 63];
        out[written++] = (unsigned char)BASE64_ALPHABET[(triple >> 12) & 63];
        out[written++] = i + 1 < length ? (unsigned char)BASE64_ALPHABET[(triple >> 6) & 63] : '=';
        out[written++] = i + 2 < length ? (unsigned char)BASE64_ALPHABET[triple & 63] : '=';
    }
    out[written] = '\0';
    return (int)written;
}

static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Like OpenSSL's: padding characters decode as zero bits and still count toward the length
 * returned, so the caller subtracts them; -1 for a length that is not a multiple of four or
 * a character outside the alphabet. */
int EVP_DecodeBlock(unsigned char *out, const unsigned char *in, int length) {
    if (length < 0 || length % 4 != 0) return -1;
    size_t written = 0;
    for (int i = 0; i < length; i += 4) {
        uint32_t triple = 0;
        for (int j = 0; j < 4; j++) {
            const unsigned char c = in[i + j];
            const int value = c == '=' ? 0 : base64_value(c);
            if (value < 0) return -1;
            triple = (triple << 6) | (uint32_t)value;
        }
        out[written++] = (unsigned char)(triple >> 16);
        out[written++] = (unsigned char)(triple >> 8);
        out[written++] = (unsigned char)triple;
    }
    return (int)written;
}

EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void) { return nullptr; }
void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *context) { (void)context; }
const EVP_CIPHER *EVP_aes_256_gcm(void) { return nullptr; }
int EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX *c, int t, int a, void *p) { (void)c;(void)t;(void)a;(void)p; return 0; }
int EVP_EncryptInit_ex(EVP_CIPHER_CTX *c, const EVP_CIPHER *e, void *i, const unsigned char *k,
        const unsigned char *v) { (void)c;(void)e;(void)i;(void)k;(void)v; return 0; }
int EVP_EncryptUpdate(EVP_CIPHER_CTX *c, unsigned char *o, int *ol, const unsigned char *in,
        int il) { (void)c;(void)o;(void)ol;(void)in;(void)il; return 0; }
int EVP_EncryptFinal_ex(EVP_CIPHER_CTX *c, unsigned char *o, int *ol) { (void)c;(void)o;(void)ol; return 0; }
int EVP_DecryptInit_ex(EVP_CIPHER_CTX *c, const EVP_CIPHER *e, void *i, const unsigned char *k,
        const unsigned char *v) { (void)c;(void)e;(void)i;(void)k;(void)v; return 0; }
int EVP_DecryptUpdate(EVP_CIPHER_CTX *c, unsigned char *o, int *ol, const unsigned char *in,
        int il) { (void)c;(void)o;(void)ol;(void)in;(void)il; return 0; }
int EVP_DecryptFinal_ex(EVP_CIPHER_CTX *c, unsigned char *o, int *ol) { (void)c;(void)o;(void)ol; return 0; }

#endif
