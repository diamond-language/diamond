#include "compiler.h"
#include "disassemble.h"
#include "loader.h"
#include "value.h"
#include "vm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static constexpr char DIAMOND_VERSION[] = "0.1.0-dev";
static constexpr unsigned char DIAMOND_CORE_SOURCE[] = {
#embed "../lib/core.dia" suffix(,)
    0
};
static constexpr char DIAMOND_USER_LINE_RESET[] = "\n#line 1\n";

static void print_diagnostic(const char *name, const char *source,
                             DiamondDiagnostic diagnostic,
                             const DiamondSourceBundle *bundle,
                             size_t user_offset) {
    size_t line=diagnostic.span.line;
    if(diagnostic.span.start>=user_offset) {
        const size_t offset=diagnostic.span.start-user_offset;
        for(size_t index=0;index<bundle->segment_count;index++) {
            const DiamondSourceSegment *segment=&bundle->segments[index];
            if(offset<segment->start||offset>=segment->end)continue;
            name=segment->path;line=segment->original_line+line-1;break;
        }
    }
    fprintf(stderr, "%s:%zu:%zu: error: %s\n", name, line,
            diagnostic.span.column, diagnostic.message);

    size_t line_start = diagnostic.span.start;
    while (line_start > 0 && source[line_start - 1] != '\n') {
        line_start--;
    }
    size_t line_end = diagnostic.span.start;
    while (source[line_end] != '\0' && source[line_end] != '\n') {
        line_end++;
    }
    fprintf(stderr, "%.*s\n", (int)(line_end - line_start), source + line_start);
    for (size_t column = 1; column < diagnostic.span.column; column++) {
        fputc(' ', stderr);
    }
    fputs("^\n", stderr);
}

static int run_source(const char *name, const char *source, bool dump_bytecode) {
    DiamondSourceBundle bundle;char load_error[768];
    if(!diamond_load_program(name,source,&bundle,load_error,sizeof load_error)) {
        fprintf(stderr,"diamond: %s\n",load_error);return 74;
    }
    const size_t core_length=sizeof(DIAMOND_CORE_SOURCE)-1;
    const size_t source_length=strlen(bundle.source);
    const size_t reset_length=sizeof(DIAMOND_USER_LINE_RESET)-1;
    if(source_length>SIZE_MAX-core_length-reset_length-1) {
        fprintf(stderr,"diamond: expanded source is too large\n");
        diamond_source_bundle_free(&bundle);
        return 74;
    }
    char *combined=malloc(core_length+reset_length+source_length+1);
    if(combined==nullptr) {
        fprintf(stderr,"diamond: out of memory building expanded source\n");
        diamond_source_bundle_free(&bundle);
        return 74;
    }
    memcpy(combined,DIAMOND_CORE_SOURCE,core_length);
    memcpy(combined+core_length,DIAMOND_USER_LINE_RESET,reset_length);
    memcpy(combined+core_length+reset_length,bundle.source,source_length+1);
    DiamondProgram program;
    DiamondDiagnostic diagnostic;
    if (!diamond_compile(combined, &program, &diagnostic)) {
        print_diagnostic(name,combined,diagnostic,&bundle,
                         core_length+reset_length);
        free(combined);
        diamond_source_bundle_free(&bundle);
        return 65;
    }

    DiamondChunk chunk = diamond_program_chunk(&program);
    chunk.name = name;
    if (dump_bytecode) {
        (void)diamond_disassemble(stdout, name, &chunk);
    }
    DiamondVm vm;
    diamond_vm_init(&vm);
    vm.stress_gc = getenv("DIAMOND_STRESS_GC") != nullptr;
    DiamondValue result = DIAMOND_NIL;
    const DiamondVmStatus status = diamond_vm_run(&vm, &chunk, &result);
    if (status != DIAMOND_VM_OK) {
        const char *detail=diamond_vm_error(&vm);
        fprintf(stderr, "%s: runtime error: %s\n", name,
                detail != nullptr ? detail : diamond_vm_status_name(status));
        diamond_vm_free(&vm);
        free(combined);
        diamond_source_bundle_free(&bundle);
        return 70;
    }

    diamond_value_print(result);
    putchar('\n');
    if (getenv("DIAMOND_TRACE_IC") != nullptr) {
        fprintf(stderr,"inline caches: %zu hits, %zu misses\n",
                vm.inline_cache_hits,vm.inline_cache_misses);
    }
    if (getenv("DIAMOND_TRACE_SHAPES") != nullptr) {
        fprintf(stderr,"shape transitions: %zu\n",vm.shape_transitions);
    }
    if (getenv("DIAMOND_TRACE_FIELDS") != nullptr) {
        fprintf(stderr,"field caches: %zu hits, %zu misses\n",
                vm.field_cache_hits,vm.field_cache_misses);
    }
    diamond_vm_free(&vm);
    free(combined);
    diamond_source_bundle_free(&bundle);
    return 0;
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        fprintf(stderr, "diamond: cannot open '%s': %s\n", path, strerror(errno));
        return nullptr;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "diamond: cannot seek '%s': %s\n", path,
                strerror(errno));
        fclose(file);
        return nullptr;
    }
    const long length = ftell(file);
    if (length < 0 || (unsigned long long)length >= SIZE_MAX ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "diamond: cannot read '%s': %s\n", path,
                strerror(errno));
        fclose(file);
        return nullptr;
    }

    char *source = malloc((size_t)length + 1);
    if (source == nullptr) {
        fprintf(stderr, "diamond: out of memory reading '%s'\n", path);
        fclose(file);
        return nullptr;
    }
    const size_t read_count = fread(source, 1, (size_t)length, file);
    if (read_count != (size_t)length) {
        fprintf(stderr, "diamond: cannot read '%s': %s\n", path,
                ferror(file) ? strerror(errno) : "unexpected end of file");
        free(source);
        fclose(file);
        return nullptr;
    }
    source[read_count] = '\0';
    fclose(file);
    return source;
}

static void print_usage(void) {
    fputs("usage: diamond [-e CODE | FILE | --version]\n"
          "       diamond --dump-bytecode [-e CODE | FILE]\n", stderr);
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("diamond %s\n", DIAMOND_VERSION);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "-e") == 0) {
        return run_source("-e", argv[2], false);
    }
    if (argc == 4 && strcmp(argv[1], "--dump-bytecode") == 0 &&
        strcmp(argv[2], "-e") == 0) {
        return run_source("-e", argv[3], true);
    }
    const bool dump_file = argc == 3 &&
        strcmp(argv[1], "--dump-bytecode") == 0;
    if (argc == 2 || dump_file) {
        const char *path = dump_file ? argv[2] : argv[1];
        char *source = read_file(path);
        if (source == nullptr) {
            return 74;
        }
        const int status = run_source(path, source, dump_file);
        free(source);
        return status;
    }

    print_usage();
    return 64;
}
