#include "run_source.h"

#include "compiler.h"
#include "disassemble.h"
#include "loader.h"
#include "value.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static constexpr unsigned char DIAMOND_CORE_SOURCE[] = {
#embed "../lib/core.di" suffix(,)
    0
};
static constexpr char DIAMOND_USER_LINE_RESET[] = "\n#line 1\n";

static void print_diagnostic(const char *name, const char *source,
                             DiamondDiagnostic diagnostic,
                             const DiamondSourceBundle *bundle,
                             size_t user_offset) {
    const DiamondResolvedLocation resolved=diamond_resolve_diagnostic_location(
        name,source,diagnostic,bundle,user_offset);
    fprintf(stderr, "%s:%zu:%zu: error: %s\n", resolved.path, resolved.line,
            resolved.column, diagnostic.message);
    fprintf(stderr, "%.*s\n",
            (int)(resolved.line_end - resolved.line_start), source + resolved.line_start);
    for (size_t column = 1; column < resolved.column; column++) {
        fputc(' ', stderr);
    }
    fputs("^\n", stderr);
}

int diamond_run_source_with_program(const char *name, const char *source,
        bool dump_bytecode, DiamondProgram *program,
        int script_argc, char *const *script_argv) {
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
    DiamondDiagnostic diagnostic;
    if (!diamond_compile(combined, program, &diagnostic)) {
        print_diagnostic(name,combined,diagnostic,&bundle,
                         core_length+reset_length);
        free(combined);
        diamond_source_bundle_free(&bundle);
        return 65;
    }

    DiamondChunk chunk = diamond_program_chunk(program);
    chunk.name = name;
    if (dump_bytecode) {
        (void)diamond_disassemble(stdout, name, &chunk);
    }
    DiamondVm vm;
    diamond_vm_init(&vm);
    diamond_vm_set_argv(&vm,script_argc,script_argv);
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
        if (iteration>0 && getenv("DIAMOND_INVALIDATE_IC_EACH_RUN") != nullptr)
            diamond_vm_invalidate_method_caches(&vm);
        status=diamond_vm_run(&vm,&chunk,&result);
        if (getenv("DIAMOND_TRACE_IC_EACH_RUN") != nullptr)
            fprintf(stderr,"run %zu: inline caches: %zu hits, %zu misses, rewrites: %zu\n",
                    iteration+1,vm.inline_cache_hits,vm.inline_cache_misses,
                    vm.direct_dispatch_rewrites);
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
    if (getenv("DIAMOND_TRACE_IC_POLICY") != nullptr)
        fprintf(stderr,"dispatch policy: quicken threshold %zu, monomorphic threshold %zu\n",
                vm.quickening_threshold,vm.monomorphic_threshold);
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
    if (getenv("DIAMOND_TRACE_GC") != nullptr)
        fprintf(stderr,"GC: %zu collections, %.6fs total\n",
                vm.gc_collection_count,vm.gc_total_seconds);
    diamond_vm_free(&vm);
    free(combined);
    diamond_source_bundle_free(&bundle);
    return 0;
}

/* The ordinary entry point (used by the CLI and everything else that
 * only ever runs one program per process): allocates a fresh
 * DiamondProgram and frees it when done. DiamondProgram is tens of MB
 * (fixed-size arrays throughout, sized for self-hosting-scale programs
 * -- see docs/roadmap.md), so this heap-allocates it rather than
 * putting it on the stack, mirroring loader.c's own diamond_load_
 * program (which already heap-allocates one for a required package's
 * manifest for the same reason). A caller running *many* programs in
 * one process (tests/run_cases.c, the batch test runner) should call
 * diamond_run_source_with_program directly instead, with one
 * DiamondProgram reused across every call -- diamond_compile always
 * re-initializes it from scratch via diamond_program_init before
 * compiling, so reuse is safe, and it avoids paying this malloc/free's
 * real cost (an allocation this size goes through mmap/munmap, not the
 * ordinary heap, so it's genuine kernel work, not just bookkeeping)
 * hundreds of times over. */
int diamond_run_source(const char *name, const char *source, bool dump_bytecode,
        int script_argc, char *const *script_argv) {
    DiamondProgram *program=malloc(sizeof *program);
    if(program==nullptr) {
        fprintf(stderr,"diamond: out of memory allocating program\n");
        return 74;
    }
    const int status=diamond_run_source_with_program(name,source,dump_bytecode,program,
        script_argc,script_argv);
    free(program);
    return status;
}
