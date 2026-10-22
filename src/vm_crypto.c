#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE

#include "vm_internal.h"
#include "tls_crypto.h"

/* <crypt.h> exists on glibc (libxcrypt) and musl (see BCrypt.hash's own
 * comment below for what musl's version lacks), declaring crypt_r/
 * struct crypt_data/CRYPT_GENSALT_* -- but not on FreeBSD, which declares
 * plain crypt()/crypt_r() directly in <unistd.h> (already included below)
 * instead, with no separate header at all. __has_include, not an
 * __APPLE__/__FreeBSD__-style OS check (see docs/portability.md's own
 * "What hasn't been found" on why this codebase avoids those): this is a
 * feature test, and the same reasoning applies wherever else a libc omits
 * this header. */
#if __has_include(<crypt.h>)
#include <crypt.h>
#endif
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* BCrypt.hash(password, cost) -- crypt_gensalt_rn generates a full "$2b$
 * <cost>$<22-char-salt>" setting string, passing rbytes=nullptr so
 * libxcrypt pulls its own entropy from the OS (CRYPT_GENSALT_IMPLEMENTS_
 * AUTO_ENTROPY, see /usr/include/crypt.h) -- no separate RAND_bytes call
 * needed here, unlike SecureRandom below. `struct crypt_data` is 32KB
 * (CRYPT_DATA_INTERNAL_SIZE), too large to put on a Fiber's own (smaller,
 * mmapped) C stack safely, so it's heap-allocated per call rather than a
 * local, unlike the small fixed-size buffers elsewhere in this function.
 * `password->chars` is passed as a NUL-terminated C string (libcrypt's own
 * API shape) -- an embedded NUL byte in the password truncates what
 * actually gets hashed, the same inherent limitation every C-crypt-backed
 * bcrypt binding has; not otherwise validated against here. */
DiamondVmStatus bcrypt_hash_helper(DiamondVm *vm,DiamondValue password_value,
        DiamondValue cost_value,DiamondValue *out_result) {
    if(password_value.kind!=DIAMOND_VALUE_OBJECT||
       password_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,password_value);
        snprintf(vm->error,sizeof vm->error,
            "BCrypt.hash password must be a String, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(cost_value.kind!=DIAMOND_VALUE_INT) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,cost_value);
        snprintf(vm->error,sizeof vm->error,
            "BCrypt.hash cost must be an Int, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t cost=cost_value.as.integer;
    if(cost<4||cost>31) {
        (void)snprintf(vm->error,sizeof vm->error,
            "BCrypt.hash cost must be between 4 and 31, got %" PRId64,cost);
        return DIAMOND_VM_ARITY_ERROR;
    }
    const DiamondString *password=(const DiamondString *)password_value.as.object;
#ifdef CRYPT_GENSALT_IMPLEMENTS_AUTO_ENTROPY
    char salt[CRYPT_GENSALT_OUTPUT_SIZE];
    if(crypt_gensalt_rn("$2b$",(unsigned long)cost,nullptr,0,salt,sizeof salt)==nullptr) {
        (void)snprintf(vm->error,sizeof vm->error,"BCrypt.hash: failed to generate a salt");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    struct crypt_data *data=calloc(1,sizeof *data);
    if(data==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    const char *digest=crypt_r(password->chars,salt,data);
    if(digest==nullptr||digest[0]=='*') {
        free(data);
        (void)snprintf(vm->error,sizeof vm->error,"BCrypt.hash: crypt_r failed unexpectedly");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    DiamondString *result=allocate_string(vm,digest,strlen(digest));
    free(data);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(result);
    return DIAMOND_VM_OK;
#else
    /* No libxcrypt (CRYPT_GENSALT_IMPLEMENTS_AUTO_ENTROPY, defined by
     * libxcrypt's own crypt.h, is this codebase's established way of
     * detecting it -- see this function's own top comment) -- musl's
     * <crypt.h>, for one, has crypt_r but neither this salt-generation
     * convenience function nor bcrypt ($2b$) support in crypt_r itself
     * at all (confirmed directly: crypt_r("x","$2b$04$...",&data)
     * returns "*", libcrypt's own "unsupported algorithm" signal, on
     * musl -- not a missing-symbol problem alone). See docs/portability.md for the full finding; a truly portable BCrypt would
     * need to bundle its own implementation rather than delegate to the
     * system crypt(3), which is real, separate work. */
    (void)password;(void)cost;(void)out_result;
    (void)snprintf(vm->error,sizeof vm->error,
        "BCrypt.hash is not supported on this platform's crypt() implementation");
    return DIAMOND_VM_PROGRAM_ERROR;
#endif
}

/* BCrypt.verify(password, digest) -- re-hashes `password` against
 * `digest` itself used as the crypt(3) "setting" (a valid bcrypt hash is
 * also a valid setting string: crypt_r reads only its own "$2b$<cost>$
 * <salt>" prefix and ignores the rest), then compares with OpenSSL's
 * CRYPTO_memcmp (constant-time) rather than plain memcmp/strcmp -- real
 * defense-in-depth against a timing attack on the stored-hash comparison,
 * essentially free given libcrypto is already linked for TLS. A malformed
 * `digest` (crypt_r returns nullptr, or libxcrypt's own "invalid setting"
 * signal -- an output starting with '*', see crypt.h) or a length
 * mismatch is treated as an ordinary non-match (false), not an exception
 * -- verifying against a bad/foreign hash is a normal outcome here, not a
 * program error. */
DiamondVmStatus bcrypt_verify_helper(DiamondVm *vm,DiamondValue password_value,
        DiamondValue digest_value,DiamondValue *out_result) {
    if(password_value.kind!=DIAMOND_VALUE_OBJECT||
       password_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,password_value);
        snprintf(vm->error,sizeof vm->error,
            "BCrypt.verify password must be a String, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(digest_value.kind!=DIAMOND_VALUE_OBJECT||
       digest_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,digest_value);
        snprintf(vm->error,sizeof vm->error,
            "BCrypt.verify digest must be a String, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *password=(const DiamondString *)password_value.as.object;
    const DiamondString *digest=(const DiamondString *)digest_value.as.object;
    /* struct crypt_data/crypt_r have no portable feature test the way
     * <crypt.h>'s own __has_include above does -- confirmed directly,
     * a real compile failure on macOS ("incomplete type 'struct
     * crypt_data'"): unlike FreeBSD (which declares both in <unistd.h>
     * with no <crypt.h> at all) and glibc/musl (both via <crypt.h>),
     * macOS's libc provides neither the reentrant crypt_r nor
     * struct crypt_data in any header. __APPLE__ here, not a header
     * probe, is the one deliberate exception to this codebase's own
     * "no OS-name branching" convention (docs/portability.md) -- every
     * other platform gap so far had a real feature-test proxy; this one
     * doesn't. Degrades the same documented way an unsupported
     * algorithm already does on musl: BCrypt.verify never raises, an
     * unrecognized digest is just reported as no match. */
#if !defined(__APPLE__)
    struct crypt_data *data=calloc(1,sizeof *data);
    if(data==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    const char *computed=crypt_r(password->chars,digest->chars,data);
    bool matches=false;
    if(computed!=nullptr&&computed[0]!='*') {
        const size_t computed_length=strlen(computed);
        if(computed_length==digest->length)
            matches=CRYPTO_memcmp(computed,digest->chars,computed_length)==0;
    }
    free(data);
#else
    (void)password;(void)digest;
    const bool matches=false;
#endif
    *out_result=DIAMOND_BOOL(matches);
    return DIAMOND_VM_OK;
}

/* SecureRandom.bytes(n) -- OpenSSL RAND_bytes, already linked for TLS.
 * Diamond's own String is already documented as a raw byte buffer
 * (docs/io.md), so arbitrary random bytes need no separate encoding
 * here, unlike .hex below. */
DiamondVmStatus secure_random_bytes_helper(DiamondVm *vm,DiamondValue count_value,
        DiamondValue *out_result) {
    if(count_value.kind!=DIAMOND_VALUE_INT) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,count_value);
        snprintf(vm->error,sizeof vm->error,
            "SecureRandom.bytes count must be an Int, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t count=count_value.as.integer;
    if(count<0||count>INT32_MAX) {
        (void)snprintf(vm->error,sizeof vm->error,
            "SecureRandom.bytes count must be between 0 and %d",INT32_MAX);
        return DIAMOND_VM_ARITY_ERROR;
    }
    unsigned char *buffer=malloc(count>0?(size_t)count:1);
    if(buffer==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(count>0&&RAND_bytes(buffer,(int)count)!=1) {
        free(buffer);
        (void)snprintf(vm->error,sizeof vm->error,"SecureRandom.bytes: RAND_bytes failed");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    DiamondString *result=allocate_string(vm,(const char *)buffer,(size_t)count);
    free(buffer);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(result);
    return DIAMOND_VM_OK;
}

/* SecureRandom.hex(n) -- n random bytes (RAND_bytes, as above), hex-
 * encoded into a 2*n-length String. */
DiamondVmStatus secure_random_hex_helper(DiamondVm *vm,DiamondValue count_value,
        DiamondValue *out_result) {
    if(count_value.kind!=DIAMOND_VALUE_INT) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,count_value);
        snprintf(vm->error,sizeof vm->error,
            "SecureRandom.hex count must be an Int, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const int64_t count=count_value.as.integer;
    if(count<0||count>INT32_MAX/2) {
        (void)snprintf(vm->error,sizeof vm->error,
            "SecureRandom.hex count must be between 0 and %d",INT32_MAX/2);
        return DIAMOND_VM_ARITY_ERROR;
    }
    unsigned char *buffer=malloc(count>0?(size_t)count:1);
    if(buffer==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    if(count>0&&RAND_bytes(buffer,(int)count)!=1) {
        free(buffer);
        (void)snprintf(vm->error,sizeof vm->error,"SecureRandom.hex: RAND_bytes failed");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    char *hex=malloc(count>0?(size_t)count*2:1);
    if(hex==nullptr) {
        free(buffer);
        return DIAMOND_VM_OUT_OF_MEMORY;
    }
    static const char digits[]="0123456789abcdef";
    for(int64_t index=0;index<count;index++) {
        hex[index*2]=digits[(buffer[index]>>4)&0xF];
        hex[index*2+1]=digits[buffer[index]&0xF];
    }
    free(buffer);
    DiamondString *result=allocate_string(vm,hex,(size_t)count*2);
    free(hex);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(result);
    return DIAMOND_VM_OK;
}

DiamondVmStatus sha256_hex_helper(DiamondVm *vm,DiamondValue key_value,
        DiamondValue data_value,bool keyed,DiamondValue *out_result) {
    if(data_value.kind!=DIAMOND_VALUE_OBJECT||
       data_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,data_value);
        snprintf(vm->error,sizeof vm->error,
            "%s data must be a String, got %s",keyed?"HMAC.sha256":"Digest.sha256",
            actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(keyed&&(key_value.kind!=DIAMOND_VALUE_OBJECT||
          key_value.as.object->kind!=DIAMOND_OBJECT_STRING)) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,key_value);
        snprintf(vm->error,sizeof vm->error,
            "HMAC.sha256 key must be a String, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *data=(const DiamondString *)data_value.as.object;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length=0;
    if(keyed) {
        const DiamondString *key=(const DiamondString *)key_value.as.object;
        if(key->length>INT_MAX) {
            (void)snprintf(vm->error,sizeof vm->error,"HMAC.sha256 key is too large");
            return DIAMOND_VM_ARITY_ERROR;
        }
        if(HMAC(EVP_sha256(),key->chars,(int)key->length,
                (const unsigned char *)data->chars,data->length,
                digest,&digest_length)==nullptr) {
            (void)snprintf(vm->error,sizeof vm->error,"HMAC.sha256 failed");
            return DIAMOND_VM_PROGRAM_ERROR;
        }
    } else if(EVP_Digest(data->chars,data->length,digest,&digest_length,
                         EVP_sha256(),nullptr)!=1) {
        (void)snprintf(vm->error,sizeof vm->error,"Digest.sha256 failed");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    char hex[64];
    static const char digits[]="0123456789abcdef";
    for(size_t index=0;index<digest_length;index++) {
        hex[index*2]=digits[digest[index]>>4];
        hex[index*2+1]=digits[digest[index]&0x0f];
    }
    DiamondString *result=allocate_string(vm,hex,(size_t)digest_length*2);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(result);
    return DIAMOND_VM_OK;
}

/* Identical shape to sha256_hex_helper above, using EVP_sha1() -- kept as
 * its own copy rather than parameterizing the existing, already-shipped
 * sha256 helper: SHA-1 exists here solely for RFC 6238 TOTP compatibility,
 * not as a second general-purpose choice alongside sha256, so the two
 * are deliberately not presented as options of one shared entry point. */
DiamondVmStatus sha1_hex_helper(DiamondVm *vm,DiamondValue key_value,
        DiamondValue data_value,bool keyed,DiamondValue *out_result) {
    if(data_value.kind!=DIAMOND_VALUE_OBJECT||
       data_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,data_value);
        snprintf(vm->error,sizeof vm->error,
            "%s data must be a String, got %s",keyed?"HMAC.sha1":"Digest.sha1",
            actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    if(keyed&&(key_value.kind!=DIAMOND_VALUE_OBJECT||
          key_value.as.object->kind!=DIAMOND_OBJECT_STRING)) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,key_value);
        snprintf(vm->error,sizeof vm->error,
            "HMAC.sha1 key must be a String, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *data=(const DiamondString *)data_value.as.object;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length=0;
    if(keyed) {
        const DiamondString *key=(const DiamondString *)key_value.as.object;
        if(key->length>INT_MAX) {
            (void)snprintf(vm->error,sizeof vm->error,"HMAC.sha1 key is too large");
            return DIAMOND_VM_ARITY_ERROR;
        }
        if(HMAC(EVP_sha1(),key->chars,(int)key->length,
                (const unsigned char *)data->chars,data->length,
                digest,&digest_length)==nullptr) {
            (void)snprintf(vm->error,sizeof vm->error,"HMAC.sha1 failed");
            return DIAMOND_VM_PROGRAM_ERROR;
        }
    } else if(EVP_Digest(data->chars,data->length,digest,&digest_length,
                         EVP_sha1(),nullptr)!=1) {
        (void)snprintf(vm->error,sizeof vm->error,"Digest.sha1 failed");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    char hex[64];
    static const char digits[]="0123456789abcdef";
    for(size_t index=0;index<digest_length;index++) {
        hex[index*2]=digits[digest[index]>>4];
        hex[index*2+1]=digits[digest[index]&0x0f];
    }
    DiamondString *result=allocate_string(vm,hex,(size_t)digest_length*2);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(result);
    return DIAMOND_VM_OK;
}

/* HMAC.verify(data, key, signature) -- recomputes HMAC-SHA256 over `data`
 * with `key` (the exact computation sha256_hex_helper's own keyed path
 * already does) and compares the resulting lowercase-hex digest against
 * `signature` with CRYPTO_memcmp rather than plain memcmp/strcmp -- the
 * same constant-time-comparison reasoning bcrypt_verify_helper's own
 * comment gives. A `signature` of the wrong length, or any argument of
 * the wrong type, is an ordinary non-match (false) rather than an
 * exception -- verifying against a bad/foreign signature is a normal
 * outcome here, not a program error, matching BCrypt.verify. */
DiamondVmStatus hmac_verify_helper(DiamondVm *vm,DiamondValue data_value,
        DiamondValue key_value,DiamondValue signature_value,DiamondValue *out_result) {
    if(data_value.kind!=DIAMOND_VALUE_OBJECT||
       data_value.as.object->kind!=DIAMOND_OBJECT_STRING||
       key_value.kind!=DIAMOND_VALUE_OBJECT||
       key_value.as.object->kind!=DIAMOND_OBJECT_STRING||
       signature_value.kind!=DIAMOND_VALUE_OBJECT||
       signature_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "HMAC.verify data, key, and signature must all be Strings");
        return DIAMOND_VM_TYPE_ERROR;
    }
    DiamondValue computed_value=DIAMOND_NIL;
    const DiamondVmStatus status=sha256_hex_helper(vm,key_value,data_value,true,
        &computed_value);
    if(status!=DIAMOND_VM_OK)return status;
    const DiamondString *computed=(const DiamondString *)computed_value.as.object;
    const DiamondString *signature=(const DiamondString *)signature_value.as.object;
    const bool matches=computed->length==signature->length&&
        CRYPTO_memcmp(computed->chars,signature->chars,computed->length)==0;
    *out_result=DIAMOND_BOOL(matches);
    return DIAMOND_VM_OK;
}

enum { DIAMOND_CIPHER_KEY_LENGTH = 32, DIAMOND_CIPHER_NONCE_LENGTH = 12,
       DIAMOND_CIPHER_TAG_LENGTH = 16 };

/* Cipher.encrypt(key, plaintext) -- AES-256-GCM via OpenSSL's EVP_CIPHER
 * API (EVP_aes_256_gcm(), already linked for TLS -- no new dependency).
 * `key` must be exactly DIAMOND_CIPHER_KEY_LENGTH raw bytes: AES-256
 * takes a fixed-size key, and silently truncating/padding a wrong-length
 * key would be a much worse failure mode than rejecting it outright.
 * Generates a fresh random DIAMOND_CIPHER_NONCE_LENGTH-byte nonce per
 * call (RAND_bytes, same as secure_random_bytes_helper) -- GCM's own
 * security requires never reusing a (key, nonce) pair, and a 96-bit
 * random nonce makes an accidental collision astronomically unlikely
 * for any realistic number of cookies one key ever encrypts. Returns one
 * binary-safe String: nonce, then the GCM authentication tag, then the
 * ciphertext -- packed together so decrypt only needs the key and this
 * one blob, no separate nonce/tag bookkeeping at the call site. */
DiamondVmStatus aes_gcm_encrypt_helper(DiamondVm *vm,DiamondValue key_value,
        DiamondValue plaintext_value,DiamondValue *out_result) {
#ifdef DIAMOND_NO_CRYPTO
    snprintf(vm->error,sizeof vm->error,"Cipher is not compiled into this build");
    (void)key_value;(void)out_result;
    return DIAMOND_VM_PROGRAM_ERROR;
#endif
    if(key_value.kind!=DIAMOND_VALUE_OBJECT||
       key_value.as.object->kind!=DIAMOND_OBJECT_STRING||
       plaintext_value.kind!=DIAMOND_VALUE_OBJECT||
       plaintext_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "Cipher.encrypt key and plaintext must both be Strings");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *key=(const DiamondString *)key_value.as.object;
    if(key->length!=DIAMOND_CIPHER_KEY_LENGTH) {
        (void)snprintf(vm->error,sizeof vm->error,
            "Cipher.encrypt key must be exactly %d bytes, got %zu",
            DIAMOND_CIPHER_KEY_LENGTH,key->length);
        return DIAMOND_VM_ARITY_ERROR;
    }
    const DiamondString *plaintext=(const DiamondString *)plaintext_value.as.object;
    if(plaintext->length>INT_MAX) {
        (void)snprintf(vm->error,sizeof vm->error,"Cipher.encrypt plaintext is too large");
        return DIAMOND_VM_ARITY_ERROR;
    }
    unsigned char nonce[DIAMOND_CIPHER_NONCE_LENGTH];
    if(RAND_bytes(nonce,sizeof nonce)!=1) {
        (void)snprintf(vm->error,sizeof vm->error,"Cipher.encrypt: RAND_bytes failed");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    const size_t blob_length=DIAMOND_CIPHER_NONCE_LENGTH+DIAMOND_CIPHER_TAG_LENGTH+
        plaintext->length;
    unsigned char *blob=malloc(blob_length>0?blob_length:1);
    if(blob==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    memcpy(blob,nonce,sizeof nonce);
    unsigned char *ciphertext=blob+DIAMOND_CIPHER_NONCE_LENGTH+DIAMOND_CIPHER_TAG_LENGTH;
    EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();
    bool ok=ctx!=nullptr;
    int written=0,total=0;
    if(ok)ok=EVP_EncryptInit_ex(ctx,EVP_aes_256_gcm(),nullptr,nullptr,nullptr)==1;
    if(ok)ok=EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_IVLEN,
        DIAMOND_CIPHER_NONCE_LENGTH,nullptr)==1;
    if(ok)ok=EVP_EncryptInit_ex(ctx,nullptr,nullptr,
        (const unsigned char *)key->chars,nonce)==1;
    if(ok&&plaintext->length>0)
        ok=EVP_EncryptUpdate(ctx,ciphertext,&written,
            (const unsigned char *)plaintext->chars,(int)plaintext->length)==1;
    total=written;
    if(ok)ok=EVP_EncryptFinal_ex(ctx,ciphertext+total,&written)==1;
    total+=written;
    if(ok)ok=EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_GET_TAG,
        DIAMOND_CIPHER_TAG_LENGTH,blob+DIAMOND_CIPHER_NONCE_LENGTH)==1;
    if(ctx!=nullptr)EVP_CIPHER_CTX_free(ctx);
    if(!ok||(size_t)total!=plaintext->length) {
        free(blob);
        (void)snprintf(vm->error,sizeof vm->error,"Cipher.encrypt failed");
        return DIAMOND_VM_PROGRAM_ERROR;
    }
    DiamondString *result=allocate_string(vm,(const char *)blob,blob_length);
    free(blob);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(result);
    return DIAMOND_VM_OK;
}

/* Cipher.decrypt(key, blob) -- the inverse of aes_gcm_encrypt_helper
 * above. Returns nil (not an exception) on any failure -- wrong key, a
 * tampered or truncated blob, or GCM's own tag check failing -- the same
 * "bad input is a normal outcome here, not a programmer error" reasoning
 * BCrypt.verify's own comment already gives for a malformed digest: a
 * forged/expired/wrong-key cookie is expected to happen in normal
 * operation, not something calling code should have to rescue. */
DiamondVmStatus aes_gcm_decrypt_helper(DiamondVm *vm,DiamondValue key_value,
        DiamondValue blob_value,DiamondValue *out_result) {
#ifdef DIAMOND_NO_CRYPTO
    snprintf(vm->error,sizeof vm->error,"Cipher is not compiled into this build");
    (void)key_value;(void)out_result;
    return DIAMOND_VM_PROGRAM_ERROR;
#endif
    if(key_value.kind!=DIAMOND_VALUE_OBJECT||
       key_value.as.object->kind!=DIAMOND_OBJECT_STRING||
       blob_value.kind!=DIAMOND_VALUE_OBJECT||
       blob_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
        snprintf(vm->error,sizeof vm->error,
            "Cipher.decrypt key and blob must both be Strings");
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *key=(const DiamondString *)key_value.as.object;
    if(key->length!=DIAMOND_CIPHER_KEY_LENGTH) {
        (void)snprintf(vm->error,sizeof vm->error,
            "Cipher.decrypt key must be exactly %d bytes, got %zu",
            DIAMOND_CIPHER_KEY_LENGTH,key->length);
        return DIAMOND_VM_ARITY_ERROR;
    }
    const DiamondString *blob=(const DiamondString *)blob_value.as.object;
    const size_t header_length=DIAMOND_CIPHER_NONCE_LENGTH+DIAMOND_CIPHER_TAG_LENGTH;
    if(blob->length<header_length||blob->length-header_length>INT_MAX) {
        *out_result=DIAMOND_NIL;
        return DIAMOND_VM_OK;
    }
    const unsigned char *nonce=(const unsigned char *)blob->chars;
    const unsigned char *tag=(const unsigned char *)blob->chars+DIAMOND_CIPHER_NONCE_LENGTH;
    const unsigned char *ciphertext=(const unsigned char *)blob->chars+header_length;
    const size_t ciphertext_length=blob->length-header_length;
    unsigned char *plaintext=malloc(ciphertext_length>0?ciphertext_length:1);
    if(plaintext==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    EVP_CIPHER_CTX *ctx=EVP_CIPHER_CTX_new();
    bool ok=ctx!=nullptr;
    int written=0,total=0;
    if(ok)ok=EVP_DecryptInit_ex(ctx,EVP_aes_256_gcm(),nullptr,nullptr,nullptr)==1;
    if(ok)ok=EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_IVLEN,
        DIAMOND_CIPHER_NONCE_LENGTH,nullptr)==1;
    if(ok)ok=EVP_DecryptInit_ex(ctx,nullptr,nullptr,
        (const unsigned char *)key->chars,nonce)==1;
    if(ok&&ciphertext_length>0)
        ok=EVP_DecryptUpdate(ctx,plaintext,&written,ciphertext,(int)ciphertext_length)==1;
    total=written;
    if(ok)ok=EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_TAG,
        DIAMOND_CIPHER_TAG_LENGTH,(void *)tag)==1;
    /* EVP_DecryptFinal_ex is where GCM's own tag verification actually
     * happens -- a tampered ciphertext or tag fails here, inside OpenSSL,
     * with no separate comparison of our own needed (and so no timing
     * side-channel exposed to Diamond code either). */
    if(ok)ok=EVP_DecryptFinal_ex(ctx,plaintext+total,&written)==1;
    total+=written;
    if(ctx!=nullptr)EVP_CIPHER_CTX_free(ctx);
    if(!ok) {
        free(plaintext);
        *out_result=DIAMOND_NIL;
        return DIAMOND_VM_OK;
    }
    DiamondString *result=allocate_string(vm,(const char *)plaintext,(size_t)total);
    free(plaintext);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(result);
    return DIAMOND_VM_OK;
}

/* Base64.encode(data) -- RFC 4648 standard alphabet with padding, via
 * OpenSSL's EVP_EncodeBlock (already linked). Output length for `n`
 * input bytes is always exactly 4*ceil(n/3); EVP_EncodeBlock's own
 * return value is trusted rather than independently recomputed. */
DiamondVmStatus base64_encode_helper(DiamondVm *vm,DiamondValue data_value,
        DiamondValue *out_result) {
    if(data_value.kind!=DIAMOND_VALUE_OBJECT||
       data_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,data_value);
        snprintf(vm->error,sizeof vm->error,
            "Base64.encode data must be a String, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *data=(const DiamondString *)data_value.as.object;
    if(data->length>(size_t)INT_MAX/4*3) {
        snprintf(vm->error,sizeof vm->error,"Base64.encode input is too large");
        return DIAMOND_VM_ARITY_ERROR;
    }
    const size_t capacity=(data->length+2)/3*4;
    /* +1: EVP_EncodeBlock always NUL-terminates its output on top of the
     * `capacity` encoded bytes it returns via `written` -- allocating
     * exactly `capacity` bytes (the previous behavior here) left it
     * writing one byte past the end of the buffer on every call.
     * Confirmed with Valgrind memcheck ("Invalid write of size 1 ...
     * 0 bytes after a block of size 16"); the extra byte itself is
     * never read (allocate_string below copies only `written` bytes). */
    unsigned char *output=malloc(capacity+1);
    if(output==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    const int written=EVP_EncodeBlock(output,(const unsigned char *)data->chars,
        (int)data->length);
    DiamondString *result=allocate_string(vm,(const char *)output,(size_t)written);
    free(output);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(result);
    return DIAMOND_VM_OK;
}

/* Base64.decode(data) -- the inverse. Validates the character set itself
 * (rather than trusting EVP_DecodeBlock's own lenience, which varies by
 * OpenSSL version) before decoding, so a malformed input always raises a
 * clear ArgumentError instead of silently decoding to garbage or
 * behaving differently across environments. `=` padding is only valid in
 * the final one or two positions, matching RFC 4648. */
DiamondVmStatus base64_decode_helper(DiamondVm *vm,DiamondValue data_value,
        DiamondValue *out_result) {
    if(data_value.kind!=DIAMOND_VALUE_OBJECT||
       data_value.as.object->kind!=DIAMOND_OBJECT_STRING) {
        char actual[80];
        diamond_format_value_type(actual,sizeof actual,data_value);
        snprintf(vm->error,sizeof vm->error,
            "Base64.decode data must be a String, got %s",actual);
        return DIAMOND_VM_TYPE_ERROR;
    }
    const DiamondString *data=(const DiamondString *)data_value.as.object;
    if(data->length%4!=0) {
        snprintf(vm->error,sizeof vm->error,
            "Base64.decode input length must be a multiple of 4");
        return DIAMOND_VM_ARITY_ERROR;
    }
    for(size_t index=0;index<data->length;index++) {
        const unsigned char c=(unsigned char)data->chars[index];
        const bool is_alphabet=(c>='A'&&c<='Z')||(c>='a'&&c<='z')||
            (c>='0'&&c<='9')||c=='+'||c=='/';
        const bool is_valid_padding=c=='='&&index>=data->length-2&&data->length>0;
        if(!is_alphabet&&!is_valid_padding) {
            snprintf(vm->error,sizeof vm->error,
                "Base64.decode: invalid character at position %zu",index);
            return DIAMOND_VM_ARITY_ERROR;
        }
    }
    if(data->length==0) {
        DiamondString *empty=allocate_string(vm,"",0);
        if(empty==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
        *out_result=DIAMOND_OBJECT(empty);
        return DIAMOND_VM_OK;
    }
    const size_t capacity=data->length/4*3;
    unsigned char *output=malloc(capacity);
    if(output==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    const int written=EVP_DecodeBlock(output,(const unsigned char *)data->chars,
        (int)data->length);
    if(written<0) {
        free(output);
        snprintf(vm->error,sizeof vm->error,"Base64.decode: invalid input");
        return DIAMOND_VM_ARITY_ERROR;
    }
    size_t padding=0;
    if(data->chars[data->length-1]=='=')padding++;
    if(data->length>=2&&data->chars[data->length-2]=='=')padding++;
    const size_t real_length=(size_t)written-padding;
    DiamondString *result=allocate_string(vm,(const char *)output,real_length);
    free(output);
    if(result==nullptr)return DIAMOND_VM_OUT_OF_MEMORY;
    *out_result=DIAMOND_OBJECT(result);
    return DIAMOND_VM_OK;
}
