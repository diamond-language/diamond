/* Runs a command on a real pseudo-terminal and follows a script against it, for
 * checks that need a terminal: input, resize, and restoring settings on exit.
 *
 *   pty_driver [--timed-read] SCRIPT COMMAND [ARGUMENT...]
 *
 * Script lines, with \r \n \e \\ and \xNN escapes in TEXT:
 *   send TEXT        write to the terminal
 *   expect TEXT      wait for TEXT in output arriving from now on
 *   resize ROWS COLS set the window size
 *   fits ROWS COLS   wait until the latest frame fits that window
 *   framehas TEXT    the frame `fits` accepted contains TEXT
 *   sleep MS         pause, not reading
 *   signal N         signal the command
 *   exit N           wait for the command to exit with status N (128 + signal)
 *   restored         terminal settings match those from before the command ran
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static int master = -1;
static pid_t child = -1;
static char *output = nullptr;
static size_t output_length = 0, output_capacity = 0;
static char *frame = nullptr;
static size_t frame_length = 0;

static double now(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec + (double)time.tv_nsec / 1e9;
}

static void fail(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputs("\n--- last output ---\n", stderr);
    size_t start = output_length > 4000 ? output_length - 4000 : 0;
    for (size_t index = start; index < output_length; index++) {
        unsigned char byte = (unsigned char)output[index];
        if (byte == '\n' || (byte >= 32 && byte < 127)) fputc(byte, stderr);
        else fprintf(stderr, "\\x%02x", byte);
    }
    fputc('\n', stderr);
    if (child > 0) kill(-child, SIGKILL);
    exit(1);
}

/* Appends what arrives within timeout_ms. Returns whether anything did. */
static bool read_output(int timeout_ms) {
    struct pollfd ready = {.fd = master, .events = POLLIN};
    if (poll(&ready, 1, timeout_ms) <= 0) return false;
    if (output_capacity - output_length < 65536) {
        output_capacity = (output_capacity + 65536) * 2;
        output = realloc(output, output_capacity);
        if (output == nullptr) fail("out of memory");
    }
    ssize_t count = read(master, output + output_length, 65536);
    if (count <= 0) fail("terminal closed: %s", count < 0 ? strerror(errno) : "EOF");
    output_length += (size_t)count;
    return true;
}

static void write_terminal(const char *text, size_t length) {
    while (length > 0) {
        ssize_t count = write(master, text, length);
        if (count < 0) fail("terminal write failed: %s", strerror(errno));
        text += count;
        length -= (size_t)count;
        read_output(0); /* a long send can't fill the output side and stall */
    }
}

static size_t unescape(const char *text, char *out) {
    size_t length = 0;
    while (*text != '\0') {
        if (*text != '\\') { out[length++] = *text++; continue; }
        text++;
        switch (*text) {
            case 'r': out[length++] = '\r'; break;
            case 'n': out[length++] = '\n'; break;
            case 'e': out[length++] = '\x1b'; break;
            case '\\': out[length++] = '\\'; break;
            case 'x': {
                char digits[3] = {text[1], text[1] != '\0' ? text[2] : '\0', '\0'};
                out[length++] = (char)strtol(digits, nullptr, 16);
                text += 2;
                break;
            }
            default: fail("bad escape: \\%c", *text);
        }
        text++;
    }
    return length;
}

static void expect_text(const char *marker, size_t length) {
    double deadline = now() + 15;
    size_t start = output_length;
    while (memmem(output + start, output_length - start, marker, length) == nullptr) {
        if (now() > deadline) fail("timed out waiting for: %.*s", (int)length, marker);
        read_output(100);
    }
}

/* The text after the last clear-screen, without control sequences. */
static void latest_frame(void) {
    size_t start = 0;
    for (size_t index = 0; index + 4 <= output_length; index++)
        if (memcmp(output + index, "\x1b[2J", 4) == 0) start = index + 4;
    frame = realloc(frame, output_length - start + 1);
    if (frame == nullptr) fail("out of memory");
    frame_length = 0;
    for (size_t index = start; index < output_length; index++) {
        if (output[index] == '\x1b' && index + 1 < output_length && output[index + 1] == '[') {
            index += 2;
            while (index < output_length && output[index] >= 0x30 && output[index] <= 0x3f) index++;
            while (index < output_length && output[index] >= 0x20 && output[index] <= 0x2f) index++;
            continue; /* the loop's increment skips the final byte */
        }
        frame[frame_length++] = output[index];
    }
}

static bool frame_fits(size_t rows, size_t columns) {
    size_t lines = 1, line_start = 0;
    for (size_t index = 0; index + 1 < frame_length; index++) {
        if (frame[index] == '\r' && frame[index + 1] == '\n') {
            if (index - line_start >= columns) return false;
            lines++;
            line_start = index + 2;
        }
    }
    return lines == rows - 1 && frame_length - line_start < columns;
}

static void wait_for_fit(size_t rows, size_t columns) {
    double deadline = now() + 15;
    for (;;) {
        while (read_output(30)) {}
        latest_frame();
        if (frame_fits(rows, columns)) return;
        /* A frame queried just before the resize can still be in flight. */
        if (now() > deadline) fail("frame never fit %zu rows, %zu columns", rows, columns);
        read_output(100);
    }
}

static void wait_for_exit(int wanted) {
    double deadline = now() + 10;
    for (;;) {
        int status = 0;
        pid_t reaped = waitpid(child, &status, WNOHANG);
        if (reaped == child) {
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            child = -1;
            if (code != wanted) fail("exit status %d, wanted %d", code, wanted);
            return;
        }
        if (now() > deadline) fail("command did not exit");
        read_output(50);
    }
}

static void set_window(int slave, int rows, int columns) {
    struct winsize size = {.ws_row = (unsigned short)rows, .ws_col = (unsigned short)columns};
    if (ioctl(slave, TIOCSWINSZ, &size) != 0) fail("resize failed: %s", strerror(errno));
}

int main(int argc, char **argv) {
    bool timed_read = argc > 1 && strcmp(argv[1], "--timed-read") == 0;
    int first = timed_read ? 2 : 1;
    if (argc < first + 2) {
        fputs("usage: pty_driver [--timed-read] SCRIPT COMMAND [ARGUMENT...]\n", stderr);
        return 64;
    }
    FILE *script = fopen(argv[first], "r");
    if (script == nullptr) fail("cannot open %s: %s", argv[first], strerror(errno));
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) fail("no pty: %s", strerror(errno));
    int slave = open(ptsname(master), O_RDWR | O_NOCTTY);
    if (slave < 0) fail("no pty slave: %s", strerror(errno));
    struct termios settings;
    if (tcgetattr(slave, &settings) != 0) fail("tcgetattr: %s", strerror(errno));
    if (timed_read) {
        settings.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        settings.c_cc[VMIN] = 0;
        settings.c_cc[VTIME] = 1;
        if (tcsetattr(slave, TCSANOW, &settings) != 0) fail("tcsetattr: %s", strerror(errno));
    } else {
        set_window(slave, 24, 100);
    }
    const struct termios original = settings;
    setenv("TERM", "xterm", 1);
    child = fork();
    if (child < 0) fail("fork: %s", strerror(errno));
    if (child == 0) {
        setsid();
        if (ioctl(slave, TIOCSCTTY, 0) != 0) _exit(126);
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        close(master);
        close(slave);
        execvp(argv[first + 1], argv + first + 1);
        _exit(127);
    }
    char line[8192], text[8192];
    size_t line_number = 0;
    while (fgets(line, sizeof line, script) != nullptr) {
        line_number++;
        line[strcspn(line, "\n")] = '\0';
        if (line[0] == '\0') continue;
        char *argument = strchr(line, ' ');
        if (argument != nullptr) *argument++ = '\0';
        size_t length = argument != nullptr ? unescape(argument, text) : 0;
        int first_number = 0, second_number = 0;
        if (argument != nullptr) sscanf(argument, "%d %d", &first_number, &second_number);
        if (strcmp(line, "send") == 0) write_terminal(text, length);
        else if (strcmp(line, "expect") == 0) expect_text(text, length);
        else if (strcmp(line, "resize") == 0) set_window(slave, first_number, second_number);
        else if (strcmp(line, "fits") == 0) wait_for_fit((size_t)first_number, (size_t)second_number);
        else if (strcmp(line, "framehas") == 0) {
            if (memmem(frame, frame_length, text, length) == nullptr)
                fail("line %zu: frame lacks %s", line_number, argument);
        } else if (strcmp(line, "sleep") == 0) usleep((useconds_t)first_number * 1000);
        else if (strcmp(line, "signal") == 0) kill(child, first_number);
        else if (strcmp(line, "exit") == 0) wait_for_exit(first_number);
        else if (strcmp(line, "restored") == 0) {
            struct termios current;
            if (tcgetattr(slave, &current) != 0) fail("tcgetattr: %s", strerror(errno));
            if (current.c_iflag != original.c_iflag || current.c_oflag != original.c_oflag ||
                current.c_cflag != original.c_cflag || current.c_lflag != original.c_lflag ||
                memcmp(current.c_cc, original.c_cc, sizeof current.c_cc) != 0)
                fail("terminal settings were not restored");
        } else fail("line %zu: unknown command %s", line_number, line);
    }
    return 0;
}
