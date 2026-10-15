/* A VFS for SQLite in the browser build: in-memory databases only. The module has no
 * filesystem a database could live in, and SQLite's own unix VFS wants file locking
 * wasi-libc does not have, so the amalgamation is compiled with SQLITE_OS_OTHER and this
 * supplies the few platform services an in-memory database needs (time, randomness).
 * Opening a file is refused with SQLITE_CANTOPEN, which SQLite3.open reports as an
 * ordinary "cannot open" error. */
#include "sqlite3.h"

#include <string.h>
#include <time.h>
#include <unistd.h>
#if __has_include(<sys/random.h>)
#include <sys/random.h>
#endif

static int memory_open(sqlite3_vfs *vfs, sqlite3_filename name, sqlite3_file *file, int flags,
        int *out_flags) {
    (void)vfs; (void)name; (void)flags; (void)out_flags;
    file->pMethods = NULL;
    return SQLITE_CANTOPEN;
}
static int memory_delete(sqlite3_vfs *vfs, const char *name, int sync) {
    (void)vfs; (void)name; (void)sync; return SQLITE_OK;
}
static int memory_access(sqlite3_vfs *vfs, const char *name, int flags, int *result) {
    (void)vfs; (void)name; (void)flags; *result = 0; return SQLITE_OK;
}
static int memory_full_pathname(sqlite3_vfs *vfs, const char *name, int length, char *out) {
    (void)vfs;
    sqlite3_snprintf(length, out, "%s", name);
    return SQLITE_OK;
}
static int memory_randomness(sqlite3_vfs *vfs, int length, char *out) {
    (void)vfs;
    int done = 0;
    while (done < length) {
        int chunk = length - done > 256 ? 256 : length - done;
        if (getentropy(out + done, (size_t)chunk) != 0) break;
        done += chunk;
    }
    return done;
}
static int memory_sleep(sqlite3_vfs *vfs, int microseconds) {
    (void)vfs; (void)microseconds; return 0;
}
static int memory_current_time(sqlite3_vfs *vfs, double *julian_day) {
    (void)vfs;
    *julian_day = 2440587.5 + (double)time(NULL) / 86400.0;
    return SQLITE_OK;
}
static int memory_last_error(sqlite3_vfs *vfs, int length, char *buffer) {
    (void)vfs; (void)length; (void)buffer; return 0;
}

int sqlite3_os_init(void) {
    static sqlite3_vfs vfs = {
        .iVersion = 1, .szOsFile = sizeof(sqlite3_file), .mxPathname = 512,
        .zName = "diamond-memory",
        .xOpen = memory_open, .xDelete = memory_delete, .xAccess = memory_access,
        .xFullPathname = memory_full_pathname, .xRandomness = memory_randomness,
        .xSleep = memory_sleep, .xCurrentTime = memory_current_time,
        .xGetLastError = memory_last_error,
    };
    return sqlite3_vfs_register(&vfs, 1);
}
int sqlite3_os_end(void) { return SQLITE_OK; }
