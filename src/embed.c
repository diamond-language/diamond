/* POSIX 2008 (O_CLOEXEC) on glibc; unlike _XOPEN_SOURCE it keeps the BSD types FreeBSD's headers need. */
#define _GNU_SOURCE
#include "embed.h"
#include "tls_crypto.h"
#include "vm.h"
#include "vm_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool diamond_embed_allowed(void) {
    return !diamond_sandbox_enabled()||sandbox_category_allowed("filesystem");
}

bool diamond_embed_read(const char *path,char **out_chars,size_t *out_length,
                        char digest_hex[DIAMOND_EMBED_DIGEST_HEX+1],
                        char *error,size_t error_capacity) {
    if(out_chars!=nullptr)*out_chars=nullptr;
    if(out_length!=nullptr)*out_length=0;
    const int file=open(path,O_RDONLY|O_CLOEXEC);
    if(file<0) {snprintf(error,error_capacity,"%s",strerror(errno));return false;}
    struct stat status;
    if(fstat(file,&status)!=0) {
        snprintf(error,error_capacity,"%s",strerror(errno));close(file);return false;
    }
    if(!S_ISREG(status.st_mode)) {
        snprintf(error,error_capacity,"not a regular file");close(file);return false;
    }
    if(status.st_size<0||(unsigned long long)status.st_size>DIAMOND_MAX_STRING_CONSTANT_LENGTH) {
        snprintf(error,error_capacity,"file is larger than the %d-byte limit",
                 (int)DIAMOND_MAX_STRING_CONSTANT_LENGTH);
        close(file);return false;
    }
    /* Read to end of file rather than trusting st_size, so a file that grows or shrinks while
     * being read is still read consistently with its digest. */
    size_t capacity=(size_t)status.st_size+1,length=0;
    char *chars=malloc(capacity);
    if(chars==nullptr) {snprintf(error,error_capacity,"out of memory");close(file);return false;}
    for(;;) {
        if(length+1==capacity) {
            if(capacity>DIAMOND_MAX_STRING_CONSTANT_LENGTH) {
                snprintf(error,error_capacity,"file is larger than the %d-byte limit",
                         (int)DIAMOND_MAX_STRING_CONSTANT_LENGTH);
                free(chars);close(file);return false;
            }
            char *grown=realloc(chars,capacity*2);
            if(grown==nullptr) {
                snprintf(error,error_capacity,"out of memory");free(chars);close(file);return false;
            }
            chars=grown;capacity*=2;
        }
        const ssize_t got=read(file,chars+length,capacity-1-length);
        if(got<0) {
            if(errno==EINTR)continue;
            snprintf(error,error_capacity,"%s",strerror(errno));free(chars);close(file);return false;
        }
        if(got==0)break;
        length+=(size_t)got;
    }
    close(file);
    if(length>DIAMOND_MAX_STRING_CONSTANT_LENGTH) {
        snprintf(error,error_capacity,"file is larger than the %d-byte limit",
                 (int)DIAMOND_MAX_STRING_CONSTANT_LENGTH);
        free(chars);return false;
    }
    chars[length]='\0';
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length=0;
    if(EVP_Digest(chars,length,digest,&digest_length,EVP_sha256(),nullptr)!=1||digest_length!=32) {
        snprintf(error,error_capacity,"cannot compute SHA-256");free(chars);return false;
    }
    static const char hex[]="0123456789abcdef";
    for(unsigned int index=0;index<32;index++) {
        digest_hex[index*2]=hex[digest[index]>>4];
        digest_hex[index*2+1]=hex[digest[index]&15];
    }
    digest_hex[DIAMOND_EMBED_DIGEST_HEX]='\0';
    if(out_chars!=nullptr)*out_chars=chars; else free(chars);
    if(out_length!=nullptr)*out_length=length;
    return true;
}
