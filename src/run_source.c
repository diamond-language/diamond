#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE

#include "run_source.h"

#include "compiled_prelude.h"
#include "compiled_prelude_data.h"
#include "compiler.h"
#include "disassemble.h"
#include "loader.h"
#include "prelude.h"
#include "value.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static constexpr char DIAMOND_USER_LINE_RESET[] = "\n#line 1\n";

/* DIAMOND_DEBUG_BREAKPOINTS: a comma-separated list of combined-buffer
 * line numbers (see diamond_compile_with_breakpoints's own doc comment,
 * compiler.h), the env-var contract dap/main.c's `launch` handling drives
 * to turn editor breakpoints into compile-time debugger() pauses (see
 * docs/debugging.md). Same convention as every other DIAMOND_*-env-var
 * knob elsewhere in this file. 256 entries is generous for an editor's
 * own gutter breakpoints (compile_sequence's own line_has_breakpoint
 * comment, src/compiler.c, makes the identical sizing argument) -- an
 * env var with more than that is almost certainly malformed, so this
 * silently stops accepting further entries rather than erroring out. */
enum { DIAMOND_MAX_DEBUG_BREAKPOINTS = 256 };

static size_t parse_debug_breakpoints_env(const char *env,
        size_t *out_lines, size_t capacity) {
    if(env==nullptr)return 0;
    size_t count=0;
    const char *cursor=env;
    while(*cursor!='\0'&&count<capacity) {
        while(*cursor==' '||*cursor==',')cursor++;
        if(*cursor=='\0')break;
        char *end=nullptr;
        const unsigned long long parsed=strtoull(cursor,&end,10);
        if(end==cursor)break;
        out_lines[count++]=(size_t)parsed;
        cursor=end;
    }
    return count;
}

/* DIAMOND_TRACE_STARTUP=1 reports where process time actually goes,
 * following the DIAMOND_TRACE_GC pattern below (also seconds via
 * CLOCK_MONOTONIC). "load" is diamond_load_program's require expansion,
 * "compile" is lexing/parsing/codegen over the prelude+require+user
 * source combined -- diamond_compile takes one string and doesn't
 * distinguish prelude from user code internally, so this can't further
 * split prelude-only cost without a second, throwaway compile call. Kept
 * as an instrumentation flag rather than always-on output to match every
 * other DIAMOND_TRACE_* knob's opt-in stderr reporting. */
static double diamond_monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

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

/* Everything shared by diamond_run_source_with_program and
 * diamond_run_source_with_template once compilation has already
 * succeeded: run `chunk`, apply every DIAMOND_*-env-var-driven
 * tracing/stress knob, print the result, and report DIAMOND_TRACE_
 * STARTUP's own summary line -- identical either way, since neither
 * the VM nor any of those knobs know or care how `chunk`'s program got
 * compiled. `owned_buffer` (freed here, may be nullptr) is
 * diamond_run_source_with_program's own concatenated prelude+user
 * text; diamond_run_source_with_template has no equivalent allocation
 * of its own to free, since diamond_compile_incremental compiles
 * `bundle`'s own source directly. `prelude_bytes`/`user_bytes` feed
 * only the trace line's own byte breakdown -- diamond_run_source_
 * with_template passes 0/total, having reprocessed no prelude text at
 * all rather than some nonzero-but-not-actually-recompiled amount. */
static int run_compiled_chunk(const char *name, DiamondChunk chunk, bool dump_bytecode,
        int script_argc, char *const *script_argv,
        bool trace_startup, double start_time, double loaded_time, double compiled_time,
        size_t total_bytes, size_t prelude_bytes, size_t user_bytes,
        char *owned_buffer, DiamondSourceBundle *bundle) {
    chunk.name = name;
    if (dump_bytecode) {
        (void)diamond_disassemble(stdout, name, &chunk);
    }
    DiamondVm vm;
    diamond_vm_init(&vm);
    diamond_vm_set_argv(&vm,script_argc,script_argv);
    vm.stress_gc = getenv("DIAMOND_STRESS_GC") != nullptr;
    vm.stress_minor_gc = getenv("DIAMOND_STRESS_MINOR_GC") != nullptr;
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
        free(owned_buffer);
        diamond_source_bundle_free(bundle);
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
        fprintf(stderr,"GC: %zu major (%.6fs), %zu minor (%.6fs)\n",
                vm.gc_major_collection_count,vm.gc_major_total_seconds,
                vm.gc_minor_collection_count,vm.gc_minor_total_seconds);
    if (trace_startup) {
        const double run_time=diamond_monotonic_seconds();
        fprintf(stderr,
            "startup: load %.6fs, compile %.6fs (%zu bytes: %zu prelude + "
            "%zu user), run %.6fs, total %.6fs\n",
            loaded_time-start_time,compiled_time-loaded_time,
            total_bytes,prelude_bytes,user_bytes,run_time-compiled_time,run_time-start_time);
    }
    diamond_vm_free(&vm);
    free(owned_buffer);
    diamond_source_bundle_free(bundle);
    return 0;
}

/* The compile+run half of diamond_run_source_with_program, split out so
 * diamond_run_source (the CLI's own auto-dispatch entry point) can load
 * `bundle` exactly once and then pick this or
 * run_source_from_bundle_template below, rather than loading it twice
 * (once to decide, once inside whichever *_with_program/_with_template
 * call it made) -- loading involves real file I/O for every required
 * package, so doing it twice would cost real startup time, not just
 * style. */
static int run_source_from_bundle_program(const char *name, DiamondSourceBundle *bundle,
        bool dump_bytecode, DiamondProgram *program,
        int script_argc, char *const *script_argv,
        bool trace_startup, double start_time, double loaded_time,
        const size_t *breakpoint_lines, size_t breakpoint_line_count) {
    const bool include_json=diamond_prelude_needs_json(bundle->source);
    const size_t prelude_length=diamond_prelude_length(include_json);
    const size_t source_length=strlen(bundle->source);
    const size_t reset_length=sizeof(DIAMOND_USER_LINE_RESET)-1;
    if(source_length > SIZE_MAX - prelude_length - reset_length - 1) {
        fprintf(stderr,"diamond: expanded source is too large\n");
        diamond_source_bundle_free(bundle);
        return 74;
    }
    char *combined=malloc(prelude_length+reset_length+source_length+1);
    if(combined==nullptr) {
        fprintf(stderr,"diamond: out of memory building expanded source\n");
        diamond_source_bundle_free(bundle);
        return 74;
    }
    size_t offset=diamond_prelude_write(combined,include_json);
    memcpy(combined+offset,DIAMOND_USER_LINE_RESET,reset_length);offset+=reset_length;
    memcpy(combined+offset,bundle->source,source_length+1);
    DiamondDiagnostic diagnostic;
    diamond_program_free(program);
    if (!diamond_compile_with_breakpoints(combined, program,
            breakpoint_lines, breakpoint_line_count, &diagnostic)) {
        print_diagnostic(name,combined,diagnostic,bundle,prelude_length+reset_length);
        free(combined);
        diamond_source_bundle_free(bundle);
        return 65;
    }
    const double compiled_time=trace_startup ? diamond_monotonic_seconds() : 0;

    return run_compiled_chunk(name, diamond_program_chunk(program), dump_bytecode,
        script_argc, script_argv, trace_startup, start_time, loaded_time, compiled_time,
        prelude_length+reset_length+source_length, prelude_length, source_length,
        combined, bundle);
}

int diamond_run_source_with_program(const char *name, const char *source,
        bool dump_bytecode, DiamondProgram *program,
        int script_argc, char *const *script_argv) {
    const bool trace_startup=getenv("DIAMOND_TRACE_STARTUP") != nullptr;
    const double start_time=trace_startup ? diamond_monotonic_seconds() : 0;
    DiamondSourceBundle bundle;char load_error[768];
    if(!diamond_load_program(name,source,&bundle,load_error,sizeof load_error)) {
        fprintf(stderr,"diamond: %s\n",load_error);return 74;
    }
    const double loaded_time=trace_startup ? diamond_monotonic_seconds() : 0;
    return run_source_from_bundle_program(name,&bundle,dump_bytecode,program,
        script_argc,script_argv,trace_startup,start_time,loaded_time,nullptr,0);
}

/* See run_source_from_bundle_program's own comment -- the same split,
 * for diamond_run_source_with_template. */
static int run_source_from_bundle_template(const char *name, DiamondSourceBundle *bundle,
        bool dump_bytecode, DiamondProgram *program, const DiamondProgram *template,
        int script_argc, char *const *script_argv,
        bool trace_startup, double start_time, double loaded_time) {
    const size_t source_length=strlen(bundle->source);
    DiamondDiagnostic diagnostic;
    diamond_program_free(program);
    if (!diamond_compile_incremental(bundle->source, program, template, &diagnostic)) {
        print_diagnostic(name,bundle->source,diagnostic,bundle,0);
        diamond_source_bundle_free(bundle);
        return 65;
    }
    const double compiled_time=trace_startup ? diamond_monotonic_seconds() : 0;

    return run_compiled_chunk(name, diamond_program_chunk(program), dump_bytecode,
        script_argc, script_argv, trace_startup, start_time, loaded_time, compiled_time,
        source_length, 0, source_length,
        nullptr, bundle);
}

int diamond_run_source_with_template(const char *name, const char *source,
        bool dump_bytecode, DiamondProgram *program, const DiamondProgram *template,
        int script_argc, char *const *script_argv) {
    const bool trace_startup=getenv("DIAMOND_TRACE_STARTUP") != nullptr;
    const double start_time=trace_startup ? diamond_monotonic_seconds() : 0;
    DiamondSourceBundle bundle;char load_error[768];
    if(!diamond_load_program(name,source,&bundle,load_error,sizeof load_error)) {
        fprintf(stderr,"diamond: %s\n",load_error);return 74;
    }
    const double loaded_time=trace_startup ? diamond_monotonic_seconds() : 0;
    return run_source_from_bundle_template(name,&bundle,dump_bytecode,program,template,
        script_argc,script_argv,trace_startup,start_time,loaded_time);
}

/* Builds a fresh, independently-owned DiamondProgram from the build-time
 * #embed'd compiled prelude (src/compiled_prelude_data.c) -- the
 * diamond_program_read_compiled deserialize is what replaces a live
 * lex/parse/codegen of the prelude source on every `diamond` CLI
 * invocation (see CHANGELOG.md's "Performance").
 * diamond_program_init_fresh, not diamond_program_init: `template` is
 * freshly calloc'd right here and never reused, so the ordinary
 * function's own memset would just re-zero memory calloc already
 * zeroed (see that function's own comment, src/compiler.c) --
 * read_compiled immediately overwrites every field that matters anyway.
 * Returns nullptr (never asserts/aborts) if the embedded blob somehow
 * fails to deserialize -- should never happen for a buffer this same
 * build's own `make` just generated, but diamond_run_source below falls
 * back to an ordinary live compile rather than trust that blindly. */
static DiamondProgram *build_embedded_prelude_template(void) {
    DiamondProgram *template=calloc(1,sizeof *template);
    if (template==nullptr) return nullptr;
    diamond_program_init_fresh(template);
    if (!diamond_program_read_compiled(diamond_compiled_prelude_data(),
            diamond_compiled_prelude_size(), template)) {
        diamond_program_free(template);
        free(template);
        return nullptr;
    }
    return template;
}

/* The ordinary entry point (used by the CLI and everything else that
 * only ever runs one program per process): allocates a fresh
 * DiamondProgram and frees it when done. Loads `bundle` exactly once,
 * then picks one of two compile strategies depending on whether the
 * require-expanded source needs JSON:
 *
 *   - the common case (no JSON) builds a template from the build-time
 *     embedded, already-compiled prelude (see
 *     build_embedded_prelude_template above) and compiles the user
 *     source alone via diamond_compile_incremental against it, skipping
 *     the prelude's own lex/parse/codegen entirely;
 *   - a program that needs JSON (diamond_prelude_needs_json), or the one
 *     where the embedded blob somehow fails to deserialize, falls back
 *     to the exact same live diamond_compile-over-prelude+source path
 *     this function always used before the embedded template existed --
 *     zero behavior change for that case, since the embedded template
 *     is always built with JSON included (tools/gen_compiled_prelude.c)
 *     and would otherwise make a name declared only by lib/core/json.di
 *     collide with a same-named top-level class/def in a program that
 *     never even mentions JSON.
 *
 * A caller running *many* programs in one process (tests/run_cases.c,
 * the batch test runner) should call diamond_run_source_with_program or
 * diamond_run_source_with_template directly instead, with one
 * DiamondProgram/template reused across every call -- diamond_compile(_
 * incremental) always re-initializes `program` from scratch before
 * compiling, so reuse is safe, and it avoids paying this function's own
 * per-call template-deserialize cost hundreds of times over. */
int diamond_run_source(const char *name, const char *source, bool dump_bytecode,
        int script_argc, char *const *script_argv) {
    DiamondProgram *program=calloc(1,sizeof *program);
    if(program==nullptr) {
        fprintf(stderr,"diamond: out of memory allocating program\n");
        return 74;
    }
    const bool trace_startup=getenv("DIAMOND_TRACE_STARTUP") != nullptr;
    const double start_time=trace_startup ? diamond_monotonic_seconds() : 0;
    DiamondSourceBundle bundle;char load_error[768];
    if(!diamond_load_program(name,source,&bundle,load_error,sizeof load_error)) {
        fprintf(stderr,"diamond: %s\n",load_error);
        free(program);
        return 74;
    }
    const double loaded_time=trace_startup ? diamond_monotonic_seconds() : 0;

    size_t breakpoint_lines[DIAMOND_MAX_DEBUG_BREAKPOINTS];
    const size_t breakpoint_line_count=parse_debug_breakpoints_env(
        getenv("DIAMOND_DEBUG_BREAKPOINTS"),breakpoint_lines,
        DIAMOND_MAX_DEBUG_BREAKPOINTS);

    /* A debug session (breakpoint_line_count>0) always takes the
     * ordinary live prelude+source compile below, never the embedded-
     * template fast path: diamond_compile_with_breakpoints only has a
     * no-template overload (see its own doc comment, compiler.h), and a
     * debug launch isn't the startup-time-sensitive case this
     * optimization exists for -- see build_embedded_prelude_template's
     * own comment. dap/main.c's own breakpoint-position resolution
     * (setBreakpoints) assumes this exact combined-buffer layout
     * (prelude+reset+bundle source, the same one run_source_from_bundle_
     * program builds below) whenever it's about to launch with
     * DIAMOND_DEBUG_BREAKPOINTS set, so the two sides must keep agreeing
     * on which path runs -- forcing it here, unconditionally, is what
     * keeps that true regardless of diamond_prelude_needs_json. */
    DiamondProgram *template=nullptr;
    if (breakpoint_line_count==0&&!diamond_prelude_needs_json(bundle.source))
        template=build_embedded_prelude_template();

    int status;
    if (template != nullptr) {
        status=run_source_from_bundle_template(name,&bundle,dump_bytecode,program,template,
            script_argc,script_argv,trace_startup,start_time,loaded_time);
        diamond_program_free(template);
        free(template);
    } else {
        status=run_source_from_bundle_program(name,&bundle,dump_bytecode,program,
            script_argc,script_argv,trace_startup,start_time,loaded_time,
            breakpoint_lines,breakpoint_line_count);
    }
    diamond_program_free(program);
    free(program);
    return status;
}
