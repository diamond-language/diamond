#ifndef DIAMOND_PORTABLE_CRYPTO_H
#define DIAMOND_PORTABLE_CRYPTO_H

/* The slice of OpenSSL's libcrypto Diamond uses for Digest, HMAC, SecureRandom, Base64 and
 * constant-time comparison, implemented without OpenSSL. Compiled in place of <openssl/...>
 * when DIAMOND_NO_CRYPTO is defined (make NO_CRYPTO=1), under the same names so the callers in
 * src/vm.c need no changes. AES-GCM (Cipher) has no portable version here: its entry points
 * fail, and the Cipher helpers report that. */

#include <stddef.h>

#define EVP_MAX_MD_SIZE 64

typedef struct evp_md_st EVP_MD;
typedef struct evp_md_ctx_st EVP_MD_CTX;
const EVP_MD *EVP_sha256(void);
const EVP_MD *EVP_sha1(void);
EVP_MD_CTX *EVP_MD_CTX_new(void);
void EVP_MD_CTX_free(EVP_MD_CTX *context);
int EVP_DigestInit_ex(EVP_MD_CTX *context, const EVP_MD *type, void *impl);
int EVP_DigestUpdate(EVP_MD_CTX *context, const void *data, size_t count);
int EVP_DigestFinal_ex(EVP_MD_CTX *context, unsigned char *md, unsigned int *size);
int EVP_Digest(const void *data, size_t count, unsigned char *md, unsigned int *size,
               const EVP_MD *type, void *impl);
unsigned char *HMAC(const EVP_MD *evp_md, const void *key, int key_len,
                    const unsigned char *data, size_t data_len, unsigned char *md,
                    unsigned int *md_len);
int RAND_bytes(unsigned char *buffer, int count);
int CRYPTO_memcmp(const void *a, const void *b, size_t length);
int EVP_EncodeBlock(unsigned char *out, const unsigned char *in, int length);
int EVP_DecodeBlock(unsigned char *out, const unsigned char *in, int length);

typedef struct evp_cipher_st EVP_CIPHER;
typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;
#define EVP_CTRL_GCM_SET_IVLEN 0x9
#define EVP_CTRL_GCM_GET_TAG 0x10
#define EVP_CTRL_GCM_SET_TAG 0x11
const EVP_CIPHER *EVP_aes_256_gcm(void);
EVP_CIPHER_CTX *EVP_CIPHER_CTX_new(void);
void EVP_CIPHER_CTX_free(EVP_CIPHER_CTX *context);
int EVP_CIPHER_CTX_ctrl(EVP_CIPHER_CTX *context, int type, int arg, void *pointer);
int EVP_EncryptInit_ex(EVP_CIPHER_CTX *context, const EVP_CIPHER *cipher, void *impl,
                       const unsigned char *key, const unsigned char *iv);
int EVP_EncryptUpdate(EVP_CIPHER_CTX *context, unsigned char *out, int *out_length,
                      const unsigned char *in, int in_length);
int EVP_EncryptFinal_ex(EVP_CIPHER_CTX *context, unsigned char *out, int *out_length);
int EVP_DecryptInit_ex(EVP_CIPHER_CTX *context, const EVP_CIPHER *cipher, void *impl,
                       const unsigned char *key, const unsigned char *iv);
int EVP_DecryptUpdate(EVP_CIPHER_CTX *context, unsigned char *out, int *out_length,
                      const unsigned char *in, int in_length);
int EVP_DecryptFinal_ex(EVP_CIPHER_CTX *context, unsigned char *out, int *out_length);

#endif
