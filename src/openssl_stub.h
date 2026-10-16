#ifndef DIAMOND_OPENSSL_STUB_H
#define DIAMOND_OPENSSL_STUB_H

/* The TLS slice of OpenSSL's API as inert stubs, used when DIAMOND_NO_TLS is defined
 * (make NO_TLS=1). Every constructor fails, so TLSSocket.connect and TLSServer.listen take
 * their ordinary "could not create the TLS context" error path, and ERR_error_string_n
 * says why. Nothing here links against OpenSSL. */

#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_session_st SSL_SESSION;
typedef struct ssl_method_st SSL_METHOD;
typedef struct x509_st X509;
typedef struct X509_name_st X509_NAME;
typedef struct X509_VERIFY_PARAM_st X509_VERIFY_PARAM;
typedef struct bio_st BIO;
typedef struct bio_method_st BIO_METHOD;
typedef struct evp_md_st EVP_MD;
#define STACK_OF(type) struct stack_st_##type
STACK_OF(X509_NAME);

enum {
    SSL_ERROR_NONE = 0, SSL_ERROR_SSL = 1, SSL_ERROR_WANT_READ = 2, SSL_ERROR_WANT_WRITE = 3,
    SSL_ERROR_SYSCALL = 5, SSL_ERROR_ZERO_RETURN = 6,
    SSL_VERIFY_NONE = 0, SSL_VERIFY_PEER = 1, SSL_VERIFY_FAIL_IF_NO_PEER_CERT = 2,
    SSL_FILETYPE_PEM = 1, X509_V_OK = 0,
    SSL_TLSEXT_ERR_OK = 0, SSL_TLSEXT_ERR_ALERT_FATAL = 2, OPENSSL_NPN_NEGOTIATED = 1,
    SSL_SESS_CACHE_CLIENT = 1,
};

#define DIAMOND_TLS_UNAVAILABLE 0x7d1a0001UL

static inline unsigned long ERR_get_error(void) { return DIAMOND_TLS_UNAVAILABLE; }
static inline unsigned long ERR_peek_error(void) { return DIAMOND_TLS_UNAVAILABLE; }
static inline void ERR_clear_error(void) {}
static inline void ERR_error_string_n(unsigned long code, char *buffer, size_t length) {
    (void)code;
    if (length > 0) snprintf(buffer, length, "TLS support is not compiled into this build");
}

static inline const SSL_METHOD *TLS_client_method(void) { return nullptr; }
static inline const SSL_METHOD *TLS_server_method(void) { return nullptr; }
static inline SSL_CTX *SSL_CTX_new(const SSL_METHOD *method) { (void)method; return nullptr; }
static inline void SSL_CTX_free(SSL_CTX *context) { (void)context; }
static inline int SSL_CTX_set_default_verify_paths(SSL_CTX *c) { (void)c; return 0; }
static inline int SSL_CTX_load_verify_locations(SSL_CTX *c, const char *file, const char *path) {
    (void)c; (void)file; (void)path; return 0;
}
static inline int SSL_CTX_use_certificate_chain_file(SSL_CTX *c, const char *file) {
    (void)c; (void)file; return 0;
}
static inline int SSL_CTX_use_PrivateKey_file(SSL_CTX *c, const char *file, int type) {
    (void)c; (void)file; (void)type; return 0;
}
static inline int SSL_CTX_check_private_key(const SSL_CTX *c) { (void)c; return 0; }
static inline void SSL_CTX_set_verify(SSL_CTX *c, int mode, void *callback) {
    (void)c; (void)mode; (void)callback;
}
static inline long SSL_CTX_set_session_cache_mode(SSL_CTX *c, long mode) { (void)c; return mode; }
static inline void SSL_CTX_sess_set_new_cb(SSL_CTX *c, int (*callback)(SSL *, SSL_SESSION *)) {
    (void)c; (void)callback;
}
static inline int SSL_CTX_set_alpn_protos(SSL_CTX *c, const unsigned char *p, unsigned int n) {
    (void)c; (void)p; (void)n; return 1;
}
static inline void SSL_CTX_set_alpn_select_cb(SSL_CTX *c,
        int (*callback)(SSL *, const unsigned char **, unsigned char *, const unsigned char *,
                        unsigned int, void *), void *argument) {
    (void)c; (void)callback; (void)argument;
}
static inline int SSL_select_next_proto(unsigned char **out, unsigned char *out_length,
        const unsigned char *server, unsigned int server_length, const unsigned char *client,
        unsigned int client_length) {
    (void)out; (void)out_length; (void)server; (void)server_length; (void)client;
    (void)client_length; return 0;
}
static inline STACK_OF(X509_NAME) *SSL_load_client_CA_file(const char *file) {
    (void)file; return nullptr;
}
static inline void SSL_CTX_set_client_CA_list(SSL_CTX *c, STACK_OF(X509_NAME) *list) {
    (void)c; (void)list;
}
static inline void sk_X509_NAME_pop_free(STACK_OF(X509_NAME) *list, void (*free_function)(X509_NAME *)) {
    (void)list; (void)free_function;
}
static inline void X509_NAME_free(X509_NAME *name) { (void)name; }

static inline SSL *SSL_new(SSL_CTX *context) { (void)context; return nullptr; }
static inline void SSL_free(SSL *ssl) { (void)ssl; }
static inline int SSL_set_fd(SSL *ssl, int fd) { (void)ssl; (void)fd; return 0; }
static inline int SSL_get_fd(const SSL *ssl) { (void)ssl; return -1; }
static inline int SSL_connect(SSL *ssl) { (void)ssl; return -1; }
static inline int SSL_accept(SSL *ssl) { (void)ssl; return -1; }
static inline int SSL_read(SSL *ssl, void *buffer, int count) {
    (void)ssl; (void)buffer; (void)count; return -1;
}
static inline int SSL_write(SSL *ssl, const void *buffer, int count) {
    (void)ssl; (void)buffer; (void)count; return -1;
}
static inline int SSL_pending(const SSL *ssl) { (void)ssl; return 0; }
static inline int SSL_shutdown(SSL *ssl) { (void)ssl; return -1; }
static inline int SSL_get_error(const SSL *ssl, int result) { (void)ssl; (void)result; return SSL_ERROR_SSL; }
static inline long SSL_get_verify_result(const SSL *ssl) { (void)ssl; return 1; }
static inline X509 *SSL_get1_peer_certificate(const SSL *ssl) { (void)ssl; return nullptr; }
static inline X509_VERIFY_PARAM *SSL_get0_param(SSL *ssl) { (void)ssl; return nullptr; }
static inline int SSL_set1_host(SSL *ssl, const char *host) { (void)ssl; (void)host; return 0; }
static inline int X509_VERIFY_PARAM_set1_ip_asc(X509_VERIFY_PARAM *p, const char *ip) {
    (void)p; (void)ip; return 0;
}
static inline long SSL_ctrl(SSL *ssl, int command, long argument, void *pointer) {
    (void)ssl; (void)command; (void)argument; (void)pointer; return 0;
}
#define SSL_set_tlsext_host_name(ssl, name) SSL_ctrl((ssl), 55, 0, (void *)(name))
static inline void SSL_set_app_data(SSL *ssl, void *data) { (void)ssl; (void)data; }
static inline void *SSL_get_app_data(const SSL *ssl) { (void)ssl; return nullptr; }
static inline void SSL_get0_alpn_selected(const SSL *ssl, const unsigned char **data,
        unsigned int *length) {
    (void)ssl; *data = nullptr; *length = 0;
}
static inline int SSL_set_session(SSL *ssl, SSL_SESSION *session) { (void)ssl; (void)session; return 0; }
static inline int SSL_session_reused(const SSL *ssl) { (void)ssl; return 0; }
#define TLSEXT_NAMETYPE_host_name 0
static inline const char *SSL_get_servername(const SSL *ssl, int type) {
    (void)ssl; (void)type; return nullptr;
}
static inline void SSL_SESSION_free(SSL_SESSION *session) { (void)session; }
static inline int i2d_SSL_SESSION(const SSL_SESSION *session, unsigned char **out) {
    (void)session; (void)out; return 0;
}
static inline SSL_SESSION *d2i_SSL_SESSION(SSL_SESSION **session, const unsigned char **in, long length) {
    (void)session; (void)in; (void)length; return nullptr;
}

static inline void X509_free(X509 *certificate) { (void)certificate; }
static inline X509_NAME *X509_get_subject_name(const X509 *certificate) { (void)certificate; return nullptr; }
static inline const char *X509_verify_cert_error_string(long code) { (void)code; return "TLS is not compiled into this build"; }
static inline int X509_digest(const X509 *certificate, const EVP_MD *type, unsigned char *md,
        unsigned int *length) {
    (void)certificate; (void)type; (void)md; (void)length; return 0;
}
static inline const BIO_METHOD *BIO_s_mem(void) { return nullptr; }
static inline BIO *BIO_new(const BIO_METHOD *method) { (void)method; return nullptr; }
static inline int BIO_read(BIO *bio, void *buffer, int count) {
    (void)bio; (void)buffer; (void)count; return -1;
}
static inline int BIO_free(BIO *bio) { (void)bio; return 0; }
static inline int X509_NAME_print_ex(BIO *bio, const X509_NAME *name, int indent, unsigned long flags) {
    (void)bio; (void)name; (void)indent; (void)flags; return 0;
}
#define XN_FLAG_RFC2253 0
#define XN_FLAG_ONELINE 0

#endif
