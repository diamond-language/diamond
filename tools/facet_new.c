/* `facet new <kind> <path>`: creates a runnable, tested project skeleton.
 * The templates live in tools/new_templates/ and are #embed'ed here, so
 * an installed facet needs no source checkout. Every project's tests use
 * the minitest cut, declared in diamond.dev.cut so that it never becomes
 * a runtime dependency of a published library. */
#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "facet_internal.h"

#include <time.h>

static constexpr unsigned char DEV_CUT[] = {
#embed "new_templates/common/diamond.dev.cut" suffix(,)
    0
};
static constexpr unsigned char TEST_SH[] = {
#embed "new_templates/common/test.sh" suffix(,)
    0
};
static constexpr unsigned char APP_GITIGNORE[] = {
#embed "new_templates/common/gitignore_app" suffix(,)
    0
};
static constexpr unsigned char CLI_MAIN[] = {
#embed "new_templates/cli/main.d6" suffix(,)
    0
};
static constexpr unsigned char CLI_LIB[] = {
#embed "new_templates/cli/lib.d6" suffix(,)
    0
};
static constexpr unsigned char CLI_TEST[] = {
#embed "new_templates/cli/test.d6" suffix(,)
    0
};
static constexpr unsigned char CLI_CUT[] = {
#embed "new_templates/cli/diamond.cut" suffix(,)
    0
};
static constexpr unsigned char CLI_README[] = {
#embed "new_templates/cli/README.md" suffix(,)
    0
};
static constexpr unsigned char WEB_MAIN[] = {
#embed "new_templates/web/main.d6" suffix(,)
    0
};
static constexpr unsigned char WEB_LIB[] = {
#embed "new_templates/web/lib.d6" suffix(,)
    0
};
static constexpr unsigned char WEB_TEST[] = {
#embed "new_templates/web/test.d6" suffix(,)
    0
};
static constexpr unsigned char WEB_CUT[] = {
#embed "new_templates/web/diamond.cut" suffix(,)
    0
};
static constexpr unsigned char WEB_README[] = {
#embed "new_templates/web/README.md" suffix(,)
    0
};
static constexpr unsigned char TCP_MAIN[] = {
#embed "new_templates/tcp/main.d6" suffix(,)
    0
};
static constexpr unsigned char TCP_LIB[] = {
#embed "new_templates/tcp/lib.d6" suffix(,)
    0
};
static constexpr unsigned char TCP_TEST[] = {
#embed "new_templates/tcp/test.d6" suffix(,)
    0
};
static constexpr unsigned char TCP_CUT[] = {
#embed "new_templates/tcp/diamond.cut" suffix(,)
    0
};
static constexpr unsigned char TCP_README[] = {
#embed "new_templates/tcp/README.md" suffix(,)
    0
};
static constexpr unsigned char UDP_MAIN[] = {
#embed "new_templates/udp/main.d6" suffix(,)
    0
};
static constexpr unsigned char UDP_LIB[] = {
#embed "new_templates/udp/lib.d6" suffix(,)
    0
};
static constexpr unsigned char UDP_TEST[] = {
#embed "new_templates/udp/test.d6" suffix(,)
    0
};
static constexpr unsigned char UDP_CUT[] = {
#embed "new_templates/udp/diamond.cut" suffix(,)
    0
};
static constexpr unsigned char UDP_README[] = {
#embed "new_templates/udp/README.md" suffix(,)
    0
};
static constexpr unsigned char LIB_LIB[] = {
#embed "new_templates/lib/lib.d6" suffix(,)
    0
};
static constexpr unsigned char LIB_TEST[] = {
#embed "new_templates/lib/test.d6" suffix(,)
    0
};
static constexpr unsigned char LIB_CUT[] = {
#embed "new_templates/lib/diamond.cut" suffix(,)
    0
};
static constexpr unsigned char LIB_README[] = {
#embed "new_templates/lib/README.md" suffix(,)
    0
};
static constexpr unsigned char LIB_LICENSE[] = {
#embed "new_templates/lib/LICENSE" suffix(,)
    0
};
static constexpr unsigned char LIB_GITIGNORE[] = {
#embed "new_templates/lib/gitignore" suffix(,)
    0
};

/* One generated file. `path` may contain {{name}}; `raw` files are
 * copied without placeholder substitution. */
typedef struct NewFile {
    const char *path;
    const unsigned char *text;
    bool raw;
    bool executable;
} NewFile;

#define COMMON_FILES                                                  \
    {"test.sh", TEST_SH, true, true},                                 \
    {"diamond.dev.cut", DEV_CUT, true, false}

static const NewFile CLI_FILES[] = {
    {"diamond.cut", CLI_CUT, false, false}, {"README.md", CLI_README, false, false},
    {".gitignore", APP_GITIGNORE, false, false}, {"main.d6", CLI_MAIN, false, false},
    {"lib/{{name}}.d6", CLI_LIB, false, false},
    {"test/{{name}}_test.d6", CLI_TEST, false, false}, COMMON_FILES,
};
static const NewFile WEB_FILES[] = {
    {"diamond.cut", WEB_CUT, false, false}, {"README.md", WEB_README, false, false},
    {".gitignore", APP_GITIGNORE, false, false}, {"main.d6", WEB_MAIN, false, false},
    {"lib/{{name}}.d6", WEB_LIB, false, false},
    {"test/{{name}}_test.d6", WEB_TEST, false, false}, COMMON_FILES,
};
static const NewFile TCP_FILES[] = {
    {"diamond.cut", TCP_CUT, false, false}, {"README.md", TCP_README, false, false},
    {".gitignore", APP_GITIGNORE, false, false}, {"main.d6", TCP_MAIN, false, false},
    {"lib/{{name}}.d6", TCP_LIB, false, false},
    {"test/{{name}}_test.d6", TCP_TEST, false, false}, COMMON_FILES,
};
static const NewFile UDP_FILES[] = {
    {"diamond.cut", UDP_CUT, false, false}, {"README.md", UDP_README, false, false},
    {".gitignore", APP_GITIGNORE, false, false}, {"main.d6", UDP_MAIN, false, false},
    {"lib/{{name}}.d6", UDP_LIB, false, false},
    {"test/{{name}}_test.d6", UDP_TEST, false, false}, COMMON_FILES,
};
static const NewFile LIB_FILES[] = {
    {"diamond.cut", LIB_CUT, false, false}, {"README.md", LIB_README, false, false},
    {"LICENSE", LIB_LICENSE, false, false}, {".gitignore", LIB_GITIGNORE, false, false},
    {"lib/{{name}}.d6", LIB_LIB, false, false},
    {"test/{{name}}_test.d6", LIB_TEST, false, false}, COMMON_FILES,
};

/* Names a web project cannot take: its dependency cuts, and the classes
 * they define. */
static const char *const WEB_RESERVED[] = {
    "gremlin", "rack", "http", "logger",
    "GremlinShutdown", "NonblockingConnection", "ContentNegotiation", "Cors",
    "RackChain", "RackHeaders", "RateLimit", "SecurityHeaders", "StaticFiles",
    "HttpRequestError", "HttpSession", "Logger", "RequestLogging", nullptr,
};

static const struct {
    const char *kind;
    const char *summary;
    const NewFile *files;
    size_t file_count;
    const char *const *reserved;
} KINDS[] = {
    {"cli", "command-line program", CLI_FILES, sizeof CLI_FILES / sizeof CLI_FILES[0], nullptr},
    {"web", "web application (gremlin + rack)", WEB_FILES,
     sizeof WEB_FILES / sizeof WEB_FILES[0], WEB_RESERVED},
    {"tcp", "line-based TCP server", TCP_FILES, sizeof TCP_FILES / sizeof TCP_FILES[0], nullptr},
    {"udp", "UDP server", UDP_FILES, sizeof UDP_FILES / sizeof UDP_FILES[0], nullptr},
    {"lib", "library cut to publish", LIB_FILES, sizeof LIB_FILES / sizeof LIB_FILES[0], nullptr},
};

/* The project's module is the CamelCase form of its name, so a name whose
 * CamelCase form is a built-in namespace, type, prelude or error class
 * would shadow it (`module File` hides File.open), and the minitest
 * cut's classes would be defined twice. tests/facet_new_test.sh re-derives this
 * list from the compiler and lib/ sources. */
static const char *const RESERVED[] = {
    /* builtin_namespace_calls (src/compiler_names.c) */
    "Fiber", "File", "Dir", "Regexp", "Tensor", "Time", "Process", "BCrypt",
    "Cipher", "Gzip", "Base64", "SecureRandom", "Digest", "ProgramBuilder",
    "Thread", "Channel", "Supervisor", "Array", "Signal",
    /* built-in type names (src/compiler.c) */
    "Int", "Float", "String", "Bool", "Nil", "Hash", "Callable", "Sized", "Symbol",
    /* built-in classes (src/compiler_program.c) */
    "Exception", "StandardError", "RuntimeError", "TypeError", "ArgumentError",
    "IndexError", "ZeroDivisionError", "RangeError", "SystemStackError",
    "FiberError", "RegexpError", "WouldBlockError", "ThreadError",
    "NoMethodError", "SupervisorError", "SandboxError", "ResourceLimitError",
    "FrozenError",
    /* lib/core.d6 and lib/core/ */
    "Comparable", "Enumerable", "LazyEnumerator", "Math", "Range", "StringBuilder",
    /* the minitest cut (lib/minitest.d6) */
    "Minitest", "AssertionError", "Skip", nullptr,
};

static bool in_list(const char *const *list, const char *name) {
    for (; list != nullptr && *list != nullptr; list++)
        if (strcmp(*list, name) == 0) return true;
    return false;
}

/* my_app -> MyApp. The name is already lowercase ASCII, digits and
 * underscores starting with a letter, so the result is a constant name. */
static void camel_case(const char *name, char *out, size_t out_size) {
    size_t length = 0;
    bool upper = true;
    for (; *name != '\0' && length + 1 < out_size; name++) {
        if (*name == '_') {
            upper = true;
            continue;
        }
        out[length++] = upper ? (char)toupper((unsigned char)*name) : *name;
        upper = false;
    }
    out[length] = '\0';
}

/* A maintainer value from git config goes inside a manifest string
 * literal, so it must be plain printable ASCII with no quote, backslash
 * or interpolation; anything else falls back to a placeholder. */
static bool manifest_safe(const char *text, size_t max_length) {
    const size_t length = strlen(text);
    if (length == 0 || length > max_length || text[0] == ' ' || text[length - 1] == ' ')
        return false;
    for (const char *c = text; *c != '\0'; c++)
        if (*c < 0x20 || *c > 0x7e || *c == '"' || *c == '\\' || *c == '#')
            return false;
    return true;
}

static bool git_config(const char *key, char *out, size_t out_size) {
    char *argv[] = {"git", "config", "--get", (char *)key, nullptr};
    char error[256];
    return run_git_capture(argv, "reading git config", out, out_size, error, sizeof error);
}

typedef struct NewValues {
    const char *name;
    char camel[FACET_MAX_NAME];
    char year[16];
    char maintainer[96];
    char contact[224];
} NewValues;

static const char *placeholder_value(const NewValues *values, const char *key,
                                     size_t key_length) {
    if (key_length == 4 && memcmp(key, "name", 4) == 0) return values->name;
    if (key_length == 4 && memcmp(key, "Name", 4) == 0) return values->camel;
    if (key_length == 4 && memcmp(key, "year", 4) == 0) return values->year;
    if (key_length == 10 && memcmp(key, "maintainer", 10) == 0) return values->maintainer;
    if (key_length == 7 && memcmp(key, "contact", 7) == 0) return values->contact;
    return nullptr;
}

/* Writes text to file, replacing each {{key}} with its value. */
static bool write_substituted(FILE *file, const char *text, const NewValues *values) {
    while (*text != '\0') {
        const char *open = strstr(text, "{{");
        const char *close = open != nullptr ? strstr(open + 2, "}}") : nullptr;
        const char *value = close != nullptr
            ? placeholder_value(values, open + 2, (size_t)(close - open - 2)) : nullptr;
        if (value == nullptr) {
            /* Step past one brace only, so "{{{name}}" still substitutes. */
            const size_t span = open != nullptr ? (size_t)(open - text) + 1 : strlen(text);
            if (fwrite(text, 1, span, file) != span) return false;
            text += span;
            continue;
        }
        const size_t span = (size_t)(open - text);
        if (fwrite(text, 1, span, file) != span || fputs(value, file) < 0) return false;
        text = close + 2;
    }
    return true;
}

static bool substitute_path(const char *pattern, const NewValues *values, char *out,
                            size_t out_size) {
    FILE *file = fmemopen(out, out_size, "w");
    if (file == nullptr) return false;
    const bool ok = write_substituted(file, pattern, values) && fputc('\0', file) != EOF;
    return fclose(file) == 0 && ok;
}

static bool write_new_file(const char *root, const NewFile *spec, const NewValues *values,
                           char *error, size_t error_size) {
    char relative[FACET_MAX_PATH];
    char path[FACET_MAX_PATH];
    if (!substitute_path(spec->path, values, relative, sizeof relative) ||
        snprintf(path, sizeof path, "%s/%s", root, relative) >= (int)sizeof path) {
        (void)snprintf(error, error_size, "path for '%s' is too long", spec->path);
        return false;
    }
    const int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, spec->executable ? 0755 : 0644);
    FILE *file = fd < 0 ? nullptr : fdopen(fd, "wb");
    if (file == nullptr) {
        if (fd >= 0) close(fd);
        (void)snprintf(error, error_size, "cannot create '%s': %s", path, strerror(errno));
        return false;
    }
    const char *text = (const char *)spec->text;
    bool ok = spec->raw ? fputs(text, file) >= 0 : write_substituted(file, text, values);
    if (fclose(file) != 0) ok = false;
    if (!ok) (void)snprintf(error, error_size, "cannot write '%s': %s", path, strerror(errno));
    return ok;
}

static void print_new_usage(void) {
    fputs("usage: facet new <kind> <path>\n\nkinds:\n", stderr);
    for (size_t index = 0; index < sizeof KINDS / sizeof KINDS[0]; index++)
        fprintf(stderr, "  %-4s  %s\n", KINDS[index].kind, KINDS[index].summary);
}

int cmd_new(int argc, char **argv) {
    if (argc != 4) {
        print_new_usage();
        return 64;
    }
    size_t kind = 0;
    while (kind < sizeof KINDS / sizeof KINDS[0] && strcmp(KINDS[kind].kind, argv[2]) != 0)
        kind++;
    if (kind == sizeof KINDS / sizeof KINDS[0]) {
        fprintf(stderr, "facet: unknown project kind '%s'\n", argv[2]);
        print_new_usage();
        return 64;
    }

    /* The project name is the path's last component. */
    char root[FACET_MAX_PATH];
    if (snprintf(root, sizeof root, "%s", argv[3]) >= (int)sizeof root) {
        fprintf(stderr, "facet: path '%s' is too long\n", argv[3]);
        return 64;
    }
    size_t root_length = strlen(root);
    while (root_length > 1 && root[root_length - 1] == '/') root[--root_length] = '\0';
    const char *slash = strrchr(root, '/');
    NewValues values = {.name = slash != nullptr ? slash + 1 : root};
    if (!publishable_name(values.name)) {
        fprintf(stderr,
                "facet: '%s' is not a valid project name: use lowercase ASCII letters, "
                "digits and underscores, starting with a letter\n", values.name);
        return 64;
    }
    camel_case(values.name, values.camel, sizeof values.camel);
    if (in_list(RESERVED, values.camel) || in_list(KINDS[kind].reserved, values.name) ||
        in_list(KINDS[kind].reserved, values.camel)) {
        fprintf(stderr,
                "facet: '%s' would name the project's module %s, which a %s project "
                "already uses; choose another name\n", values.name, values.camel, argv[2]);
        return 64;
    }
    if (file_exists(root)) {
        fprintf(stderr, "facet: '%s' already exists\n", root);
        return 65;
    }

    const time_t now = time(nullptr);
    struct tm today;
    (void)snprintf(values.year, sizeof values.year, "%d",
                   gmtime_r(&now, &today) != nullptr ? today.tm_year + 1900 : 2026);
    bool placeholder_maintainer = false;
    if (strcmp(argv[2], "lib") == 0) {
        if (!git_config("user.name", values.maintainer, sizeof values.maintainer) ||
            !manifest_safe(values.maintainer, 80)) {
            (void)snprintf(values.maintainer, sizeof values.maintainer, "Your Name");
            placeholder_maintainer = true;
        }
        if (!git_config("user.email", values.contact, sizeof values.contact) ||
            !manifest_safe(values.contact, 200) || strchr(values.contact, '@') == nullptr ||
            strchr(values.contact, ' ') != nullptr) {
            (void)snprintf(values.contact, sizeof values.contact, "you@example.com");
            placeholder_maintainer = true;
        }
    }

    char lib_dir[FACET_MAX_PATH], test_dir[FACET_MAX_PATH];
    if (snprintf(lib_dir, sizeof lib_dir, "%s/lib", root) >= (int)sizeof lib_dir ||
        snprintf(test_dir, sizeof test_dir, "%s/test", root) >= (int)sizeof test_dir) {
        fprintf(stderr, "facet: path '%s' is too long\n", root);
        return 64;
    }
    if (mkdir(root, 0755) != 0) {
        fprintf(stderr, "facet: cannot create '%s': %s\n", root, strerror(errno));
        return 73;
    }
    char error[FACET_MAX_PATH + 128] = "";
    bool ok = mkdir(lib_dir, 0755) == 0 && mkdir(test_dir, 0755) == 0;
    if (!ok) (void)snprintf(error, sizeof error, "cannot create directories in '%s': %s",
                            root, strerror(errno));
    for (size_t index = 0; ok && index < KINDS[kind].file_count; index++)
        ok = write_new_file(root, &KINDS[kind].files[index], &values, error, sizeof error);
    if (!ok) {
        fprintf(stderr, "facet: %s\n", error);
        (void)remove_directory_recursive(root);
        return 73;
    }

    printf("facet: created %s %s in %s\n", argv[2], values.name, root);
    if (placeholder_maintainer)
        printf("facet: set the maintainer in diamond.cut and LICENSE "
               "(git config user.name/user.email were not usable)\n");
    printf("\n  cd %s\n", root);
    printf("  facet install\n");
    printf("  bash test.sh\n");
    if (strcmp(argv[2], "lib") != 0) printf("  diamond main.d6\n");
    return 0;
}
