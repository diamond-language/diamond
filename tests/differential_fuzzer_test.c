/* Include the harness to exercise its real environment and capture path. */
#include "../fuzz/differential_fuzzer.c"

int main(void) {
    char *args[] = {"differential_fuzzer", nullptr};
    char **argv = args;
    int argc = 1;
    LLVMFuzzerInitialize(&argc, &argv);
    if (argc != 2 || strcmp(argv[1], "-timeout=10") != 0 || argv[2] != nullptr)
        return 1;
    char *custom[] = {"differential_fuzzer", "-timeout=20", nullptr};
    argv = custom; argc = 2;
    LLVMFuzzerInitialize(&argc, &argv);
    if (argc != 2 || argv != custom) return 1;

    DiamondProgram *template = calloc(1, sizeof *template);
    DiamondProgram *program = calloc(1, sizeof *program);
    const size_t length = diamond_prelude_length(true);
    char *source = malloc(length + 1);
    if (template == nullptr || program == nullptr || source == nullptr) return 1;
    diamond_prelude_write(source, true);
    source[length] = '\0';
    DiamondDiagnostic diagnostic;
    if (!diamond_compile(source, template, &diagnostic)) return 1;
    free(source);

    setenv("DIAMOND_SANDBOX", "1", 1);
    setenv("DIAMOND_NO_CACHE", "1", 1);
    /* An inherited memory budget must not silently disable the native run. */
    setenv("DIAMOND_MAX_MEMORY_BYTES", "100000000", 1);
    const char *seed = "def plus_one(x: Int) -> Int = x + 1\nputs(plus_one(41))\n";
    Capture plain = {}, stressed = {};
    set_plain();
    if (!run_captured(seed, program, template, &plain)) return 1;
    for (size_t i = 0; i < program->function_count; i++)
        if (program->functions[i]->jit_code != nullptr) return 1;
    set_stressed();
    if (!run_captured(seed, program, template, &stressed)) return 1;
    bool compiled = false;
    for (size_t i = 0; i < program->function_count; i++)
        compiled |= program->functions[i]->jit_code != nullptr;
#if defined(__x86_64__) && defined(__GLIBC__)
    if (!compiled) {
        fputs("differential fuzzer did not execute native code\n", stderr);
        return 1;
    }
#else
    (void)compiled;
#endif
    if (plain.status != 0 || stressed.status != 0 ||
        strcmp(plain.stdout_text, stressed.stdout_text) != 0 ||
        strcmp(plain.stderr_text, stressed.stderr_text) != 0) return 1;
    free(plain.stdout_text); free(plain.stderr_text);
    free(stressed.stdout_text); free(stressed.stderr_text);
    diamond_program_free(program); free(program);
    diamond_program_free(template); free(template);
    /* An endless interpreter preflight must be skipped before the unbudgeted
     * run. Terminate this test if that guard regresses; never resume the VM. */
    const char *endless = "loop\nend\n";
    alarm(15);
    LLVMFuzzerTestOneInput((const uint8_t *)endless, strlen(endless));
    alarm(0);
    puts("differential fuzzer executes native code on supported platforms");
    return 0;
}
