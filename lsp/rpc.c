#include "rpc.h"

#include <stdlib.h>
#include <string.h>

/* Reads one header line (without its terminating \r\n) into buffer.
 * Returns 1 on a complete line (buffer may be empty -- the header
 * terminator), 0 on clean EOF before any byte was read, -1 on a
 * too-long or EOF-mid-line malformed header. */
static int read_header_line(FILE *stream,char *buffer,size_t capacity) {
    size_t length=0;
    bool saw_any=false;
    int c;
    while((c=fgetc(stream))!=EOF) {
        saw_any=true;
        if(c=='\n') {
            if(length>0&&buffer[length-1]=='\r')length--;
            buffer[length]='\0';
            return 1;
        }
        if(length+1>=capacity)return -1;
        buffer[length++]=(char)c;
    }
    return saw_any?-1:0;
}

JsonValue *rpc_read_message(FILE *stream,const char **error) {
    if(error!=nullptr)*error=nullptr;
    long content_length=-1;
    char line[256];
    while(true) {
        const int status=read_header_line(stream,line,sizeof line);
        if(status==0)return nullptr;
        if(status==-1) {
            if(error!=nullptr)*error="malformed header line";
            return nullptr;
        }
        if(line[0]=='\0')break;
        static constexpr char prefix[]="Content-Length:";
        static constexpr size_t prefix_length=sizeof(prefix)-1;
        if(strncmp(line,prefix,prefix_length)==0) {
            const char *value=line+prefix_length;
            while(*value==' ')value++;
            content_length=strtol(value,nullptr,10);
        }
    }
    if(content_length<0) {
        if(error!=nullptr)*error="missing Content-Length header";
        return nullptr;
    }
    char *body=malloc((size_t)content_length+1);
    if(body==nullptr) {
        if(error!=nullptr)*error="out of memory";
        return nullptr;
    }
    const size_t read_count=fread(body,1,(size_t)content_length,stream);
    if(read_count!=(size_t)content_length) {
        free(body);
        if(error!=nullptr)*error="unexpected end of stream reading message body";
        return nullptr;
    }
    body[content_length]='\0';
    const char *parse_error=nullptr;
    JsonValue *message=json_parse(body,(size_t)content_length,&parse_error);
    free(body);
    if(message==nullptr) {
        if(error!=nullptr)*error=parse_error;
        return nullptr;
    }
    return message;
}

bool rpc_write_message(FILE *stream,const JsonValue *message) {
    size_t length=0;
    char *body=json_serialize(message,&length);
    if(body==nullptr)return false;
    bool ok=fprintf(stream,"Content-Length: %zu\r\n\r\n",length)>=0;
    if(ok)ok=fwrite(body,1,length,stream)==length;
    free(body);
    if(ok)ok=fflush(stream)==0;
    return ok;
}
