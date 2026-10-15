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

#if defined(__wasm__)
/* No OpenSSL on WebAssembly: a small portable SHA-256 for the source hash. */
#else
#include <openssl/evp.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static constexpr char DIAMOND_USER_LINE_RESET[] = "\n#line 1\n";

/* Sibling-file bytecode cache (see docs/caching.md): SHA-256 of the fully
 * require-expanded bundle.source, and a ".dic" path derived from `name`.
 * `-e` (name is always the literal string "-e" regardless of what code
 * was actually passed, per main.c's own dispatch) and any path with no
 * stable on-disk identity get no cache path at all -- diamond_run_source
 * checks for exactly this nullptr before ever trying to read or write
 * one. */
#if defined(__wasm__)
static uint32_t sha256_rotate_right(uint32_t value, unsigned count) {
    return (value >> count) | (value << (32u - count));
}

static void sha256_block(uint32_t state[8], const uint8_t block[64]) {
    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t w[64];
    for (size_t i = 0; i < 16; i++)
        w[i] = ((uint32_t)block[i * 4] << 24) | ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) | (uint32_t)block[i * 4 + 3];
    for (size_t i = 16; i < 64; i++) {
        const uint32_t s0 = sha256_rotate_right(w[i - 15], 7) ^ sha256_rotate_right(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = sha256_rotate_right(w[i - 2], 17) ^ sha256_rotate_right(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (size_t i = 0; i < 64; i++) {
        const uint32_t s1 = sha256_rotate_right(e, 6) ^ sha256_rotate_right(e, 11) ^ sha256_rotate_right(e, 25);
        const uint32_t choose = (e & f) ^ (~e & g);
        const uint32_t t1 = h + s1 + choose + k[i] + w[i];
        const uint32_t s0 = sha256_rotate_right(a, 2) ^ sha256_rotate_right(a, 13) ^ sha256_rotate_right(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = s0 + majority;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

static void diamond_hash_source(const char *source, uint8_t out[32]) {
    uint32_t state[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                         0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    const size_t length = strlen(source);
    size_t offset = 0;
    for (; offset + 64 <= length; offset += 64)
        sha256_block(state, (const uint8_t *)source + offset);
    uint8_t tail[128] = {0};
    const size_t remaining = length - offset;
    memcpy(tail, source + offset, remaining);
    tail[remaining] = 0x80;
    const size_t tail_length = remaining + 1 + 8 <= 64 ? 64 : 128;
    const uint64_t bits = (uint64_t)length * 8u;
    for (size_t i = 0; i < 8; i++)
        tail[tail_length - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t block = 0; block < tail_length; block += 64)
        sha256_block(state, tail + block);
    for (size_t i = 0; i < 8; i++) {
        out[i * 4] = (uint8_t)(state[i] >> 24); out[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(state[i] >> 8); out[i * 4 + 3] = (uint8_t)state[i];
    }
}
#else
static void diamond_hash_source(const char *source, uint8_t out[32]) {
    unsigned int digest_length = 0;
    EVP_Digest(source, strlen(source), out, &digest_length, EVP_sha256(), nullptr);
}
#endif

/* `app.di` -> `app.dic` (matches Python's own classic foo.py -> foo.pyc
 * sibling-cache convention -- append, don't replace, so the source's own
 * extension stays legible in the cache file's name); anything not ending
 * in ".di" just gets ".dic" appended wholesale. Returns nullptr for `-e`
 * (no real file to sit a cache file next to) or on allocation failure --
 * both are treated identically by every caller: no cache path means no
 * caching for this run, silently. */
char *diamond_cache_path_for(const char *name) {
    if (strcmp(name, "-e") == 0) return nullptr;
    const size_t name_length = strlen(name);
    const bool has_di_extension = name_length >= 3 &&
        strcmp(name + name_length - 3, ".di") == 0;
    char *path = malloc(name_length + 5);
    if (path == nullptr) return nullptr;
    memcpy(path, name, name_length);
    if (has_di_extension) {
        path[name_length] = 'c';
        path[name_length + 1] = '\0';
    } else {
        memcpy(path + name_length, ".dic", 5);
    }
    return path;
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
 * the trace line's byte breakdown. A nonzero prelude size also locates
 * the user-source boundary for targeted dumps; template compiles pass
 * zero there and identify inherited functions by prelude_function_count. */
static int run_compiled_chunk(const char *name, DiamondChunk chunk, DiamondBytecodeDump dump_bytecode,
        int script_argc, char *const *script_argv,
        bool trace_startup, double start_time, double loaded_time, double compiled_time,
        size_t total_bytes, size_t prelude_bytes, size_t user_bytes,
        char *owned_buffer, DiamondSourceBundle *bundle, size_t prelude_function_count) {
    chunk.name = name;
    if (dump_bytecode == DIAMOND_DUMP_USER) {
        const size_t source_start=prelude_bytes==0?0:
            prelude_bytes+sizeof(DIAMOND_USER_LINE_RESET)-1;
        (void)diamond_disassemble_user(stdout,name,&chunk,prelude_function_count,source_start);
    } else if (dump_bytecode) {
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
    diamond_vm_configure_jit_from_env(&vm);
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
        diamond_vm_exit_if_threads_running(70);
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
    if (getenv("DIAMOND_TRACE_JIT") != nullptr) {
        fprintf(stderr,"jit: %zu compiled function(s), %zu bailout(s), %zu hard propagation(s)\n",
                vm.jit_compiled_functions,vm.jit_bailouts,vm.jit_hard_propagations);
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
    if (getenv("DIAMOND_TRACE_TAIL_CALLS") != nullptr)
        fprintf(stderr,"tail calls: %zu optimized in place\n",
                vm.opcode_counts[DIAMOND_OP_TAIL_CALL]);
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
        DiamondBytecodeDump dump_bytecode, DiamondProgram *program,
        int script_argc, char *const *script_argv,
        bool trace_startup, double start_time, double loaded_time,
        const char *cache_path, const uint8_t *source_hash,
        const DiamondProgram *cache_template) {
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
    if (!diamond_compile_with_breakpoints(combined, program, &diagnostic)) {
        print_diagnostic(name,combined,diagnostic,bundle,prelude_length+reset_length);
        free(combined);
        diamond_source_bundle_free(bundle);
        return 65;
    }
    const double compiled_time=trace_startup ? diamond_monotonic_seconds() : 0;
    if (cache_path != nullptr) {
        /* Compiled live, but its prelude prefix is the embedded template's
         * own text, so the cache can leave those functions out too (it
         * falls back to the whole program when they differ). */
        diamond_program_write_cache_file(cache_path, source_hash, program, cache_template);
        if (getenv("DIAMOND_TRACE_CACHE") != nullptr)
            fprintf(stderr, "cache: wrote %s\n", cache_path);
    }

    return run_compiled_chunk(name, diamond_program_chunk(program), dump_bytecode,
        script_argc, script_argv, trace_startup, start_time, loaded_time, compiled_time,
        prelude_length+reset_length+source_length, prelude_length, source_length,
        combined, bundle, 0);
}

int diamond_run_source_with_program(const char *name, const char *source,
        DiamondBytecodeDump dump_bytecode, DiamondProgram *program,
        int script_argc, char *const *script_argv) {
    const bool trace_startup=getenv("DIAMOND_TRACE_STARTUP") != nullptr;
    const double start_time=trace_startup ? diamond_monotonic_seconds() : 0;
    DiamondSourceBundle bundle;char load_error[768];
    if(!diamond_load_program(name,source,&bundle,load_error,sizeof load_error)) {
        fprintf(stderr,"diamond: %s\n",load_error);return 74;
    }
    const double loaded_time=trace_startup ? diamond_monotonic_seconds() : 0;
    return run_source_from_bundle_program(name,&bundle,dump_bytecode,program,
        script_argc,script_argv,trace_startup,start_time,loaded_time,
        nullptr,nullptr,nullptr);
}

/* Returned by run_source_from_bundle_template (never a real exit status) when
 * its compile failed and it was asked to leave the retry to the caller. */
enum { DIAMOND_TEMPLATE_COMPILE_FAILED = -1 };

/* See run_source_from_bundle_program's own comment -- the same split,
 * for diamond_run_source_with_template. */
static int run_source_from_bundle_template(const char *name, DiamondSourceBundle *bundle,
        DiamondBytecodeDump dump_bytecode, DiamondProgram *program, const DiamondProgram *template,
        int script_argc, char *const *script_argv,
        bool trace_startup, double start_time, double loaded_time,
        const char *cache_path, const uint8_t *source_hash, bool fall_back_on_failure) {
    const size_t source_length=strlen(bundle->source);
    DiamondDiagnostic diagnostic;
    diamond_program_free(program);
    if (!diamond_compile_incremental(bundle->source, program, template, &diagnostic)) {
        /* The caller will retry with a live prelude compile and report that
         * attempt's diagnostic, leaving `bundle` untouched. */
        if (fall_back_on_failure) return DIAMOND_TEMPLATE_COMPILE_FAILED;
        print_diagnostic(name,bundle->source,diagnostic,bundle,0);
        diamond_source_bundle_free(bundle);
        return 65;
    }
    const double compiled_time=trace_startup ? diamond_monotonic_seconds() : 0;
    if (cache_path != nullptr) {
        diamond_program_write_cache_file(cache_path, source_hash, program, template);
        if (getenv("DIAMOND_TRACE_CACHE") != nullptr)
            fprintf(stderr, "cache: wrote %s\n", cache_path);
    }

    return run_compiled_chunk(name, diamond_program_chunk(program), dump_bytecode,
        script_argc, script_argv, trace_startup, start_time, loaded_time, compiled_time,
        source_length, 0, source_length,
        nullptr, bundle, template->function_count);
}

int diamond_run_source_with_template(const char *name, const char *source,
        DiamondBytecodeDump dump_bytecode, DiamondProgram *program, const DiamondProgram *template,
        int script_argc, char *const *script_argv) {
    const bool trace_startup=getenv("DIAMOND_TRACE_STARTUP") != nullptr;
    const double start_time=trace_startup ? diamond_monotonic_seconds() : 0;
    DiamondSourceBundle bundle;char load_error[768];
    if(!diamond_load_program(name,source,&bundle,load_error,sizeof load_error)) {
        fprintf(stderr,"diamond: %s\n",load_error);return 74;
    }
    const double loaded_time=trace_startup ? diamond_monotonic_seconds() : 0;
    return run_source_from_bundle_template(name,&bundle,dump_bytecode,program,template,
        script_argc,script_argv,trace_startup,start_time,loaded_time,nullptr,nullptr,false);
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
int diamond_run_source(const char *name, const char *source, DiamondBytecodeDump dump_bytecode,
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

    /* A debug session always takes the ordinary live prelude+source
     * compile below, never the embedded-template fast path:
     * diamond_compile_with_breakpoints only has a no-template overload
     * (see its own doc comment, compiler.h), and a debug launch isn't
     * the startup-time-sensitive case this optimization exists for --
     * see build_embedded_prelude_template's own comment. dap/main.c's
     * own breakpoint-position resolution (setBreakpoints) assumes this
     * exact combined-buffer layout (prelude+reset+bundle source, the
     * same one run_source_from_bundle_program builds below) whenever it
     * launches a debuggee, so the two sides must keep agreeing on which
     * path runs -- forcing it here, unconditionally, is what keeps that
     * true regardless of diamond_prelude_needs_json.
     *
     * Triggered by DIAMOND_DEBUG_BREAKPOINTS *or* DIAMOND_DEBUG_FD being
     * present, not just the former: a DAP session that starts with zero
     * breakpoints selected (attach and run first, decide where to break
     * later -- a real, common workflow) still needs every statement
     * instrumented with DIAMOND_OP_BREAKPOINT_CHECK, or there would be
     * nothing for a later live `setBreakpoints` command to arm at all --
     * see DiamondVm.debug_active_lines's own comment (src/vm.h) and
     * docs/debugging.md's "live breakpoints" section. dap/main.c already
     * sets DIAMOND_DEBUG_FD unconditionally for every spawned child, so
     * this reaches every real DAP session regardless of its initial
     * breakpoint set; DIAMOND_DEBUG_BREAKPOINTS alone (no DAP client at
     * all) remains supported too, for direct manual testing. */
    const bool debug_mode = getenv("DIAMOND_DEBUG_BREAKPOINTS")!=nullptr ||
        getenv("DIAMOND_DEBUG_BREAKPOINT_OFFSETS")!=nullptr ||
        getenv("DIAMOND_DEBUG_FD")!=nullptr;

    /* Bytecode cache (docs/caching.md): only for this auto-dispatch entry
     * point, never diamond_run_source_with_program/_with_template (those
     * two are for a caller -- tests/run_cases.c -- running many programs
     * against one already-loaded template in one process, where a
     * per-run disk cache buys nothing and risks masking the very
     * regressions that harness exists to catch). Skipped entirely for
     * `-e` (diamond_cache_path_for's own nullptr return), a debug
     * session (must not interfere with dap/main.c's own combined-buffer
     * assumptions, same reason the embedded-template fast path above is
     * already skipped for one), and whenever DIAMOND_NO_CACHE is set. */
    /* A targeted dump needs the compile path's prelude boundary. Recompile
     * instead of guessing whether a cached program came from a template. */
    char *cache_path = (dump_bytecode != DIAMOND_DUMP_USER &&
        !debug_mode && getenv("DIAMOND_NO_CACHE")==nullptr) ?
        diamond_cache_path_for(name) : nullptr;
    uint8_t source_hash[32];
    bool used_cache = false;
    /* The embedded prelude, built once: a cache file leaves the prelude's own
     * functions out and rebuilds them from this, and a cache miss compiles
     * against it. */
    DiamondProgram *template=nullptr;
    if (!debug_mode)
        template=build_embedded_prelude_template();
    /* A program that mentions JSON is compiled against the same embedded
     * prelude (which already includes JSON) and only falls back to a live
     * prelude+source compile if that fails -- the rare program that
     * declares a name of its own that collides with JSON's. */
    const bool mentions_json=diamond_prelude_needs_json(bundle.source);
    if (cache_path != nullptr) {
        diamond_hash_source(bundle.source, source_hash);
        used_cache = diamond_program_read_cache_file(cache_path, source_hash, program, template);
        if (getenv("DIAMOND_TRACE_CACHE") != nullptr)
            fprintf(stderr, "cache: %s %s\n", used_cache ? "hit" : "miss", cache_path);
    }

    int status;
    if (used_cache) {
        const double compiled_time=trace_startup ? diamond_monotonic_seconds() : 0;
        const size_t user_bytes=strlen(bundle.source);
        status=run_compiled_chunk(name, diamond_program_chunk(program), dump_bytecode,
            script_argc, script_argv, trace_startup, start_time, loaded_time, compiled_time,
            user_bytes, 0, user_bytes, nullptr, &bundle, 0);
    } else {
        status=DIAMOND_TEMPLATE_COMPILE_FAILED;
        if (template != nullptr)
            status=run_source_from_bundle_template(name,&bundle,dump_bytecode,program,template,
                script_argc,script_argv,trace_startup,start_time,loaded_time,
                cache_path,source_hash,mentions_json);
        if (status==DIAMOND_TEMPLATE_COMPILE_FAILED)
            status=run_source_from_bundle_program(name,&bundle,dump_bytecode,program,
                script_argc,script_argv,trace_startup,start_time,loaded_time,
                cache_path,source_hash,template);
    }
    if (template != nullptr) {
        diamond_program_free(template);
        free(template);
    }
    free(cache_path);
    diamond_program_free(program);
    free(program);
    return status;
}

/* Compiles `source` the same way diamond_run_source's own auto-dispatch
 * does (the embedded-prelude-template fast path when the require-
 * expanded source doesn't need JSON, the ordinary live prelude+source
 * compile otherwise) -- for a caller (diamond build, src/main.c) that
 * wants the finished DiamondProgram itself, not a running result. A
 * deliberately separate function rather than a refactor of
 * diamond_run_source into a shared "compile" half: that function's own
 * run_compiled_chunk call needs `combined`/`bundle`/the prelude-vs-user
 * byte breakdown for DIAMOND_TRACE_STARTUP's own reporting, none of
 * which a caller that never runs anything needs -- reusing the same
 * underlying helpers (diamond_load_program, build_embedded_prelude_
 * template, diamond_prelude_needs_json/length/write, diamond_compile(_
 * incremental)) keeps the two paths behaviorally identical without
 * risking diamond_run_source's own already-proven shape. Prints its own
 * diagnostic on a load/compile failure (print_diagnostic, the same
 * function run_source_from_bundle_program/_template already call
 * internally) and returns false -- the caller doesn't need its own
 * diagnostic-formatting path at all. No debug-breakpoint support (see
 * diamond_compile_with_breakpoints): a standalone AOT binary has no
 * DAP client to pause for. */
/* Compiles `bundle` the way diamond_run_source does: against the embedded
 * prelude `template` when there is one, and only a program that mentions JSON
 * and fails that falls back to a live prelude+source compile (the rare one
 * declaring names that collide with JSON's). Prints the diagnostic of the
 * attempt that decided the outcome; the caller still owns `bundle`. */
static bool compile_bundle_for_run(const char *name, const DiamondSourceBundle *bundle,
        DiamondProgram *program, const DiamondProgram *template) {
    const bool mentions_json=diamond_prelude_needs_json(bundle->source);
    DiamondDiagnostic diagnostic;
    if(template!=nullptr) {
        diamond_program_free(program);
        if(diamond_compile_incremental(bundle->source,program,template,&diagnostic))
            return true;
        if(!mentions_json) {
            print_diagnostic(name,bundle->source,diagnostic,bundle,0);
            return false;
        }
    }
    const size_t prelude_length=diamond_prelude_length(mentions_json);
    const size_t source_length=strlen(bundle->source);
    const size_t reset_length=sizeof(DIAMOND_USER_LINE_RESET)-1;
    char *combined=malloc(prelude_length+reset_length+source_length+1);
    if(combined==nullptr) {
        fprintf(stderr,"diamond: out of memory building expanded source\n");
        return false;
    }
    size_t offset=diamond_prelude_write(combined,mentions_json);
    memcpy(combined+offset,DIAMOND_USER_LINE_RESET,reset_length);offset+=reset_length;
    memcpy(combined+offset,bundle->source,source_length+1);
    diamond_program_free(program);
    const bool ok=diamond_compile(combined,program,&diagnostic);
    if(!ok)print_diagnostic(name,combined,diagnostic,bundle,prelude_length+reset_length);
    free(combined);
    return ok;
}

bool diamond_compile_source(const char *name, const char *source, DiamondProgram *program) {
    DiamondSourceBundle bundle;char load_error[768];
    if(!diamond_load_program(name,source,&bundle,load_error,sizeof load_error)) {
        fprintf(stderr,"diamond: %s\n",load_error);
        return false;
    }
    DiamondProgram *template=build_embedded_prelude_template();
    const bool ok=compile_bundle_for_run(name,&bundle,program,template);
    if(template!=nullptr) {
        diamond_program_free(template);
        free(template);
    }
    diamond_source_bundle_free(&bundle);
    return ok;
}

int diamond_warm_cache(const char *name, const char *source) {
    char *cache_path=diamond_cache_path_for(name);
    if(cache_path==nullptr) {
        fprintf(stderr,"diamond: cannot cache '%s'\n",name);
        return 64;
    }
    DiamondSourceBundle bundle;char load_error[768];
    if(!diamond_load_program(name,source,&bundle,load_error,sizeof load_error)) {
        fprintf(stderr,"diamond: %s\n",load_error);
        free(cache_path);
        return 74;
    }
    DiamondProgram *program=calloc(1,sizeof *program);
    DiamondProgram *template=build_embedded_prelude_template();
    if(program==nullptr||template==nullptr) {
        fprintf(stderr,"diamond: out of memory\n");
        free(program);
        if(template!=nullptr) {diamond_program_free(template);free(template);}
        diamond_source_bundle_free(&bundle);
        free(cache_path);
        return 74;
    }
    uint8_t source_hash[32];
    diamond_hash_source(bundle.source,source_hash);
    int status=0;
    if(diamond_program_read_cache_file(cache_path,source_hash,program,template)) {
        printf("up to date %s\n",cache_path);
    } else if(!compile_bundle_for_run(name,&bundle,program,template)) {
        status=65;
    } else {
        diamond_program_write_cache_file(cache_path,source_hash,program,template);
        /* The write is best effort; confirm it landed instead of reporting
         * success for a read-only directory. */
        DiamondProgram *check=calloc(1,sizeof *check);
        const bool written=check!=nullptr&&
            diamond_program_read_cache_file(cache_path,source_hash,check,template);
        if(check!=nullptr) {diamond_program_free(check);free(check);}
        if(written) {
            printf("cached %s\n",cache_path);
        } else {
            fprintf(stderr,"diamond: cannot write %s\n",cache_path);
            status=73;
        }
    }
    diamond_program_free(program);
    free(program);
    diamond_program_free(template);
    free(template);
    diamond_source_bundle_free(&bundle);
    free(cache_path);
    return status;
}
