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
    bool mapped_segment=false;
    size_t mapped_segment_start=0;
    size_t mapped_segment_end=0;
    size_t mapped_original_line=1;
    if(diagnostic.span.start>=user_offset) {
        const size_t offset=diagnostic.span.start-user_offset;
        for(size_t index=0;index<bundle->segment_count;index++) {
            const DiamondSourceSegment *segment=&bundle->segments[index];
            if(offset<segment->start||
               (offset>segment->end && offset-segment->end>9))continue;
            name=segment->path;
            mapped_segment=true;
            mapped_segment_start=user_offset+segment->start;
            mapped_segment_end=user_offset+segment->end;
            mapped_original_line=segment->original_line;
            break;
        }
    }
    size_t line_start = diagnostic.span.start;
    if(mapped_segment && line_start>=mapped_segment_end)
        line_start=mapped_segment_end;
    if(mapped_segment && line_start>0 && source[line_start]=='\n')line_start--;
    if(mapped_segment && source[line_start]=='#' && line_start>0) {
        line_start--;
        while(line_start>0&&source[line_start-1]!='\n')line_start--;
    }
    while (line_start > 0 && source[line_start - 1] != '\n') {
        line_start--;
    }
    if(mapped_segment) {
        line=mapped_original_line;
        for(size_t index=mapped_segment_start;index<line_start;index++)
            if(source[index]=='\n')line++;
    }
    fprintf(stderr, "%s:%zu:%zu: error: %s\n", name, line,
            diagnostic.span.column, diagnostic.message);
    size_t line_end = line_start;
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
    vm.quickening = getenv("DIAMOND_QUICKEN") != nullptr;
    const char *quickening_threshold = getenv("DIAMOND_QUICKEN_THRESHOLD");
    if (quickening_threshold != nullptr && quickening_threshold[0] != '\0') {
        char *end = nullptr;
        const unsigned long long parsed = strtoull(quickening_threshold, &end, 10);
        if (end != quickening_threshold && *end == '\0' && parsed > 0 &&
            parsed <= SIZE_MAX) {
            vm.quickening_threshold = (size_t)parsed;
        }
    }
    const char *mono_threshold = getenv("DIAMOND_IC_MONO_THRESHOLD");
    if (mono_threshold != nullptr && mono_threshold[0] != '\0') {
        char *end = nullptr;
        const unsigned long long parsed = strtoull(mono_threshold, &end, 10);
        if (end != mono_threshold && *end == '\0' && parsed > 0 &&
            parsed <= SIZE_MAX) {
            vm.monomorphic_threshold = (size_t)parsed;
        }
    }
    DiamondValue result = DIAMOND_NIL;
    size_t repeat_count=1;
    const char *repeat_value=getenv("DIAMOND_REPEAT");
    if(repeat_value!=nullptr&&repeat_value[0]!='\0') {
        char *end=nullptr;
        const unsigned long long parsed=strtoull(repeat_value,&end,10);
        if(end!=repeat_value&&*end=='\0'&&parsed>0&&parsed<=SIZE_MAX)
            repeat_count=(size_t)parsed;
    }
    DiamondVmStatus status=DIAMOND_VM_OK;
    for(size_t iteration=0;iteration<repeat_count;iteration++) {
        status=diamond_vm_run(&vm,&chunk,&result);
        if(status!=DIAMOND_VM_OK)break;
    }
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
    if (getenv("DIAMOND_TRACE_IC_SITES") != nullptr) {
        for (size_t index=0;index<DIAMOND_INLINE_CACHE_COUNT;index++) {
            const DiamondMethodCache *cache=&vm.method_caches[index];
            if (cache->site != nullptr)
                fprintf(stderr,"inline cache site[%zu]: %zu hits, %zu misses, %u classes\n",
                        index,cache->hits,cache->misses,cache->entry_count);
        }
    }
    if (getenv("DIAMOND_TRACE_IC_FAST") != nullptr)
        fprintf(stderr,"monomorphic dispatches: %zu\n",vm.monomorphic_dispatches);
    if (getenv("DIAMOND_TRACE_IC_PROBES") != nullptr)
        fprintf(stderr,"method cache probes: %zu\n",vm.method_cache_probes);
    if (getenv("DIAMOND_TRACE_IC_REWRITES") != nullptr)
        fprintf(stderr,"direct dispatch rewrites: %zu\n",vm.direct_dispatch_rewrites);
    if (getenv("DIAMOND_TRACE_SHAPES") != nullptr) {
        fprintf(stderr,"shape transitions: %zu\n",vm.shape_transitions);
    }
    if (getenv("DIAMOND_TRACE_FIELDS") != nullptr) {
        fprintf(stderr,"field caches: %zu hits, %zu misses\n",
                vm.field_cache_hits,vm.field_cache_misses);
    }
    if (getenv("DIAMOND_TRACE_OPCODES") != nullptr) {
        for (size_t opcode=0;opcode<DIAMOND_OP_COUNT;opcode++)
            if (vm.opcode_counts[opcode]!=0)
                fprintf(stderr,"opcode[%zu]: %zu\n",opcode,
                        vm.opcode_counts[opcode]);
    }
    if (getenv("DIAMOND_TRACE_QUICKEN") != nullptr)
        fprintf(stderr,"quickened sites: %zu, deoptimized sites: %zu\n",
                vm.quickened_sites,vm.deoptimized_sites);
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
    if (length < 0) {
        fprintf(stderr, "diamond: cannot determine size of '%s': %s\n", path,
                strerror(errno));
        fclose(file);
        return nullptr;
    }
    if ((unsigned long long)length >= SIZE_MAX) {
        fprintf(stderr, "diamond: file '%s' is too large\n", path);
        fclose(file);
        return nullptr;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
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
