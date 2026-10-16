/* Mutates a compiled-program cache, re-signs it, and runs the program against it.
 *
 *   forge_cache WORKDIR DIAMOND PROGRAM.di ROUNDS
 *
 * WORKDIR holds PROGRAM.di and the PROGRAM.dic its first run produced. The cache
 * checksum only catches accidents, so each round corrupts a few bytes and then
 * recomputes the checksum, which leaves the structural and bytecode validation as the
 * only thing standing between the mutant and the VM. A mutant may be rejected (and
 * recompiled), run to a normal exit, or run on forever; none may kill the process with
 * a signal. Prints the number of mutants that did and exits 1 if any did.
 *
 * The checksum below is the cache format's body checksum (src/compiled_prelude.c);
 * the header is magic, then the 8-byte checksum at offset 80 over everything from 88. */
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum { CHECKSUM_AT = 80, BODY_AT = 88, MUTATE_FROM = 100, MUTATE_TO = 20000, TIMEOUT_SECONDS = 5 };

static uint64_t body_checksum(const uint8_t *data, size_t size) {
    uint64_t hash = 0x9e3779b97f4a7c15ull ^ (uint64_t)size;
    size_t index = 0;
    for (; index + 8 <= size; index += 8) {
        uint64_t word;
        memcpy(&word, data + index, sizeof word);
        hash = (hash ^ word) * 0xff51afd7ed558ccdull;
        hash ^= hash >> 32;
    }
    uint64_t tail = 0;
    memcpy(&tail, data + index, size - index);
    hash = (hash ^ tail) * 0xff51afd7ed558ccdull;
    hash ^= hash >> 29;
    return hash;
}

static uint64_t rng_state = 20261004;
static uint64_t next_random(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* Returns true when the child died from a signal other than our own timeout kill. */
static bool run_once(const char *workdir, const char *diamond, const char *program) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        if (chdir(workdir) != 0) _exit(126);
        freopen("/dev/null", "w", stdout);
        freopen("/dev/null", "w", stderr);
        execl(diamond, diamond, program, (char *)nullptr);
        _exit(127);
    }
    time_t deadline = time(nullptr) + TIMEOUT_SECONDS;
    int status = 0;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) return WIFSIGNALED(status);
        if (time(nullptr) >= deadline) {
            /* A mutant may legitimately loop forever; only an unprovoked signal is a failure. */
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return false;
        }
        struct timespec pause = {0, 20 * 1000 * 1000};
        nanosleep(&pause, nullptr);
    }
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s WORKDIR DIAMOND PROGRAM.di ROUNDS\n", argv[0]);
        return 64;
    }
    const char *workdir = argv[1], *diamond = argv[2], *program = argv[3];
    int rounds = atoi(argv[4]);

    char cache_path[4096];
    snprintf(cache_path, sizeof cache_path, "%s/%s", workdir, program);
    size_t path_length = strlen(cache_path);
    if (path_length < 3 || strcmp(cache_path + path_length - 3, ".di") != 0) {
        fprintf(stderr, "%s: program must end in .di\n", argv[0]);
        return 64;
    }
    strcpy(cache_path + path_length - 3, ".dic");

    FILE *file = fopen(cache_path, "rb");
    if (!file) {
        perror(cache_path);
        return 2;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size <= MUTATE_FROM + 1 || size <= BODY_AT) {
        fprintf(stderr, "%s: cache is too small to mutate\n", cache_path);
        return 2;
    }
    uint8_t *base = malloc((size_t)size), *mutant = malloc((size_t)size);
    if (!base || !mutant || fread(base, 1, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "%s: cannot read cache\n", cache_path);
        return 2;
    }
    fclose(file);

    long limit = size < MUTATE_TO ? size : MUTATE_TO;
    static const int edits[] = {1, 1, 2, 4};
    int crashes = 0;
    for (int round = 0; round < rounds; round++) {
        memcpy(mutant, base, (size_t)size);
        int count = edits[next_random() % 4];
        for (int edit = 0; edit < count; edit++) {
            size_t at = MUTATE_FROM + next_random() % (uint64_t)(limit - MUTATE_FROM);
            if (next_random() % 2) mutant[at] = (uint8_t)next_random();
            else mutant[at] ^= (uint8_t)(1u << (next_random() % 8));
        }
        uint64_t sum = body_checksum(mutant + BODY_AT, (size_t)size - BODY_AT);
        memcpy(mutant + CHECKSUM_AT, &sum, sizeof sum);
        file = fopen(cache_path, "wb");
        if (!file || fwrite(mutant, 1, (size_t)size, file) != (size_t)size) {
            perror(cache_path);
            return 2;
        }
        fclose(file);
        if (run_once(workdir, diamond, program)) crashes++;
    }
    printf("%d\n", crashes);
    return crashes == 0 ? 0 : 1;
}
