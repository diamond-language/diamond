#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE

#include "compiler.h"
#include "json.h"
#include "loader.h"
#include "prelude.h"
#include "rpc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* diamond-dap: a Debug Adapter Protocol server for the v1 compile-time-
 * breakpoint step debugger (see the plan this implements, and
 * docs/debugging.md for the end-user contract). Talks DAP over stdio to
 * an editor (Content-Length-framed JSON, reusing lsp/json.c/lsp/rpc.c the
 * same way diamond-lsp already does -- see lsp/main.c), and separately
 * spawns and controls one `diamond` child process per debug session over
 * a dedicated socketpair "control channel" (DIAMOND_DEBUG_FD, DiamondVm.
 * debug_fd's own comment, src/vm.h) plus two ordinary pipes relaying the
 * child's stdout/stderr as DAP `output` events.
 *
 * v1 scope, matching the plan: one debuggee at a time, no step-over/
 * into/out (`continue` is the only resume command), outer stack frames
 * carry no locals, and changing breakpoints means restarting the whole
 * session (there is no "already running, add a breakpoint" path -- the
 * child isn't spawned at all until `configurationDone`, precisely so
 * every breakpoint already known by then is baked in at compile time).
 *
 * Real DAP ordering (client sends `launch` before it's necessarily done
 * sending `setBreakpoints` for every source -- `configurationDone` is
 * the client's own signal that it's finished) is why `launch` here only
 * *records* the program/args/cwd rather than spawning immediately: the
 * plan's own prose says "launch... spawns diamond", but doing that
 * literally would race a `setBreakpoints` request that arrives after
 * `launch`, silently dropping those breakpoints. Waiting for
 * `configurationDone` (this server declares
 * supportsConfigurationDoneRequest, so a compliant client always sends
 * it) is what actually keeps "every breakpoint already known is baked
 * in" true.
 */

enum {
    DAP_MAX_PATH = DIAMOND_MAX_SOURCE_PATH,
    DAP_MAX_BREAKPOINTS = 256,
    DAP_MAX_ARGS = 64,
    DAP_OUTPUT_CHUNK = 4096,
};

typedef struct StoredBreakpoint {
    char path[DAP_MAX_PATH];
    size_t line;
} StoredBreakpoint;

typedef struct DapServer {
    int next_seq;

    StoredBreakpoint breakpoints[DAP_MAX_BREAKPOINTS];
    size_t breakpoint_count;

    /* Captured by `launch`, consumed by `configurationDone` -- see this
     * file's own top comment for why the spawn itself waits. */
    bool has_launch_config;
    char program_path[DAP_MAX_PATH];
    char cwd[DAP_MAX_PATH];
    char args[DAP_MAX_ARGS][DAP_MAX_PATH];
    size_t arg_count;

    /* Set once the child is actually running (configurationDone). */
    bool launched;
    pid_t child_pid;
    int control_fd;   /* dap's own end of the control socketpair */
    FILE *control_stream;
    int child_stdout_fd;
    int child_stderr_fd;
    bool child_exited;

    /* The exact combined-buffer layout the launched child compiled
     * against -- needed to translate every `stopped` payload's
     * combined-buffer (name,line,column) back to the original file/line
     * a DAP client actually understands. Owned for the life of the
     * session; freed at exit/disconnect. */
    char *combined;
    DiamondSourceBundle bundle;
    size_t user_offset;
    bool bundle_loaded;

    /* The most recent `stopped` payload from the control channel, kept
     * around to answer stackTrace/scopes/variables against -- there is
     * never more than one live pause at a time in v1 (the debuggee is
     * fully blocked on the control fd while paused), so a single slot is
     * enough. */
    JsonValue *stopped_message; /* owns stack/locals; freed on replace */
    const JsonValue *stopped_stack;
    const JsonValue *stopped_locals;
} DapServer;

static void dap_write(JsonValue *message) {
    rpc_write_message(stdout, message);
    json_free(message);
}

static void send_response(DapServer *server, const JsonValue *request,
        bool success, JsonValue *body) {
    double request_seq = 0;
    json_as_number(json_object_get(request, "seq"), &request_seq);
    const char *command_chars = nullptr;
    size_t command_length = 0;
    json_as_string(json_object_get(request, "command"), &command_chars, &command_length);
    JsonValue *message = json_object();
    if(message == nullptr) { json_free(body); return; }
    json_object_set(message, "seq", json_number(server->next_seq++));
    json_object_set(message, "type", json_string_z("response"));
    json_object_set(message, "request_seq", json_number(request_seq));
    json_object_set(message, "success", json_bool(success));
    json_object_set(message, "command",
        command_length > 0 && command_length < 128 ?
            json_string(command_chars, command_length) : json_string_z(""));
    if(body != nullptr) json_object_set(message, "body", body);
    dap_write(message);
}

static void send_event(DapServer *server, const char *event, JsonValue *body) {
    JsonValue *message = json_object();
    if(message == nullptr) { json_free(body); return; }
    json_object_set(message, "seq", json_number(server->next_seq++));
    json_object_set(message, "type", json_string_z("event"));
    json_object_set(message, "event", json_string_z(event));
    if(body != nullptr) json_object_set(message, "body", body);
    dap_write(message);
}

static char *read_file_or_null(const char *path) {
    FILE *file = fopen(path, "rb");
    if(file == nullptr) return nullptr;
    if(fseek(file, 0, SEEK_END) != 0) { fclose(file); return nullptr; }
    const long length = ftell(file);
    if(length < 0 || (unsigned long long)length >= SIZE_MAX) { fclose(file); return nullptr; }
    if(fseek(file, 0, SEEK_SET) != 0) { fclose(file); return nullptr; }
    char *source = malloc((size_t)length + 1);
    if(source == nullptr) { fclose(file); return nullptr; }
    const size_t read_count = fread(source, 1, (size_t)length, file);
    fclose(file);
    if(read_count != (size_t)length) { free(source); return nullptr; }
    source[read_count] = '\0';
    return source;
}

/* The inverse of diamond_combined_buffer_line (compiler.h): given a
 * 1-based combined-buffer `line`/`column`, walks `combined` to find the
 * matching byte offset -- exactly what diamond_resolve_diagnostic_
 * location needs as `diagnostic.span.start` to map a VM pause site back
 * to its original file/line. Best-effort like every other position
 * helper here: returns the offset it got to even if `line` runs past
 * the end of `combined` (shouldn't happen for a line the VM itself just
 * reported), rather than a sentinel a caller might forget to check. */
static size_t combined_buffer_offset_for_line_column(const char *combined,
        size_t line, size_t column) {
    size_t current_line = 1, offset = 0;
    while(current_line < line && combined[offset] != '\0') {
        if(combined[offset] == '\n') current_line++;
        offset++;
    }
    for(size_t moved = 1; moved < column && combined[offset] != '\0' &&
            combined[offset] != '\n'; moved++)
        offset++;
    return offset;
}

/* Loads `program_path` into `server`'s own bundle/combined-buffer state,
 * exactly mirroring run_source_from_bundle_program's own concatenation
 * (src/run_source.c) -- the two must agree byte-for-byte, since
 * DIAMOND_DEBUG_BREAKPOINTS (computed against this same combined buffer)
 * is meaningless otherwise. That function stays static to run_source.c,
 * so this reimplements the small concatenation itself rather than
 * exposing it -- see run_source.c's own note on why a debug launch
 * always takes this exact path (never the embedded-template one). */
static bool load_program_bundle(DapServer *server, const char *program_path,
        char *error, size_t error_capacity) {
    char *source = read_file_or_null(program_path);
    if(source == nullptr) {
        snprintf(error, error_capacity, "cannot read '%s': %s", program_path,
            strerror(errno));
        return false;
    }
    const bool ok = diamond_load_program(program_path, source, &server->bundle,
        error, error_capacity);
    free(source);
    if(!ok) return false;
    const bool include_json = diamond_prelude_needs_json(server->bundle.source);
    const size_t prelude_length = diamond_prelude_length(include_json);
    static constexpr char reset[] = "\n#line 1\n";
    const size_t reset_length = sizeof(reset) - 1;
    const size_t source_length = strlen(server->bundle.source);
    server->combined = malloc(prelude_length + reset_length + source_length + 1);
    if(server->combined == nullptr) {
        snprintf(error, error_capacity, "out of memory building expanded source");
        diamond_source_bundle_free(&server->bundle);
        return false;
    }
    size_t offset = diamond_prelude_write(server->combined, include_json);
    memcpy(server->combined + offset, reset, reset_length); offset += reset_length;
    memcpy(server->combined + offset, server->bundle.source, source_length + 1);
    server->user_offset = prelude_length + reset_length;
    server->bundle_loaded = true;
    return true;
}

/* Resolves one stored (path,line) breakpoint to a combined-buffer line
 * number, or SIZE_MAX if it doesn't land inside any loaded segment --
 * an editor breakpoint in a file this program never actually reaches
 * (stale, or a typo'd path), silently dropped rather than erroring the
 * whole launch over one bad breakpoint. `path` is realpath'd first: every
 * segment (loader.c's own record_segment, called for the entry file too,
 * not just require targets) is recorded under a realpath'd path, and a
 * DAP client's own source path is not guaranteed to already be one. */
static size_t resolve_breakpoint_line(DapServer *server, const char *path, size_t line) {
    char canonical[DAP_MAX_PATH];
    const char *resolved_path = path;
    if(realpath(path, canonical) != nullptr) resolved_path = canonical;
    const size_t offset = diamond_resolve_source_position(resolved_path,
        server->combined, &server->bundle, server->user_offset, line, 1);
    if(offset == SIZE_MAX) return SIZE_MAX;
    return diamond_combined_buffer_line(server->combined, offset);
}

static void free_stopped_payload(DapServer *server) {
    json_free(server->stopped_message);
    server->stopped_message = nullptr;
    server->stopped_stack = nullptr;
    server->stopped_locals = nullptr;
}

static void close_session_fds(DapServer *server) {
    if(server->control_stream != nullptr) { fclose(server->control_stream); server->control_stream = nullptr; }
    else if(server->control_fd >= 0) close(server->control_fd);
    server->control_fd = -1;
    if(server->child_stdout_fd >= 0) { close(server->child_stdout_fd); server->child_stdout_fd = -1; }
    if(server->child_stderr_fd >= 0) { close(server->child_stderr_fd); server->child_stderr_fd = -1; }
}

static void free_session_state(DapServer *server) {
    close_session_fds(server);
    free_stopped_payload(server);
    if(server->bundle_loaded) {
        diamond_source_bundle_free(&server->bundle);
        server->bundle_loaded = false;
    }
    free(server->combined);
    server->combined = nullptr;
    server->launched = false;
    server->child_pid = -1;
}

static void handle_initialize(DapServer *server, const JsonValue *request) {
    JsonValue *capabilities = json_object();
    if(capabilities == nullptr) { send_response(server, request, false, nullptr); return; }
    json_object_set(capabilities, "supportsConfigurationDoneRequest", json_bool(true));
    json_object_set(capabilities, "supportsStepInTargetsRequest", json_bool(false));
    json_object_set(capabilities, "supportsSetVariable", json_bool(false));
    json_object_set(capabilities, "supportsBreakpointLocationsRequest", json_bool(false));
    send_response(server, request, true, capabilities);
    send_event(server, "initialized", nullptr);
}

/* setBreakpoints replaces the previous set for exactly this source path
 * (DAP semantics), not an additive accumulation. */
static void handle_set_breakpoints(DapServer *server, const JsonValue *request,
        const JsonValue *arguments) {
    const JsonValue *source = json_object_get(arguments, "source");
    const char *path_chars = nullptr;
    size_t path_length = 0;
    json_as_string(json_object_get(source, "path"), &path_chars, &path_length);
    char path[DAP_MAX_PATH] = {0};
    if(path_length > 0 && path_length < sizeof path)
        memcpy(path, path_chars, path_length);

    size_t write_index = 0;
    for(size_t index = 0; index < server->breakpoint_count; index++)
        if(strcmp(server->breakpoints[index].path, path) != 0)
            server->breakpoints[write_index++] = server->breakpoints[index];
    server->breakpoint_count = write_index;

    JsonValue *verified = json_array();
    const JsonValue *lines = json_object_get(arguments, "breakpoints");
    const bool is_source_breakpoints = lines != nullptr && lines->kind == JSON_ARRAY;
    if(!is_source_breakpoints) lines = json_object_get(arguments, "lines");
    const size_t count = lines != nullptr && lines->kind == JSON_ARRAY ? lines->as.array.count : 0;
    for(size_t index = 0; index < count; index++) {
        const JsonValue *entry = lines->as.array.items[index];
        double line_value = 0;
        const bool has_line = is_source_breakpoints ?
            json_as_number(json_object_get(entry, "line"), &line_value) :
            json_as_number(entry, &line_value);
        if(!has_line || line_value < 1) continue;
        if(path[0] != '\0' && server->breakpoint_count < DAP_MAX_BREAKPOINTS) {
            StoredBreakpoint *stored = &server->breakpoints[server->breakpoint_count++];
            snprintf(stored->path, sizeof stored->path, "%s", path);
            stored->line = (size_t)line_value;
        }
        JsonValue *breakpoint = json_object();
        if(breakpoint != nullptr) {
            /* Optimistically verified: resolving for real needs the
             * program compiled, which doesn't happen until
             * configurationDone -- see this file's own top comment. A
             * line with no statement start on it silently has no effect
             * at that point instead (compile_sequence's own documented
             * behavior, src/compiler.c), matching how an editor already
             * treats an unreachable gutter breakpoint for most languages. */
            json_object_set(breakpoint, "verified", json_bool(true));
            json_object_set(breakpoint, "line", json_number(line_value));
            json_array_push(verified, breakpoint);
        }
    }
    JsonValue *body = json_object();
    if(body != nullptr) json_object_set(body, "breakpoints", verified);
    else json_free(verified);
    send_response(server, request, true, body);
}

static void handle_launch(DapServer *server, const JsonValue *request,
        const JsonValue *arguments) {
    const char *program_chars = nullptr;
    size_t program_length = 0;
    if(!json_as_string(json_object_get(arguments, "program"), &program_chars, &program_length) ||
       program_length >= sizeof server->program_path) {
        send_response(server, request, false, json_string_z("missing or invalid 'program'"));
        return;
    }
    memcpy(server->program_path, program_chars, program_length);
    server->program_path[program_length] = '\0';

    server->cwd[0] = '\0';
    const char *cwd_chars = nullptr;
    size_t cwd_length = 0;
    if(json_as_string(json_object_get(arguments, "cwd"), &cwd_chars, &cwd_length) &&
       cwd_length < sizeof server->cwd)
        { memcpy(server->cwd, cwd_chars, cwd_length); server->cwd[cwd_length] = '\0'; }

    server->arg_count = 0;
    const JsonValue *args = json_object_get(arguments, "args");
    if(args != nullptr && args->kind == JSON_ARRAY) {
        for(size_t index = 0; index < args->as.array.count && server->arg_count < DAP_MAX_ARGS; index++) {
            const char *arg_chars = nullptr;
            size_t arg_length = 0;
            if(!json_as_string(args->as.array.items[index], &arg_chars, &arg_length) ||
               arg_length >= sizeof server->args[0])
                continue;
            memcpy(server->args[server->arg_count], arg_chars, arg_length);
            server->args[server->arg_count][arg_length] = '\0';
            server->arg_count++;
        }
    }
    server->has_launch_config = true;
    send_response(server, request, true, nullptr);
}

/* The real spawn -- see this file's own top comment for why it waits
 * until here rather than running directly out of `launch`. */
static void handle_configuration_done(DapServer *server, const JsonValue *request) {
    if(!server->has_launch_config) {
        send_response(server, request, false, json_string_z("no launch configuration"));
        return;
    }
    char error[768];
    if(!load_program_bundle(server, server->program_path, error, sizeof error)) {
        send_response(server, request, false, json_string_z(error));
        return;
    }

    size_t breakpoint_lines[DAP_MAX_BREAKPOINTS];
    size_t breakpoint_line_count = 0;
    for(size_t index = 0; index < server->breakpoint_count &&
            breakpoint_line_count < DAP_MAX_BREAKPOINTS; index++) {
        const size_t resolved = resolve_breakpoint_line(server,
            server->breakpoints[index].path, server->breakpoints[index].line);
        if(resolved != SIZE_MAX) breakpoint_lines[breakpoint_line_count++] = resolved;
    }
    char breakpoints_env[DAP_MAX_BREAKPOINTS * 8] = {0};
    size_t written = 0;
    for(size_t index = 0; index < breakpoint_line_count; index++) {
        const int piece = snprintf(breakpoints_env + written, sizeof breakpoints_env - written,
            "%s%zu", index > 0 ? "," : "", breakpoint_lines[index]);
        if(piece < 0 || (size_t)piece >= sizeof breakpoints_env - written) break;
        written += (size_t)piece;
    }

    int control_fds[2];
    if(socketpair(AF_UNIX, SOCK_STREAM, 0, control_fds) != 0) {
        send_response(server, request, false, json_string_z("cannot create control socket"));
        return;
    }
    int stdout_pipe[2], stderr_pipe[2];
    if(pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        send_response(server, request, false, json_string_z("cannot create output pipes"));
        close(control_fds[0]); close(control_fds[1]);
        return;
    }

    const pid_t child = fork();
    if(child < 0) {
        send_response(server, request, false, json_string_z("fork failed"));
        close(control_fds[0]); close(control_fds[1]);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return;
    }
    if(child == 0) {
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        const int null_fd = open("/dev/null", O_RDONLY);
        if(null_fd >= 0) { dup2(null_fd, STDIN_FILENO); close(null_fd); }
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        close(control_fds[0]);
        if(server->cwd[0] != '\0') (void)chdir(server->cwd);
        char debug_fd_value[32];
        snprintf(debug_fd_value, sizeof debug_fd_value, "%d", control_fds[1]);
        setenv("DIAMOND_DEBUG_FD", debug_fd_value, 1);
        if(breakpoint_line_count > 0)
            setenv("DIAMOND_DEBUG_BREAKPOINTS", breakpoints_env, 1);
        const char *diamond_bin = getenv("DIAMOND_BIN");
        if(diamond_bin == nullptr) diamond_bin = "diamond";
        char *argv[4 + DAP_MAX_ARGS];
        size_t argc = 0;
        argv[argc++] = (char *)diamond_bin; /* argv[0]: the executable's own name. */
        argv[argc++] = server->program_path;
        for(size_t index = 0; index < server->arg_count; index++)
            argv[argc++] = server->args[index];
        argv[argc] = nullptr;
        execvp(diamond_bin, argv);
        /* execvp only returns on failure. */
        fprintf(stderr, "diamond-dap: cannot exec '%s': %s\n",
            diamond_bin != nullptr ? diamond_bin : "diamond", strerror(errno));
        _exit(127);
    }

    close(control_fds[1]);
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    server->child_pid = child;
    server->control_fd = control_fds[0];
    server->control_stream = fdopen(control_fds[0], "r+");
    if(server->control_stream != nullptr) setvbuf(server->control_stream, nullptr, _IONBF, 0);
    server->child_stdout_fd = stdout_pipe[0];
    server->child_stderr_fd = stderr_pipe[0];
    server->child_exited = false;
    server->launched = true;
    send_response(server, request, true, nullptr);
}

static void handle_continue(DapServer *server, const JsonValue *request) {
    if(server->control_stream != nullptr) {
        JsonValue *command = json_object();
        if(command != nullptr) {
            json_object_set(command, "command", json_string_z("continue"));
            rpc_write_message(server->control_stream, command);
            json_free(command);
        }
    }
    JsonValue *body = json_object();
    if(body != nullptr) json_object_set(body, "allThreadsContinued", json_bool(true));
    send_response(server, request, true, body);
}

static void handle_threads(DapServer *server, const JsonValue *request) {
    JsonValue *threads = json_array();
    JsonValue *thread = json_object();
    if(threads != nullptr && thread != nullptr) {
        json_object_set(thread, "id", json_number(1));
        json_object_set(thread, "name", json_string_z("main"));
        json_array_push(threads, thread);
    } else {
        json_free(thread);
    }
    JsonValue *body = json_object();
    if(body != nullptr) json_object_set(body, "threads", threads);
    else json_free(threads);
    send_response(server, request, true, body);
}

static void handle_stack_trace(DapServer *server, const JsonValue *request) {
    JsonValue *frames = json_array();
    if(frames != nullptr && server->stopped_stack != nullptr &&
            server->stopped_stack->kind == JSON_ARRAY) {
        for(size_t index = 0; index < server->stopped_stack->as.array.count; index++) {
            const JsonValue *entry = server->stopped_stack->as.array.items[index];
            const char *name_chars = nullptr; size_t name_length = 0;
            double line_value = 0, column_value = 0;
            json_as_string(json_object_get(entry, "name"), &name_chars, &name_length);
            json_as_number(json_object_get(entry, "line"), &line_value);
            json_as_number(json_object_get(entry, "column"), &column_value);
            JsonValue *frame = json_object();
            if(frame == nullptr) continue;
            json_object_set(frame, "id", json_number((double)index));
            json_object_set(frame, "name", name_length > 0 ?
                json_string(name_chars, name_length) : json_string_z("<chunk>"));
            double resolved_line = line_value;
            const char *resolved_path = nullptr;
            if(server->bundle_loaded) {
                const size_t offset = combined_buffer_offset_for_line_column(
                    server->combined, (size_t)line_value, (size_t)column_value);
                DiamondDiagnostic synthetic = {.span = {.start = offset,
                    .line = (size_t)line_value, .column = (size_t)column_value}};
                const DiamondResolvedLocation resolved = diamond_resolve_diagnostic_location(
                    server->program_path, server->combined, synthetic,
                    &server->bundle, server->user_offset);
                resolved_line = (double)resolved.line;
                resolved_path = resolved.path;
            }
            json_object_set(frame, "line", json_number(resolved_line));
            json_object_set(frame, "column", json_number(column_value));
            if(resolved_path != nullptr) {
                JsonValue *source = json_object();
                if(source != nullptr) {
                    json_object_set(source, "name", json_string_z(resolved_path));
                    json_object_set(source, "path", json_string_z(resolved_path));
                    json_object_set(frame, "source", source);
                }
            }
            json_array_push(frames, frame);
        }
    }
    JsonValue *body = json_object();
    if(body != nullptr) {
        json_object_set(body, "stackFrames", frames != nullptr ? frames : json_array());
        json_object_set(body, "totalFrames", json_number(
            frames != nullptr ? (double)frames->as.array.count : 0));
    } else {
        json_free(frames);
    }
    send_response(server, request, true, body);
}

/* v1 only ever hands out one variablesReference (1: the innermost,
 * paused frame's own locals -- outer frames have none, see this file's
 * own top comment) -- frameId 0 gets it, every other frameId gets an
 * empty scopes list. */
static void handle_scopes(DapServer *server, const JsonValue *request,
        const JsonValue *arguments) {
    (void)server;
    double frame_id = -1;
    json_as_number(json_object_get(arguments, "frameId"), &frame_id);
    JsonValue *scopes = json_array();
    if(scopes != nullptr && frame_id == 0) {
        JsonValue *scope = json_object();
        if(scope != nullptr) {
            json_object_set(scope, "name", json_string_z("Locals"));
            json_object_set(scope, "variablesReference", json_number(1));
            json_object_set(scope, "expensive", json_bool(false));
            json_array_push(scopes, scope);
        }
    }
    JsonValue *body = json_object();
    if(body != nullptr) json_object_set(body, "scopes", scopes != nullptr ? scopes : json_array());
    else json_free(scopes);
    send_response(server, request, true, body);
}

static void handle_variables(DapServer *server, const JsonValue *request) {
    JsonValue *variables = json_array();
    if(variables != nullptr && server->stopped_locals != nullptr &&
            server->stopped_locals->kind == JSON_ARRAY) {
        for(size_t index = 0; index < server->stopped_locals->as.array.count; index++) {
            const JsonValue *entry = server->stopped_locals->as.array.items[index];
            const char *name_chars = nullptr; size_t name_length = 0;
            const char *value_chars = nullptr; size_t value_length = 0;
            json_as_string(json_object_get(entry, "name"), &name_chars, &name_length);
            json_as_string(json_object_get(entry, "value"), &value_chars, &value_length);
            JsonValue *variable = json_object();
            if(variable == nullptr) continue;
            json_object_set(variable, "name",
                name_length > 0 ? json_string(name_chars, name_length) : json_string_z(""));
            json_object_set(variable, "value",
                value_length > 0 ? json_string(value_chars, value_length) : json_string_z(""));
            json_object_set(variable, "variablesReference", json_number(0));
            json_array_push(variables, variable);
        }
    }
    JsonValue *body = json_object();
    if(body != nullptr) json_object_set(body, "variables", variables != nullptr ? variables : json_array());
    else json_free(variables);
    send_response(server, request, true, body);
}

/* v1 runs exactly one debug session per diamond-dap process (matching
 * every editor's own convention of spawning a fresh adapter process per
 * session, rather than reusing one across sessions) -- exits right after
 * responding, rather than looping back to accept a second `launch`. */
static void handle_disconnect_or_terminate(DapServer *server, const JsonValue *request) {
    if(server->launched && server->child_pid > 0) kill(server->child_pid, SIGKILL);
    send_response(server, request, true, nullptr);
    if(server->launched && server->child_pid > 0) {
        int status = 0;
        waitpid(server->child_pid, &status, 0);
    }
    free_session_state(server);
    fflush(stdout);
    exit(0);
}

/* Called once the control channel delivers a `{"event":"paused",...}`
 * payload (debugger_structured_helper's own format, src/vm.c) -- stashes
 * it (replacing whatever was stashed before; there is only ever one live
 * pause at a time) and fires the DAP `stopped` event. */
static void handle_stopped_payload(DapServer *server, JsonValue *message) {
    free_stopped_payload(server);
    server->stopped_message = message;
    server->stopped_stack = json_object_get(message, "stack");
    server->stopped_locals = json_object_get(message, "locals");
    JsonValue *body = json_object();
    if(body != nullptr) {
        json_object_set(body, "reason", json_string_z("breakpoint"));
        json_object_set(body, "threadId", json_number(1));
        json_object_set(body, "allThreadsStopped", json_bool(true));
    }
    send_event(server, "stopped", body);
}

static void relay_output(DapServer *server, int fd, const char *category) {
    char buffer[DAP_OUTPUT_CHUNK];
    const ssize_t read_count = read(fd, buffer, sizeof buffer);
    if(read_count <= 0) return;
    JsonValue *body = json_object();
    if(body == nullptr) return;
    json_object_set(body, "category", json_string_z(category));
    json_object_set(body, "output", json_string(buffer, (size_t)read_count));
    send_event(server, "output", body);
}

static void handle_request(DapServer *server, const JsonValue *request) {
    const char *command_chars = nullptr;
    size_t command_length = 0;
    json_as_string(json_object_get(request, "command"), &command_chars, &command_length);
    char command[64] = {0};
    if(command_length > 0 && command_length < sizeof command)
        memcpy(command, command_chars, command_length);
    const JsonValue *arguments = json_object_get(request, "arguments");

    if(strcmp(command, "initialize") == 0) handle_initialize(server, request);
    else if(strcmp(command, "setBreakpoints") == 0) handle_set_breakpoints(server, request, arguments);
    else if(strcmp(command, "launch") == 0) handle_launch(server, request, arguments);
    else if(strcmp(command, "configurationDone") == 0) handle_configuration_done(server, request);
    else if(strcmp(command, "continue") == 0) handle_continue(server, request);
    else if(strcmp(command, "threads") == 0) handle_threads(server, request);
    else if(strcmp(command, "stackTrace") == 0) handle_stack_trace(server, request);
    else if(strcmp(command, "scopes") == 0) handle_scopes(server, request, arguments);
    else if(strcmp(command, "variables") == 0) handle_variables(server, request);
    else if(strcmp(command, "disconnect") == 0) handle_disconnect_or_terminate(server, request);
    else if(strcmp(command, "terminate") == 0) handle_disconnect_or_terminate(server, request);
    else send_response(server, request, false, json_string_z("unsupported request"));
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    setvbuf(stdin, nullptr, _IONBF, 0);
    setvbuf(stdout, nullptr, _IONBF, 0);

    DapServer server = {.next_seq = 1, .control_fd = -1, .child_stdout_fd = -1,
        .child_stderr_fd = -1, .child_pid = -1};

    while(true) {
        struct pollfd fds[4];
        fds[0] = (struct pollfd){.fd = STDIN_FILENO, .events = POLLIN};
        fds[1] = (struct pollfd){.fd = server.launched ? server.control_fd : -1, .events = POLLIN};
        fds[2] = (struct pollfd){.fd = server.launched ? server.child_stdout_fd : -1, .events = POLLIN};
        fds[3] = (struct pollfd){.fd = server.launched ? server.child_stderr_fd : -1, .events = POLLIN};
        const int ready = poll(fds, 4, -1);
        if(ready < 0) { if(errno == EINTR) continue; break; }

        if(fds[0].revents & (POLLIN | POLLHUP)) {
            const char *read_error = nullptr;
            JsonValue *message = rpc_read_message(stdin, &read_error);
            if(message == nullptr) break; /* client closed stdin: session over. */
            handle_request(&server, message);
            json_free(message);
        }
        if(server.launched && (fds[1].revents & (POLLIN | POLLHUP)) && server.control_fd >= 0) {
            const char *read_error = nullptr;
            JsonValue *message = rpc_read_message(server.control_stream, &read_error);
            if(message != nullptr) handle_stopped_payload(&server, message);
        }
        if(server.launched && (fds[2].revents & (POLLIN | POLLHUP)) && server.child_stdout_fd >= 0)
            relay_output(&server, server.child_stdout_fd, "stdout");
        if(server.launched && (fds[3].revents & (POLLIN | POLLHUP)) && server.child_stderr_fd >= 0)
            relay_output(&server, server.child_stderr_fd, "stderr");

        if(server.launched && !server.child_exited) {
            int status = 0;
            const pid_t result = waitpid(server.child_pid, &status, WNOHANG);
            if(result == server.child_pid) {
                server.child_exited = true;
                const int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
                JsonValue *exited_body = json_object();
                if(exited_body != nullptr) {
                    json_object_set(exited_body, "exitCode", json_number(exit_code));
                    send_event(&server, "exited", exited_body);
                }
                send_event(&server, "terminated", nullptr);
                free_session_state(&server);
            }
        }
    }

    if(server.launched && server.child_pid > 0) {
        kill(server.child_pid, SIGKILL);
        int status = 0;
        waitpid(server.child_pid, &status, 0);
    }
    free_session_state(&server);
    return 0;
}
