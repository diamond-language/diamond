#define _XOPEN_SOURCE 700
#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#endif
#include "facet_internal.h"

bool publishable_name(const char *name) {
    size_t length = strlen(name);
    if (length == 0 || length >= FACET_MAX_NAME ||
        name[0] < 'a' || name[0] > 'z') return false;
    for (size_t i = 1; i < length; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    }
    return strcmp(name, "diamond") != 0 && strcmp(name, "facet") != 0 &&
           strcmp(name, "cuts") != 0;
}

static bool regular_file(const char *path) {
    struct stat info;
    return lstat(path, &info) == 0 && S_ISREG(info.st_mode);
}

/* Top-level keys accepted in a publishable diamond.cut and in archive
 * manifests; both validators share this list so they cannot drift. */
static const char *const MANIFEST_KEYS[] = {
    "name", "version", "summary", "license", "dependencies", "maintainers",
    "homepage", "source", "documentation", "issues"};

enum { FACET_MAX_MAINTAINERS = 8, FACET_MAX_MAINTAINER_NAME = 80,
       FACET_MAX_CONTACT = 200 };

static bool manifest_key_known(const char *key) {
    for (size_t i = 0; i < sizeof MANIFEST_KEYS / sizeof MANIFEST_KEYS[0]; i++)
        if (strcmp(key, MANIFEST_KEYS[i]) == 0) return true;
    return false;
}

/* Well-formed UTF-8 without C0 controls or DEL. */
static bool printable_utf8(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p != '\0') {
        if (*p < 0x20 || *p == 0x7f) return false;
        if (*p < 0x80) { p++; continue; }
        size_t extra;
        uint32_t code;
        if (*p >= 0xc2 && *p <= 0xdf) { extra = 1; code = *p & 0x1f; }
        else if (*p >= 0xe0 && *p <= 0xef) { extra = 2; code = *p & 0x0f; }
        else if (*p >= 0xf0 && *p <= 0xf4) { extra = 3; code = *p & 0x07; }
        else return false;
        p++;
        for (size_t i = 0; i < extra; i++, p++) {
            if ((*p & 0xc0) != 0x80) return false;
            code = (code << 6) | (*p & 0x3f);
        }
        if ((extra == 2 && (code < 0x800 || (code >= 0xd800 && code <= 0xdfff))) ||
            (extra == 3 && (code < 0x10000 || code > 0x10ffff)))
            return false;
    }
    return true;
}

static bool contact_email(const char *text) {
    const char *at = strchr(text, '@');
    if (at == NULL || strchr(at + 1, '@') != NULL) return false;
    size_t local = (size_t)(at - text);
    const char *domain = at + 1;
    if (local == 0 || local > 64 || text[0] == '.' || at[-1] == '.') return false;
    for (const char *c = text; c < at; c++) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || strchr(".!#$%&'*+/=?^_`{|}~-", *c) != NULL))
            return false;
        if (*c == '.' && c[1] == '.') return false;
    }
    size_t label = 0, labels = 0;
    for (const char *c = domain;; c++) {
        if (*c == '.' || *c == '\0') {
            if (label == 0 || c[-1] == '-') return false;
            labels++;
            label = 0;
            if (*c == '\0') break;
            continue;
        }
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || (*c == '-' && label > 0)))
            return false;
        if (++label > 63) return false;
    }
    return labels >= 2 && strlen(domain) <= 253;
}

static bool contact_https_url(const char *text) {
    if (strncmp(text, "https://", 8) != 0) return false;
    const char *host = text + 8;
    size_t host_length = strcspn(host, "/?#");
    if (host_length == 0 || host[0] == '.' || host[0] == '-' || host[0] == ':') return false;
    for (const unsigned char *c = (const unsigned char *)text; *c; c++)
        if (*c <= 0x20 || *c >= 0x7f || strchr("\"<>\\^`{|}", *c) != NULL) return false;
    for (size_t i = 0; i < host_length; i++)
        if (host[i] == '@') return false;
    return true;
}

/* `maintainers` is public display metadata: 1-8 {name, contact} Hashes with
 * distinct contacts. It grants no registry ownership. Source manifests must
 * declare it; archives published before it existed may omit it. */
static bool validate_maintainers(const DiamondManifestValue *manifest, bool required,
                                 char *error, size_t error_size) {
    const DiamondManifestValue *list = diamond_manifest_get(manifest, "maintainers");
    if (list == NULL) {
        if (!required) return true;
        (void)snprintf(error, error_size,
                       "'maintainers' must list 1-%d maintainers, each with a name and contact",
                       FACET_MAX_MAINTAINERS);
        return false;
    }
    size_t count = 0;
    if (list->kind == DIAMOND_MANIFEST_ARRAY)
        for (const DiamondManifestValue *item = list->children; item != NULL; item = item->next)
            count++;
    if (list->kind != DIAMOND_MANIFEST_ARRAY || count == 0 || count > FACET_MAX_MAINTAINERS) {
        (void)snprintf(error, error_size, "'maintainers' must be an Array of 1-%d entries",
                       FACET_MAX_MAINTAINERS);
        return false;
    }
    size_t index = 0;
    for (const DiamondManifestValue *item = list->children; item != NULL;
         item = item->next, index++) {
        const DiamondManifestValue *name = diamond_manifest_get(item, "name");
        const DiamondManifestValue *contact = diamond_manifest_get(item, "contact");
        size_t keys = 0;
        if (item->kind == DIAMOND_MANIFEST_HASH)
            for (const DiamondManifestValue *key = item->children; key != NULL; key = key->next)
                keys++;
        if (item->kind != DIAMOND_MANIFEST_HASH || keys != 2 || name == NULL ||
            contact == NULL || name->kind != DIAMOND_MANIFEST_STRING ||
            contact->kind != DIAMOND_MANIFEST_STRING) {
            (void)snprintf(error, error_size,
                           "maintainer %zu must be a Hash with exactly String 'name' and 'contact'",
                           index + 1);
            return false;
        }
        size_t name_length = strlen(name->string);
        if (name_length == 0 || name_length > FACET_MAX_MAINTAINER_NAME ||
            name->string[0] == ' ' || name->string[name_length - 1] == ' ' ||
            !printable_utf8(name->string)) {
            (void)snprintf(error, error_size,
                           "maintainer %zu name must be 1-%d bytes of printable UTF-8 without surrounding spaces",
                           index + 1, FACET_MAX_MAINTAINER_NAME);
            return false;
        }
        if (strlen(contact->string) > FACET_MAX_CONTACT ||
            !(contact_email(contact->string) || contact_https_url(contact->string))) {
            (void)snprintf(error, error_size,
                           "maintainer %zu contact must be an email address or https:// URL of at most %d bytes",
                           index + 1, FACET_MAX_CONTACT);
            return false;
        }
        for (const DiamondManifestValue *earlier = list->children; earlier != item;
             earlier = earlier->next) {
            if (strcmp(diamond_manifest_get(earlier, "contact")->string, contact->string) == 0) {
                (void)snprintf(error, error_size, "maintainer %zu repeats a contact", index + 1);
                return false;
            }
        }
    }
    return true;
}

static void print_json_string(const char *text) {
    putchar('"');
    for (const unsigned char *ch = (const unsigned char *)text; *ch; ch++) {
        if (*ch == '"' || *ch == '\\') printf("\\%c", *ch);
        else if (*ch < 32) printf("\\u%04x", (unsigned)*ch);
        else putchar(*ch);
    }
    putchar('"');
}

typedef struct FacetFileList {
    char **paths;
    size_t count;
    size_t capacity;
    unsigned long long total_bytes;
    size_t examined_entries;
} FacetFileList;

static void free_file_list(FacetFileList *list) {
    for (size_t i = 0; i < list->count; i++) free(list->paths[i]);
    free(list->paths);
}

static bool same_folded_path(const char *a, const char *b) {
    for (; *a != '\0' && *b != '\0'; a++, b++) {
        unsigned char left = (unsigned char)*a, right = (unsigned char)*b;
        if (left >= 'A' && left <= 'Z') left = (unsigned char)(left + 32);
        if (right >= 'A' && right <= 'Z') right = (unsigned char)(right + 32);
        if (left != right) return false;
    }
    return *a == *b;
}

static bool sensitive_name(const char *name) {
    size_t length = strlen(name);
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c > 0x7e || c == '\\' || c == ':') return true;
    }
    if (name[0] == '.' || strcmp(name, "id_rsa") == 0 ||
        strcmp(name, "id_ed25519") == 0 || strcmp(name, "credentials") == 0 ||
        strcmp(name, "secrets") == 0) return true;
    const char *suffixes[] = {".pem", ".key", ".p12", ".pfx", ".d6c", ".dic"};
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        size_t suffix_length = strlen(suffixes[i]);
        if (length >= suffix_length &&
            strcmp(name + length - suffix_length, suffixes[i]) == 0) return true;
    }
    return false;
}

static bool add_file(FacetFileList *list, const char *root, const char *relative,
                     char *error, size_t error_size) {
    char path[FACET_MAX_PATH];
    if (snprintf(path, sizeof path, "%s/%s", root, relative) >= (int)sizeof path) {
        (void)snprintf(error, error_size, "path too long: %s", relative);
        return false;
    }
    struct stat info;
    if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
        (void)snprintf(error, error_size, "unsafe file type or hardlink: %s", relative);
        return false;
    }
    if (info.st_size < 0 || (unsigned long long)info.st_size > 10ULL * 1024 * 1024 ||
        list->total_bytes + (unsigned long long)info.st_size > 50ULL * 1024 * 1024) {
        (void)snprintf(error, error_size, "file size limit exceeded: %s", relative);
        return false;
    }
    if (list->count == 4096) {
        (void)snprintf(error, error_size, "too many package files (max 4096)");
        return false;
    }
    for (size_t i = 0; i < list->count; i++) {
        if (same_folded_path(list->paths[i], relative)) {
            (void)snprintf(error, error_size, "case-insensitive path collision: %s", relative);
            return false;
        }
    }
    if (list->count == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        char **grown = realloc(list->paths, new_capacity * sizeof *grown);
        if (grown == NULL) {
            (void)snprintf(error, error_size, "out of memory collecting files");
            return false;
        }
        list->paths = grown;
        list->capacity = new_capacity;
    }
    list->paths[list->count] = strdup(relative);
    if (list->paths[list->count] == NULL) {
        (void)snprintf(error, error_size, "out of memory collecting files");
        return false;
    }
    list->count++;
    list->total_bytes += (unsigned long long)info.st_size;
    return true;
}

static bool scan_runtime_tree(FacetFileList *list, const char *root,
                              const char *relative, unsigned depth,
                              char *error, size_t error_size) {
    if (depth > 32) {
        (void)snprintf(error, error_size, "package tree exceeds 32 levels");
        return false;
    }
    char path[FACET_MAX_PATH];
    if (snprintf(path, sizeof path, "%s/%s", root, relative) >= (int)sizeof path) {
        (void)snprintf(error, error_size, "path too long: %s", relative);
        return false;
    }
    struct stat info;
    if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode)) {
        (void)snprintf(error, error_size, "unsafe directory: %s", relative);
        return false;
    }
    DIR *directory = opendir(path);
    if (directory == NULL) {
        (void)snprintf(error, error_size, "cannot read directory: %s", relative);
        return false;
    }
    bool ok = true;
    char **siblings = NULL;
    size_t sibling_count = 0;
    size_t sibling_capacity = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (++list->examined_entries > 8192) {
            (void)snprintf(error, error_size, "too many package entries (max 8192)");
            ok = false;
            break;
        }
        for (size_t i = 0; i < sibling_count; i++) {
            if (same_folded_path(siblings[i], entry->d_name)) {
                (void)snprintf(error, error_size,
                               "case-insensitive path collision: %s/%s",
                               relative, entry->d_name);
                ok = false;
                break;
            }
        }
        if (!ok) break;
        if (sibling_count == sibling_capacity) {
            size_t new_capacity = sibling_capacity == 0 ? 8 : sibling_capacity * 2;
            char **grown = realloc(siblings, new_capacity * sizeof *grown);
            if (grown == NULL) {
                (void)snprintf(error, error_size, "out of memory collecting paths");
                ok = false;
                break;
            }
            siblings = grown;
            sibling_capacity = new_capacity;
        }
        siblings[sibling_count] = strdup(entry->d_name);
        if (siblings[sibling_count] == NULL) {
            (void)snprintf(error, error_size, "out of memory collecting paths");
            ok = false;
            break;
        }
        sibling_count++;
        if (sensitive_name(entry->d_name)) {
            (void)snprintf(error, error_size, "excluded or sensitive path in runtime tree: %s/%s",
                           relative, entry->d_name);
            ok = false;
            break;
        }
        char child[FACET_MAX_PATH];
        if (snprintf(child, sizeof child, "%s/%s", relative, entry->d_name) >=
            (int)sizeof child || snprintf(path, sizeof path, "%s/%s", root, child) >=
            (int)sizeof path || lstat(path, &info) != 0) {
            (void)snprintf(error, error_size, "cannot inspect package path");
            ok = false;
            break;
        }
        if (S_ISDIR(info.st_mode))
            ok = scan_runtime_tree(list, root, child, depth + 1, error, error_size);
        else
            ok = add_file(list, root, child, error, error_size);
        if (!ok) break;
    }
    closedir(directory);
    for (size_t i = 0; i < sibling_count; i++) free(siblings[i]);
    free(siblings);
    return ok;
}

static int compare_file_paths(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void tar_octal(char *field, size_t width, unsigned long long number) {
    (void)snprintf(field, width, "%0*llo", (int)width - 1, number);
}

void tar_header(unsigned char header[512], const char *relative,
                       unsigned mode, unsigned long long size) {
    memset(header, 0, 512);
    memcpy(header, relative, strlen(relative));
    tar_octal((char *)header + 100, 8, mode);
    tar_octal((char *)header + 108, 8, 0);
    tar_octal((char *)header + 116, 8, 0);
    tar_octal((char *)header + 124, 12, size);
    tar_octal((char *)header + 136, 12, 0);
    memset(header + 148, ' ', 8);
    header[156] = '0';
    memcpy(header + 257, "ustar", 5);
    memcpy(header + 263, "00", 2);
    unsigned checksum = 0;
    for (size_t i = 0; i < 512; i++) checksum += header[i];
    (void)snprintf((char *)header + 148, 7, "%06o", checksum);
    header[155] = ' ';
}

static bool write_tar_member(FILE *archive, const char *root, const char *relative,
                             char *error, size_t error_size) {
    if (strlen(relative) > 99) {
        (void)snprintf(error, error_size, "ustar path exceeds 99 bytes: %s", relative);
        return false;
    }
    char path[FACET_MAX_PATH];
    if (snprintf(path, sizeof path, "%s/%s", root, relative) >= (int)sizeof path) {
        (void)snprintf(error, error_size, "path too long: %s", relative);
        return false;
    }
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) {
        (void)snprintf(error, error_size, "cannot open package file: %s", relative);
        return false;
    }
    struct stat before, after;
    bool ok = fstat(fd, &before) == 0 && S_ISREG(before.st_mode) &&
              before.st_nlink == 1 && before.st_size >= 0 &&
              before.st_size <= 10 * 1024 * 1024;
    if (!ok) {
        (void)snprintf(error, error_size, "package file changed or is unsafe: %s", relative);
        close(fd);
        return false;
    }
    unsigned char header[512];
    tar_header(header, relative, (before.st_mode & 0111) != 0 ? 0755 : 0644,
               (unsigned long long)before.st_size);
    if (fwrite(header, 1, sizeof header, archive) != sizeof header) ok = false;
    unsigned char buffer[8192];
    unsigned long long remaining = (unsigned long long)before.st_size;
    while (ok && remaining > 0) {
        size_t amount = remaining < sizeof buffer ? (size_t)remaining : sizeof buffer;
        ssize_t got = read(fd, buffer, amount);
        if (got <= 0 || fwrite(buffer, 1, (size_t)got, archive) != (size_t)got) {
            ok = false;
            break;
        }
        remaining -= (unsigned long long)got;
    }
    size_t padding = (size_t)((512 - ((unsigned long long)before.st_size % 512)) % 512);
    if (ok && padding > 0) {
        unsigned char zeros[512] = {0};
        if (fwrite(zeros, 1, padding, archive) != padding) ok = false;
    }
    if (fstat(fd, &after) != 0 || after.st_size != before.st_size ||
        after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        after.st_mode != before.st_mode ||
        !same_file_timestamps(&after, &before))
        ok = false;
    close(fd);
    if (!ok) (void)snprintf(error, error_size, "package file changed while packing: %s", relative);
    return ok;
}

static bool digest_stream(FILE *file, unsigned char digest[32],
                          char *error, size_t error_size) {
    EVP_MD_CTX *context = EVP_MD_CTX_new();
    bool ok = context != NULL &&
              EVP_DigestInit_ex(context, EVP_sha256(), NULL) == 1;
    unsigned char buffer[8192];
    while (ok) {
        size_t got = fread(buffer, 1, sizeof buffer, file);
        if (got > 0 && EVP_DigestUpdate(context, buffer, got) != 1) ok = false;
        if (got < sizeof buffer) {
            if (ferror(file)) ok = false;
            break;
        }
    }
    unsigned length = 0;
    if (ok && (EVP_DigestFinal_ex(context, digest, &length) != 1 || length != 32))
        ok = false;
    EVP_MD_CTX_free(context);
    if (!ok) (void)snprintf(error, error_size, "cannot hash archive");
    return ok;
}

bool digest_file(const char *path, unsigned char digest[32],
                        char *error, size_t error_size) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        (void)snprintf(error, error_size, "cannot open archive: %s", strerror(errno));
        return false;
    }
    bool ok = digest_stream(file, digest, error, error_size);
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool pack_tar(const char *root, const FacetFileList *files,
                     const char *output_arg, char *error, size_t error_size) {
    error[0] = '\0';
    char output_copy[FACET_MAX_PATH];
    if (snprintf(output_copy, sizeof output_copy, "%s", output_arg) >=
        (int)sizeof output_copy) {
        (void)snprintf(error, error_size, "output path too long");
        return false;
    }
    char *slash = strrchr(output_copy, '/');
    const char *base = slash == NULL ? output_copy : slash + 1;
    if (base[0] == '\0' || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        (void)snprintf(error, error_size, "invalid output filename");
        return false;
    }
    const char *parent = ".";
    if (slash != NULL) {
        *slash = '\0';
        parent = output_copy[0] == '\0' ? "/" : output_copy;
    }
    char *directory = realpath(parent, NULL);
    if (directory == NULL) {
        (void)snprintf(error, error_size, "cannot resolve output directory");
        return false;
    }
    bool inside_root = strncmp(directory, root, strlen(root)) == 0 &&
        (directory[strlen(root)] == '/' || directory[strlen(root)] == '\0');
    if (inside_root) {
        (void)snprintf(error, error_size, "archive output must be outside the cut directory");
        free(directory);
        return false;
    }
    char output[FACET_MAX_PATH], temporary[FACET_MAX_PATH];
    bool paths_ok = snprintf(output, sizeof output, "%s/%s", directory, base) <
                    (int)sizeof output &&
                    snprintf(temporary, sizeof temporary, "%s/.facet-pack-XXXXXX", directory) <
                    (int)sizeof temporary;
    free(directory);
    if (!paths_ok) {
        (void)snprintf(error, error_size, "output path too long");
        return false;
    }
    int fd = mkstemp(temporary);
    if (fd < 0) {
        (void)snprintf(error, error_size, "cannot create temporary archive: %s", strerror(errno));
        return false;
    }
    FILE *archive = fdopen(fd, "wb");
    if (archive == NULL) {
        close(fd);
        unlink(temporary);
        (void)snprintf(error, error_size, "cannot open temporary archive");
        return false;
    }
    bool ok = true;
    for (size_t i = 0; ok && i < files->count; i++)
        ok = write_tar_member(archive, root, files->paths[i], error, error_size);
    unsigned char zeros[1024] = {0};
    if (ok && fwrite(zeros, 1, sizeof zeros, archive) != sizeof zeros) ok = false;
    if (fclose(archive) != 0) ok = false;
    unsigned char digest[32];
    if (ok) ok = digest_file(temporary, digest, error, error_size);
    if (ok && link(temporary, output) != 0) {
        (void)snprintf(error, error_size, "cannot create '%s': %s", output, strerror(errno));
        ok = false;
    }
    unlink(temporary);
    if (!ok) {
        if (error[0] == '\0')
            (void)snprintf(error, error_size, "cannot write archive");
        return false;
    }
    printf("facet: packed %zu files to %s\nsha256: ", files->count, output);
    for (size_t i = 0; i < sizeof digest; i++) printf("%02x", digest[i]);
    putchar('\n');
    return true;
}

bool tar_number(const unsigned char *field, size_t width,
                       unsigned long long *out) {
    if (field[width - 1] != '\0') return false;
    unsigned long long value = 0;
    for (size_t i = 0; i + 1 < width; i++) {
        if (field[i] < '0' || field[i] > '7') return false;
        value = value * 8 + (unsigned long long)(field[i] - '0');
    }
    *out = value;
    return true;
}

bool all_zero(const unsigned char *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) if (bytes[i] != 0) return false;
    return true;
}

bool valid_archive_path(const char *path) {
    if (strcmp(path, "diamond.cut") == 0 || strcmp(path, "README.md") == 0 ||
        strcmp(path, "LICENSE") == 0) return true;
    const char *prefixes[] = {"lib/", "bin/", "assets/"};
    bool allowed = false;
    for (size_t i = 0; i < sizeof prefixes / sizeof prefixes[0]; i++)
        if (strncmp(path, prefixes[i], strlen(prefixes[i])) == 0) allowed = true;
    if (!allowed) return false;
    const char *component = path;
    for (const char *cursor = path;; cursor++) {
        unsigned char c = (unsigned char)*cursor;
        if (c == '/' || c == '\0') {
            size_t length = (size_t)(cursor - component);
            if (length == 0 || (length == 1 && component[0] == '.') ||
                (length == 2 && component[0] == '.' && component[1] == '.'))
                return false;
            char name[101];
            if (length >= sizeof name) return false;
            memcpy(name, component, length);
            name[length] = '\0';
            if (sensitive_name(name)) return false;
            if (c == '\0') return true;
            component = cursor + 1;
        } else if (c < 0x20 || c > 0x7e || c == '\\' || c == ':') {
            return false;
        }
    }
}

static bool archive_path_conflict(const char *a, const char *b) {
    for (;;) {
        const char *a_end = strchr(a, '/');
        const char *b_end = strchr(b, '/');
        size_t a_length = a_end == NULL ? strlen(a) : (size_t)(a_end - a);
        size_t b_length = b_end == NULL ? strlen(b) : (size_t)(b_end - b);
        if (a_length != b_length) return false;
        bool equal_folded = true;
        for (size_t i = 0; i < a_length; i++) {
            unsigned char left = (unsigned char)a[i], right = (unsigned char)b[i];
            if (left >= 'A' && left <= 'Z') left = (unsigned char)(left + 32);
            if (right >= 'A' && right <= 'Z') right = (unsigned char)(right + 32);
            if (left != right) { equal_folded = false; break; }
        }
        if (!equal_folded) return false;
        if (memcmp(a, b, a_length) != 0) return true;
        if (a_end == NULL || b_end == NULL) return a_end != b_end;
        a = a_end + 1;
        b = b_end + 1;
    }
}

bool expected_digest(const char *text, const unsigned char actual[32]) {
    if (strlen(text) != 64) return false;
    for (size_t i = 0; i < 32; i++) {
        unsigned char digits[2] = {(unsigned char)text[i * 2],
                                   (unsigned char)text[i * 2 + 1]};
        unsigned byte = 0;
        for (size_t j = 0; j < 2; j++) {
            unsigned char c = digits[j];
            if (c >= '0' && c <= '9') byte = byte * 16 + (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') byte = byte * 16 + (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') byte = byte * 16 + (unsigned)(c - 'A' + 10);
            else return false;
        }
        if (byte != actual[i]) return false;
    }
    return true;
}

/* Catch literal imports that would be missing from a standalone artifact.
 * This is a preflight for statically visible imports, not a full Diamond
 * parser: quoted imports at the start of a source line are the common form. */
static bool import_escapes_cut(const char *source_path, const char *target) {
    if (target[0] == '/') return true;
    int depth = 0;
    for (const char *p = source_path; *p != '\0'; p++)
        if (*p == '/') depth++;
    const char *part = target;
    while (*part != '\0') {
        const char *end = strchr(part, '/');
        size_t length = end == NULL ? strlen(part) : (size_t)(end - part);
        if (length == 2 && part[0] == '.' && part[1] == '.') {
            if (--depth < 0) return true;
        } else if (length != 0 && !(length == 1 && part[0] == '.')) {
            depth++;
        }
        if (end == NULL) break;
        part = end + 1;
    }
    return false;
}

static bool runtime_source_path(const char *relative) {
    size_t length = strlen(relative);
    return (strncmp(relative, "lib/", 4) == 0 ||
            strncmp(relative, "bin/", 4) == 0) &&
           length >= 3 && (strcmp(relative + length - 3, ".d6") == 0 ||
                           strcmp(relative + length - 3, ".di") == 0);
}

static bool audit_source_imports(char *source, const char *relative,
                                 const DiamondManifestValue *dependencies,
                                 char *error, size_t error_size) {
        for (char *line = source; *line != '\0';) {
            char *end = strchr(line, '\n');
            if (end != NULL) *end = '\0';
            char *p = line;
            while (*p == ' ' || *p == '\t') p++;
            bool cut = strncmp(p, "require_cut", 11) == 0 &&
                       (p[11] == ' ' || p[11] == '\t');
            bool local = !cut && strncmp(p, "require", 7) == 0 &&
                         (p[7] == ' ' || p[7] == '\t');
            if (cut || local) {
                p += cut ? 11 : 7;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '"') {
                    char *value = ++p;
                    while (*p != '\0' && *p != '"') p++;
                    if (*p == '"') {
                        *p = '\0';
                        if (cut && (dependencies == NULL ||
                            diamond_manifest_get(dependencies, value) == NULL)) {
                            (void)snprintf(error, error_size,
                                "%s imports undeclared cut '%s'", relative, value);
                            return false;
                        }
                        if (local && import_escapes_cut(relative, value)) {
                            (void)snprintf(error, error_size,
                                "%s imports outside the cut: %s", relative, value);
                            return false;
                        }
                    }
                }
            }
            if (end == NULL) break;
            line = end + 1;
        }
    return true;
}

static bool audit_cut_imports(const char *root, const FacetFileList *files,
                              const DiamondManifestValue *dependencies,
                              char *error, size_t error_size) {
    for (size_t i = 0; i < files->count; i++) {
        const char *relative = files->paths[i];
        if (!runtime_source_path(relative)) continue;
        char path[FACET_MAX_PATH];
        if (snprintf(path, sizeof path, "%s/%s", root, relative) >=
            (int)sizeof path) {
            (void)snprintf(error, error_size, "runtime source path is too long");
            return false;
        }
        char *source = read_whole_file(path, error, error_size);
        if (source == NULL) return false;
        bool ok = audit_source_imports(source, relative, dependencies, error, error_size);
        free(source);
        if (!ok) return false;
    }
    return true;
}
/* Reads every entry of a verified-canonical tar archive: each header must be
 * the canonical one for its path, mode and size, paths sorted and unique
 * case-insensitively, within the size limits, padding zeroed, and the
 * archive properly ended. Keeps the manifest, the README (when wanted for
 * --readme) and every runtime source in memory; everything it returns is the
 * caller's to free, including after a failure. */
static bool read_archive_entries(FILE *archive, bool readme, FacetFileList *files_io,
                                 char **runtime_sources, char **manifest_source_out,
                                 char **readme_source_out, size_t *readme_length_out,
                                 char *error, size_t error_size) {
    FacetFileList files = *files_io;
    char *manifest_source = NULL;
    char *readme_source = NULL;
    size_t readme_length = 0;
    unsigned long long total_bytes = 0;
    bool ok = true, ended = false;
    while (ok && !ended) {
        unsigned char header[512];
        if (fread(header, 1, sizeof header, archive) != sizeof header) {
            (void)snprintf(error, error_size, "truncated archive header");
            ok = false;
            break;
        }
        if (all_zero(header, sizeof header)) {
            unsigned char second[512];
            if (fread(second, 1, sizeof second, archive) != sizeof second ||
                !all_zero(second, sizeof second) || fgetc(archive) != EOF ||
                ferror(archive)) {
                (void)snprintf(error, error_size, "invalid archive ending");
                ok = false;
            }
            ended = true;
            break;
        }
        const unsigned char *end = memchr(header, '\0', 100);
        unsigned long long mode, size;
        if (end == NULL || end == header ||
            !tar_number(header + 100, 8, &mode) ||
            !tar_number(header + 124, 12, &size) ||
            (mode != 0644 && mode != 0755) || size > 10ULL * 1024 * 1024 ||
            files.count == 4096 || total_bytes + size > 50ULL * 1024 * 1024) {
            (void)snprintf(error, error_size, "invalid archive entry header or limits");
            ok = false;
            break;
        }
        char path[101];
        size_t path_length = (size_t)(end - header);
        memcpy(path, header, path_length);
        path[path_length] = '\0';
        unsigned char canonical[512];
        tar_header(canonical, path, (unsigned)mode, size);
        if (memcmp(header, canonical, sizeof header) != 0 ||
            !valid_archive_path(path) ||
            (files.count > 0 && strcmp(files.paths[files.count - 1], path) >= 0)) {
            (void)snprintf(error, error_size, "noncanonical or unsafe archive entry: %s", path);
            ok = false;
            break;
        }
        for (size_t i = 0; i < files.count; i++) {
            if (archive_path_conflict(files.paths[i], path)) {
                (void)snprintf(error, error_size,
                               "case-insensitive archive path collision: %s", path);
                ok = false;
                break;
            }
        }
        if (!ok) break;
        if (files.count == files.capacity) {
            size_t capacity = files.capacity == 0 ? 16 : files.capacity * 2;
            char **grown = realloc(files.paths, capacity * sizeof *grown);
            if (grown == NULL) {
                (void)snprintf(error, error_size, "out of memory reading archive");
                ok = false;
                break;
            }
            files.paths = grown;
            files.capacity = capacity;
        }
        files.paths[files.count] = strdup(path);
        if (files.paths[files.count] == NULL) {
            (void)snprintf(error, error_size, "out of memory reading archive");
            ok = false;
            break;
        }
        files.count++;
        total_bytes += size;
        if (strcmp(path, "diamond.cut") == 0) {
            if (size > 1024 * 1024) {
                (void)snprintf(error, error_size, "manifest exceeds 1 MiB");
                ok = false;
                break;
            }
            manifest_source = malloc((size_t)size + 1);
            if (manifest_source == NULL ||
                fread(manifest_source, 1, (size_t)size, archive) != (size_t)size) {
                (void)snprintf(error, error_size, "truncated archive manifest");
                ok = false;
                break;
            }
            manifest_source[size] = '\0';
            if (memchr(manifest_source, '\0', (size_t)size) != NULL) {
                (void)snprintf(error, error_size, "NUL in archive manifest");
                ok = false;
                break;
            }
        } else if (readme && strcmp(path, "README.md") == 0 && size <= 1024 * 1024) {
            readme_source = malloc((size_t)size + 1);
            if (readme_source == NULL ||
                fread(readme_source, 1, (size_t)size, archive) != (size_t)size) {
                (void)snprintf(error, error_size, "truncated archive entry: %s", path);
                ok = false;
                break;
            }
            readme_length = (size_t)size;
        } else if (runtime_source_path(path)) {
            char *source = malloc((size_t)size + 1);
            if (source == NULL || fread(source, 1, (size_t)size, archive) != (size_t)size) {
                free(source);
                (void)snprintf(error, error_size, "truncated archive source: %s", path);
                ok = false;
                break;
            }
            source[size] = '\0';
            if (memchr(source, '\0', (size_t)size) != NULL) {
                free(source);
                (void)snprintf(error, error_size, "NUL in archive source: %s", path);
                ok = false;
                break;
            }
            runtime_sources[files.count - 1] = source;
        } else {
            unsigned char buffer[8192];
            unsigned long long remaining = size;
            while (remaining > 0) {
                size_t amount = remaining < sizeof buffer ? (size_t)remaining : sizeof buffer;
                if (fread(buffer, 1, amount, archive) != amount) {
                    (void)snprintf(error, error_size, "truncated archive entry: %s", path);
                    ok = false;
                    break;
                }
                remaining -= amount;
            }
            if (!ok) break;
        }
        unsigned char padding[512];
        size_t pad = (size_t)((512 - size % 512) % 512);
        if (pad > 0 && (fread(padding, 1, pad, archive) != pad ||
                        !all_zero(padding, pad))) {
            (void)snprintf(error, error_size, "invalid archive padding: %s", path);
            ok = false;
        }
    }
    *files_io = files;
    *manifest_source_out = manifest_source;
    *readme_source_out = readme_source;
    *readme_length_out = readme_length;
    return ok;
}

/* Checks a parsed archive manifest: only known keys, a publishable name and
 * plain semver version, a summary and license, well-formed dependencies,
 * string display fields, maintainers, and imports that only name declared
 * dependencies. */
static bool validate_archive_manifest(const DiamondManifestValue *manifest,
                                      const FacetFileList *files, char **runtime_sources,
                                      const DiamondManifestValue **name_out,
                                      const DiamondManifestValue **version_out,
                                      char *error, size_t error_size) {
    bool ok = true;
    if (ok) {
        for (const DiamondManifestValue *item = manifest->children;
             item != NULL; item = item->next) {
            if (!manifest_key_known(item->key)) {
                (void)snprintf(error, error_size,
                               "unknown archive manifest key: %s", item->key);
                ok = false;
                break;
            }
        }
    }
    const DiamondManifestValue *name = diamond_manifest_get(manifest, "name");
    const DiamondManifestValue *version = diamond_manifest_get(manifest, "version");
    Semver parsed_version;
    if (ok && (name == NULL || name->kind != DIAMOND_MANIFEST_STRING ||
               !publishable_name(name->string) || version == NULL ||
               version->kind != DIAMOND_MANIFEST_STRING ||
               version->string[0] == 'v' || version->string[0] == 'V' ||
               !semver_parse(version->string, &parsed_version) ||
               parsed_version.build[0] != '\0')) {
        (void)snprintf(error, error_size, "archive manifest has invalid name or version");
        ok = false;
    }
    if (ok) {
        const DiamondManifestValue *summary = diamond_manifest_get(manifest, "summary");
        const DiamondManifestValue *license = diamond_manifest_get(manifest, "license");
        if (summary == NULL || summary->kind != DIAMOND_MANIFEST_STRING ||
            summary->string[0] == '\0' || strlen(summary->string) > 160 ||
            strpbrk(summary->string, "\r\n\t") != NULL ||
            license == NULL || license->kind != DIAMOND_MANIFEST_STRING ||
            license->string[0] == '\0') {
            (void)snprintf(error, error_size, "archive manifest is missing valid summary or license");
            ok = false;
        }
    }
    if (ok) {
        const DiamondManifestValue *dependencies = diamond_manifest_get(manifest, "dependencies");
        if (dependencies != NULL && dependencies->kind != DIAMOND_MANIFEST_HASH) {
            (void)snprintf(error, error_size, "archive dependencies must be a Hash");
            ok = false;
        } else if (dependencies != NULL) {
            for (const DiamondManifestValue *dependency = dependencies->children;
                 dependency != NULL; dependency = dependency->next) {
                SemverConstraint range;
                if (!publishable_name(dependency->key) ||
                    dependency->kind != DIAMOND_MANIFEST_STRING ||
                    !semver_constraint_parse(dependency->string, &range)) {
                    (void)snprintf(error, error_size, "invalid archive dependency: %s",
                                   dependency->key);
                    ok = false;
                    break;
                }
            }
        }
    }
    if (ok) {
        const char *display_keys[] = {"homepage", "source", "documentation", "issues"};
        for (size_t i = 0; i < sizeof display_keys / sizeof display_keys[0]; i++) {
            const DiamondManifestValue *display = diamond_manifest_get(manifest, display_keys[i]);
            if (display != NULL && display->kind != DIAMOND_MANIFEST_STRING) {
                (void)snprintf(error, error_size,
                               "archive manifest '%s' must be a String", display_keys[i]);
                ok = false;
                break;
            }
        }
    }
    if (ok) {
        char detail[256];
        if (!validate_maintainers(manifest, false, detail, sizeof detail)) {
            (void)snprintf(error, error_size, "archive manifest %s", detail);
            ok = false;
        }
    }
    if (ok) {
        const DiamondManifestValue *dependencies = diamond_manifest_get(manifest, "dependencies");
        for (size_t i = 0; i < files->count; i++) {
            if (runtime_sources[i] != NULL &&
                !audit_source_imports(runtime_sources[i], files->paths[i], dependencies,
                                      error, error_size)) {
                ok = false;
                break;
            }
        }
    }
    *name_out = name;
    *version_out = version;
    return ok;
}

/* `facet verify --json`: the archive's identity plus the catalog display
 * fields, dependencies and maintainers. */
static void print_verify_json(const DiamondManifestValue *manifest,
                              const DiamondManifestValue *name,
                              const DiamondManifestValue *version, long long size,
                              const unsigned char *digest, size_t digest_size) {
    printf("{\"protocol\":1,\"name\":\"%s\",\"version\":\"%s\",\"size\":%lld,\"sha256\":\"",
           name->string, version->string, size);
    for (size_t i = 0; i < digest_size; i++) printf("%02x", digest[i]);
    /* Catalog display fields; the registry ignores keys it does not use. */
    const char *display_keys[] = {"summary", "license", "homepage", "source",
                                  "documentation", "issues"};
    fputs("\"", stdout);
    for (size_t i = 0; i < sizeof display_keys / sizeof display_keys[0]; i++) {
        const DiamondManifestValue *display = diamond_manifest_get(manifest, display_keys[i]);
        if (display == NULL) continue;
        printf(",\"%s\":", display_keys[i]);
        print_json_string(display->string);
    }
    fputs(",\"dependencies\":{", stdout);
    const DiamondManifestValue *dependencies = diamond_manifest_get(manifest, "dependencies");
    bool first = true;
    for (const DiamondManifestValue *dep = dependencies ? dependencies->children : NULL;
         dep != NULL; dep = dep->next) {
        printf("%s\"%s\":", first ? "" : ",", dep->key);
        first = false;
        print_json_string(dep->string);
    }
    /* Legacy archives without maintainers report an empty list. */
    fputs("},\"maintainers\":[", stdout);
    const DiamondManifestValue *maintainers = diamond_manifest_get(manifest, "maintainers");
    for (const DiamondManifestValue *item = maintainers ? maintainers->children : NULL;
         item != NULL; item = item->next) {
        fputs(item == maintainers->children ? "{\"name\":" : ",{\"name\":", stdout);
        print_json_string(diamond_manifest_get(item, "name")->string);
        fputs(",\"contact\":", stdout);
        print_json_string(diamond_manifest_get(item, "contact")->string);
        putchar('}');
    }
    fputs("]}\n", stdout);
}

int cmd_verify(int argc, char **argv) {
    bool json = false, readme = false;
    const char *expected = NULL;
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0 && !json && !readme) json = true;
        else if (strcmp(argv[i], "--readme") == 0 && !json && !readme) readme = true;
        else if (strcmp(argv[i], "--sha256") == 0 && expected == NULL && i + 1 < argc)
            expected = argv[++i];
        else {
            fputs("usage: facet verify <archive.tar> [--sha256 <digest>] [--json | --readme]\n", stderr);
            return 64;
        }
    }
    if (argc < 3) {
        fputs("usage: facet verify <archive.tar> [--sha256 <digest>] [--json | --readme]\n", stderr);
        return 64;
    }
    char error[512];
    unsigned char digest[32];
    FILE *archive = fopen(argv[2], "rb");
    if (archive == NULL) {
        fprintf(stderr, "facet: cannot open archive: %s\n", strerror(errno));
        return 66;
    }
    struct stat archive_before, archive_after;
    if (fstat(fileno(archive), &archive_before) != 0 ||
        !S_ISREG(archive_before.st_mode) || archive_before.st_size < 0 ||
        archive_before.st_size > 56LL * 1024 * 1024 ||
        !digest_stream(archive, digest, error, sizeof error) ||
        fseek(archive, 0, SEEK_SET) != 0) {
        fclose(archive);
        fputs("facet: cannot hash archive\n", stderr);
        return 66;
    }
    if (expected != NULL && !expected_digest(expected, digest)) {
        fclose(archive);
        fputs("facet: archive SHA-256 does not match expected digest\n", stderr);
        return 65;
    }
    FacetFileList files = {0};
    char **runtime_sources = calloc(4096, sizeof *runtime_sources);
    if (runtime_sources == NULL) {
        fclose(archive);
        fputs("facet: out of memory reading archive\n", stderr);
        return 65;
    }
    char *manifest_source = NULL;
    /* Only read into memory for --readme; display is capped at 1 MiB. */
    char *readme_source = NULL;
    size_t readme_length = 0;
    bool ok = read_archive_entries(archive, readme, &files, runtime_sources, &manifest_source,
                                   &readme_source, &readme_length, error, sizeof error);
    if (fstat(fileno(archive), &archive_after) != 0 ||
        archive_before.st_size != archive_after.st_size ||
        !same_file_timestamps(&archive_before, &archive_after)) {
        (void)snprintf(error, sizeof error, "archive changed while verifying");
        ok = false;
    }
    fclose(archive);
    DiamondManifestValue *manifest = NULL;
    if (ok && manifest_source != NULL) {
        char detail[160];
        manifest = diamond_manifest_parse(manifest_source, detail, sizeof detail);
        if (manifest == NULL) {
            (void)snprintf(error, sizeof error, "invalid archive manifest: %s", detail);
            ok = false;
        }
    }
    if (ok && manifest == NULL) {
        (void)snprintf(error, sizeof error, "archive is missing diamond.cut");
        ok = false;
    }
    const DiamondManifestValue *name = NULL, *version = NULL;
    if (ok)
        ok = validate_archive_manifest(manifest, &files, runtime_sources, &name, &version,
                                       error, sizeof error);
    bool has_readme = false, has_license = false, has_entry = false;
    if (ok) {
        char entry[128], legacy_entry[128];
        (void)snprintf(entry, sizeof entry, "lib/%s.d6", name->string);
        (void)snprintf(legacy_entry, sizeof legacy_entry, "lib/%s.di", name->string);
        for (size_t i = 0; i < files.count; i++) {
            if (strcmp(files.paths[i], "README.md") == 0) has_readme = true;
            if (strcmp(files.paths[i], "LICENSE") == 0) has_license = true;
            if (strcmp(files.paths[i], entry) == 0 ||
                strcmp(files.paths[i], legacy_entry) == 0) has_entry = true;
        }
        if (!has_readme || !has_license || !has_entry) {
            (void)snprintf(error, sizeof error, "archive is missing README.md, LICENSE, or public entry point");
            ok = false;
        }
    }
    if (ok && json) {
        print_verify_json(manifest, name, version, (long long)archive_before.st_size,
                          digest, sizeof digest);
    } else if (ok && readme) {
        if (readme_source == NULL) {
            fputs("facet: README.md exceeds 1 MiB\n", stderr);
            ok = false;
        } else if (readme_length > 0 && fwrite(readme_source, 1, readme_length, stdout) != readme_length) {
            ok = false;
        }
    } else if (ok) {
        printf("facet: verified %s %s (%zu files)\nsha256: ",
               name->string, version->string, files.count);
        for (size_t i = 0; i < sizeof digest; i++) printf("%02x", digest[i]);
        putchar('\n');
    } else {
        fprintf(stderr, "facet: %s\n", error);
    }
    diamond_manifest_free(manifest);
    free(manifest_source);
    free(readme_source);
    for (size_t i = 0; i < files.count; i++) free(runtime_sources[i]);
    free(runtime_sources);
    free_file_list(&files);
    return ok ? 0 : 65;
}

int cmd_check(int argc, char **argv) {
    bool packing = strcmp(argv[1], "pack") == 0;
    bool show_files = !packing && argc == 4 && strcmp(argv[3], "--files") == 0;
    if ((packing && argc != 4) || (!packing && argc != 3 && !show_files)) {
        fputs(packing ? "usage: facet pack <cut-directory> <output.tar>\n" :
                        "usage: facet check <cut-directory> [--files]\n", stderr);
        return 64;
    }
    char *canonical = realpath(argv[2], NULL);
    if (canonical == NULL) {
        fprintf(stderr, "facet: cannot open cut directory '%s': %s\n",
                argv[2], strerror(errno));
        return 66;
    }
    if (strlen(canonical) >= FACET_MAX_PATH) {
        fprintf(stderr, "facet: cut directory path is too long\n");
        free(canonical);
        return 65;
    }
    char root[FACET_MAX_PATH];
    strcpy(root, canonical);
    free(canonical);
    struct stat root_info;
    if (stat(root, &root_info) != 0 || !S_ISDIR(root_info.st_mode)) {
        fprintf(stderr, "facet: '%s' is not a directory\n", argv[2]);
        return 65;
    }
    const char *directory_name = strrchr(root, '/');
    directory_name = directory_name == NULL ? root : directory_name + 1;
    char path[FACET_MAX_PATH];
    if (snprintf(path, sizeof path, "%s/diamond.cut", root) >= (int)sizeof path ||
        !regular_file(path)) {
        fprintf(stderr, "facet: '%s' needs a regular diamond.cut\n", root);
        return 65;
    }
    char error[512];
    DiamondManifestValue *manifest = facet_read_hash(path, error, sizeof error);
    if (manifest == NULL) {
        fprintf(stderr, "facet: %s\n", error);
        return 65;
    }
    int status = 65;
    FacetFileList files = {0};
    for (const DiamondManifestValue *item = manifest->children;
         item != NULL; item = item->next) {
        if (!manifest_key_known(item->key)) {
            fprintf(stderr, "facet: unknown manifest key '%s'\n", item->key);
            goto done;
        }
    }
    const DiamondManifestValue *name = diamond_manifest_get(manifest, "name");
    if (name == NULL || name->kind != DIAMOND_MANIFEST_STRING ||
        !publishable_name(name->string) || strcmp(name->string, directory_name) != 0) {
        fprintf(stderr, "facet: cut name must match its directory and use lowercase ASCII letters, digits, or underscores (starting with a letter)\n");
        goto done;
    }
    const DiamondManifestValue *version = diamond_manifest_get(manifest, "version");
    Semver parsed_version;
    if (version == NULL || version->kind != DIAMOND_MANIFEST_STRING ||
        version->string[0] == 'v' || version->string[0] == 'V' ||
        !semver_parse(version->string, &parsed_version) || parsed_version.build[0] != '\0') {
        fputs("facet: 'version' must be canonical SemVer without a leading v or build metadata\n", stderr);
        goto done;
    }
    const DiamondManifestValue *summary = diamond_manifest_get(manifest, "summary");
    if (summary == NULL || summary->kind != DIAMOND_MANIFEST_STRING ||
        summary->string[0] == '\0' || strlen(summary->string) > 160 ||
        strpbrk(summary->string, "\r\n\t") != NULL) {
        fputs("facet: 'summary' must be a single-line String of 1-160 bytes\n", stderr);
        goto done;
    }
    const DiamondManifestValue *license = diamond_manifest_get(manifest, "license");
    if (license == NULL || license->kind != DIAMOND_MANIFEST_STRING ||
        license->string[0] == '\0') {
        fputs("facet: 'license' must be a nonempty String\n", stderr);
        goto done;
    }
    const DiamondManifestValue *dependencies = diamond_manifest_get(manifest, "dependencies");
    if (dependencies != NULL) {
        if (dependencies->kind != DIAMOND_MANIFEST_HASH) {
            fputs("facet: 'dependencies' must be a Hash\n", stderr);
            goto done;
        }
        for (const DiamondManifestValue *dependency = dependencies->children;
             dependency != NULL; dependency = dependency->next) {
            SemverConstraint range;
            if (!publishable_name(dependency->key) ||
                dependency->kind != DIAMOND_MANIFEST_STRING ||
                !semver_constraint_parse(dependency->string, &range)) {
                fprintf(stderr, "facet: dependency '%s' needs a valid SemVer range String\n",
                        dependency->key);
                goto done;
            }
        }
    }
    const char *display_keys[] = {"homepage", "source", "documentation", "issues"};
    for (size_t i = 0; i < sizeof display_keys / sizeof display_keys[0]; i++) {
        const DiamondManifestValue *display = diamond_manifest_get(manifest, display_keys[i]);
        if (display != NULL && display->kind != DIAMOND_MANIFEST_STRING) {
            fprintf(stderr, "facet: '%s' must be a String\n", display_keys[i]);
            goto done;
        }
    }
    if (!validate_maintainers(manifest, true, error, sizeof error)) {
        fprintf(stderr, "facet: %s\n", error);
        goto done;
    }
    const char *required_files[] = {"README.md", "LICENSE"};
    for (size_t i = 0; i < sizeof required_files / sizeof required_files[0]; i++) {
        if (snprintf(path, sizeof path, "%s/%s", root, required_files[i]) >=
            (int)sizeof path || !regular_file(path)) {
            fprintf(stderr, "facet: '%s' needs a regular %s\n", root,
                    required_files[i]);
            goto done;
        }
    }
    bool has_entry = snprintf(path, sizeof path, "%s/lib/%s.d6", root, name->string) <
        (int)sizeof path && regular_file(path);
    if (!has_entry)
        has_entry = snprintf(path, sizeof path, "%s/lib/%s.di", root, name->string) <
            (int)sizeof path && regular_file(path);
    if (!has_entry) {
        fprintf(stderr, "facet: '%s' needs a regular lib/%s.d6\n", root,
                name->string);
        goto done;
    }
    const char *metadata_files[] = {"diamond.cut", "README.md", "LICENSE"};
    for (size_t i = 0; i < sizeof metadata_files / sizeof metadata_files[0]; i++) {
        if (!add_file(&files, root, metadata_files[i], error, sizeof error)) {
            fprintf(stderr, "facet: %s\n", error);
            goto done;
        }
    }
    const char *runtime_roots[] = {"lib", "bin", "assets"};
    for (size_t i = 0; i < sizeof runtime_roots / sizeof runtime_roots[0]; i++) {
        if (snprintf(path, sizeof path, "%s/%s", root, runtime_roots[i]) >=
            (int)sizeof path) {
            fputs("facet: runtime path is too long\n", stderr);
            goto done;
        }
        struct stat info;
        if (lstat(path, &info) != 0 && errno == ENOENT && i != 0) continue;
        if (!scan_runtime_tree(&files, root, runtime_roots[i], 0,
                               error, sizeof error)) {
            fprintf(stderr, "facet: %s\n", error);
            goto done;
        }
    }
    qsort(files.paths, files.count, sizeof *files.paths, compare_file_paths);
    if (!audit_cut_imports(root, &files, dependencies, error, sizeof error)) {
        fprintf(stderr, "facet: %s\n", error);
        goto done;
    }
    if (show_files || packing)
        for (size_t i = 0; i < files.count; i++) puts(files.paths[i]);
    if (packing) {
        if (!pack_tar(root, &files, argv[3], error, sizeof error)) {
            fprintf(stderr, "facet: %s\n", error);
            goto done;
        }
    } else {
        printf("facet: %s %s is ready for packaging checks\n", name->string,
               version->string);
    }
    status = 0;
done:
    free_file_list(&files);
    diamond_manifest_free(manifest);
    return status;
}

static bool valid_publish_token(const char *token) {
    size_t length = strlen(token);
    if (length == 0 || length > 4096) return false;
    for (size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char)token[i];
        if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\') return false;
    }
    return true;
}

static bool run_curl_publish(const char *url, const char *archive,
                             const char *token, char *error, size_t error_size) {
    char config[] = "/tmp/facet-publish-XXXXXX";
    int fd = mkstemp(config);
    if (fd < 0) {
        (void)snprintf(error, error_size, "cannot create publish configuration: %s",
                       strerror(errno));
        return false;
    }
    (void)fchmod(fd, 0600);
    FILE *file = fdopen(fd, "wb");
    bool ok = file != NULL;
    if (ok) {
        ok = fprintf(file,
            "url = \"%s\"\nrequest = \"POST\"\n"
            "header = \"Authorization: Bearer %s\"\n"
            "header = \"Content-Type: application/octet-stream\"\n"
            "data-binary = \"@%s\"\nfail\nsilent\n"
            "show-error\nproto = \"=https\"\nconnect-timeout = \"10\"\n"
            "max-time = \"60\"\n", url, token, archive) > 0;
        if (fclose(file) != 0) ok = false;
    } else {
        close(fd);
    }
    if (!ok) {
        (void)remove(config);
        (void)snprintf(error, error_size, "cannot write publish configuration");
        return false;
    }
    char *const argv[] = {(char *)"curl", (char *)"--config", config, nullptr};
    const pid_t pid = fork();
    if (pid < 0) {
        (void)remove(config);
        (void)snprintf(error, error_size, "cannot start curl: %s", strerror(errno));
        return false;
    }
    if (pid == 0) {
        execvp("curl", argv);
        fprintf(stderr, "facet: cannot execute 'curl': %s\n", strerror(errno));
        _exit(127);
    }
    int status = 0;
    ok = waitpid(pid, &status, 0) >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    (void)remove(config);
    if (!ok) {
        (void)snprintf(error, error_size, "registry rejected publication");
        return false;
    }
    return true;
}

int cmd_publish(int argc, char **argv) {
    if (argc != 7 || strcmp(argv[3], "--registry") != 0 ||
        strcmp(argv[5], "--token") != 0) {
        fputs("usage: facet publish <cut-directory> --registry <https-url> "
              "--token <token>\n", stderr);
        return 64;
    }
    const char *root_arg = argv[2], *registry = argv[4], *token = argv[6];
    if (!valid_registry_url(registry) || !valid_publish_token(token)) {
        fputs("facet: invalid registry URL or publish token\n", stderr);
        return 64;
    }
    char *root = realpath(root_arg, NULL);
    if (root == NULL || strlen(root) >= FACET_MAX_PATH) {
        free(root);
        fprintf(stderr, "facet: cannot open cut directory '%s'\n", root_arg);
        return 66;
    }
    char manifest_path[FACET_MAX_PATH], error[512] = {0};
    FacetManifest manifest;
    bool ok = snprintf(manifest_path, sizeof manifest_path, "%s/diamond.cut", root) <
              (int)sizeof manifest_path && parse_cut_identity(manifest_path, &manifest,
                                                            error, sizeof error);
    if (!ok) {
        fprintf(stderr, "facet: %s\n", error);
        free(root);
        return 65;
    }
    char archive[] = "/tmp/facet-publish-archive-XXXXXX";
    int archive_fd = mkstemp(archive);
    if (archive_fd < 0) {
        free(root);
        fputs("facet: cannot create publish archive\n", stderr);
        return 74;
    }
    close(archive_fd);
    (void)remove(archive);
    char *pack_argv[] = {(char *)"facet", (char *)"pack", root, archive, nullptr};
    ok = cmd_check(4, pack_argv) == 0;
    char *verify_argv[] = {(char *)"facet", (char *)"verify", archive, nullptr};
    ok = ok && cmd_verify(3, verify_argv) == 0;
    char url[FACET_MAX_PATH];
    if (ok && snprintf(url, sizeof url, "%s/v1/cuts/%s/versions", registry,
                       manifest.name) >= (int)sizeof url) {
        (void)snprintf(error, sizeof error, "publish URL is too long");
        ok = false;
    }
    if (ok) ok = run_curl_publish(url, archive, token, error, sizeof error);
    if (ok)
        printf("facet: published %s %s\n", manifest.name, manifest.version);
    else
        fprintf(stderr, "facet: %s\n", error[0] == '\0' ? "publish failed" : error);
    (void)remove(archive);
    free(root);
    return ok ? 0 : 70;
}
