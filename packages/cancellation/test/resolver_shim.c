#define _GNU_SOURCE
#include <dlfcn.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Test-only libc interposition: exercise the real runtime lifecycle without
 * relying on external DNS servers or their timeout/cache configuration. */
int getaddrinfo(const char *host,const char *service,const struct addrinfo *hints,
               struct addrinfo **result) {
    int (*real_lookup)(const char *,const char *,const struct addrinfo *,struct addrinfo **);
    *(void **)(&real_lookup)=dlsym(RTLD_NEXT,"getaddrinfo");
    if(host!=NULL&&strcmp(host,"slow.test")==0) {
        const char *marker=getenv("DNS_TEST_STARTED");
        FILE *file=fopen(marker,"a");
        if(file!=NULL) {fputs("started\n",file);fclose(file);}
        while(access(getenv("DNS_TEST_RELEASE"),F_OK)!=0)usleep(1000);
        return real_lookup("127.0.0.1",service,hints,result);
    }
    if(host!=NULL&&strcmp(host,"missing.test")==0)return EAI_NONAME;
    if(host!=NULL&&(strcmp(host,"multi.test")==0||strcmp(host,"budget.test")==0)) {
        if(strcmp(host,"budget.test")==0)usleep(750000);
        struct addrinfo *first=NULL,*second=NULL,*third=NULL;
        int error=real_lookup("::1",service,hints,&first);
        if(error!=0)return error;
        error=real_lookup("127.0.0.1",service,hints,&second);
        if(error!=0) {freeaddrinfo(first);return error;}
        error=real_lookup("127.0.0.1",service,hints,&third);
        if(error!=0) {freeaddrinfo(first);freeaddrinfo(second);return error;}
        struct addrinfo *tail=first;
        while(tail->ai_next!=NULL)tail=tail->ai_next;
        tail->ai_next=second;
        tail=second;
        while(tail->ai_next!=NULL)tail=tail->ai_next;
        tail->ai_next=third;
        *result=first;
        return 0;
    }
    return real_lookup(host,service,hints,result);
}
