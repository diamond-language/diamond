#define _XOPEN_SOURCE 700
#include "vm_internal.h"

#include <stdio.h>

static int failures;

static void expect_frame(const char *headers, bool valid) {
    FILE *stream=tmpfile();
    if(stream==nullptr) { failures++; return; }
    fputs(headers,stream);
    fputs("\r\n{\"command\":\"continue\"}",stream);
    rewind(stream);
    DiamondDebugCommandKind kind;
    size_t lines[1],count;
    bool offsets=false;
    char target[64];
    const bool ok=debug_pipe_read_command(fileno(stream),&kind,lines,&count,1,&offsets,target);
    if(ok!=valid||(valid&&kind!=DIAMOND_DEBUG_COMMAND_CONTINUE)||
        (!valid&&kind!=DIAMOND_DEBUG_COMMAND_NONE)) {
        fprintf(stderr,"debug_frame_test: unexpected framing result for %s\n",headers);
        failures++;
    }
    fclose(stream);
}

int main(void) {
    expect_frame("Content-Length: 22\r\n",true);
    expect_frame("Content-Length: 22garbage\r\n",false);
    expect_frame("Content-Length: +22\r\n",false);
    expect_frame("Content-Length: -22\r\n",false);
    expect_frame("Content-Length: 67108865\r\n",false);
    expect_frame("Content-Length: 999999999999999999999999\r\n",false);
    expect_frame("Content-Length: 22\r\nContent-Length: 22\r\n",false);
    expect_frame("Content-Type: application/json\r\n",false);
    if(failures==0) puts("debug_frame_test: ok");
    return failures==0?0:1;
}
