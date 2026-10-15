#ifndef DIAMOND_TLS_CRYPTO_H
#define DIAMOND_TLS_CRYPTO_H

/* The one place the runtime sources include OpenSSL. NO_TLS replaces the TLS surface with
 * inert stubs (openssl_stub.h); NO_CRYPTO replaces digests, HMAC, random bytes and Base64 with
 * portable code (portable_crypto.h) and makes AES-GCM unavailable. With both, nothing from
 * OpenSSL is needed to build. */

#ifndef DIAMOND_NO_TLS
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#else
#include "openssl_stub.h"
#endif

#ifndef DIAMOND_NO_CRYPTO
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#else
#include "portable_crypto.h"
#endif

#endif
