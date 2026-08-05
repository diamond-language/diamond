#include "compiler.h"
#include "disassemble.h"
#include "value.h"
#include "vm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static constexpr char DIAMOND_VERSION[] = "0.1.0-dev";

static void print_diagnostic(const char *name, const char *source,
                             DiamondDiagnostic diagnostic) {
    fprintf(stderr, "%s:%zu:%zu: error: %s\n", name, diagnostic.span.line,
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
    DiamondProgram program;
    DiamondDiagnostic diagnostic;
    if (!diamond_compile(source, &program, &diagnostic)) {
        print_diagnostic(name, source, diagnostic);
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
        return 70;
    }

    diamond_value_print(result);
    putchar('\n');
    if (getenv("DIAMOND_TRACE_IC") != nullptr) {
        fprintf(stderr,"inline caches: %zu hits, %zu misses\n",
                vm.inline_cache_hits,vm.inline_cache_misses);
    }
    diamond_vm_free(&vm);
    return 0;
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr) {
        fprintf(stderr, "diamond: cannot open '%s': %s\n", path, strerror(errno));
        return nullptr;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fprintf(stderr, "diamond: cannot seek '%s'\n", path);
        fclose(file);
        return nullptr;
    }
    const long length = ftell(file);
    if (length < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "diamond: cannot read '%s'\n", path);
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
        fprintf(stderr, "diamond: cannot read '%s'\n", path);
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
