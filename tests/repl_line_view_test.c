/* Checks the REPL line editor's redraw against a small terminal model: input
 * longer than the terminal is wide must wrap onto further rows, with nothing
 * repeated and the cursor where the edit is. */
#define _POSIX_C_SOURCE 200809L
#include "repl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ROWS = 12, COLUMNS = 10, START_ROW = 2 };

typedef struct Screen {
    char cells[ROWS][COLUMNS];
    int row, column;
    /* Set after writing the last column: the next character wraps first. */
    int pending_wrap;
} Screen;

static int failures = 0;

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "repl_line_view_test: %s\n", what);
        failures++;
    }
}

static void clear_from(Screen *screen, int row, int column) {
    for (int r = row; r < ROWS; r++)
        for (int c = (r == row ? column : 0); c < COLUMNS; c++) screen->cells[r][c] = ' ';
}

/* Applies the bytes the editor wrote: text, \r, \n and the CSI A, C, J, K it uses. */
static void apply(Screen *screen, const char *bytes, size_t length) {
    for (size_t i = 0; i < length; i++) {
        const char byte = bytes[i];
        if (byte == '\x1b' && i + 1 < length && bytes[i + 1] == '[') {
            size_t j = i + 2;
            int count = 0, has_count = 0;
            while (j < length && bytes[j] >= '0' && bytes[j] <= '9') {
                count = count * 10 + (bytes[j] - '0');
                has_count = 1;
                j++;
            }
            if (!has_count) count = 1;
            const char final = j < length ? bytes[j] : '\0';
            if (final == 'A') screen->row -= count;
            else if (final == 'C') screen->column += count;
            else if (final == 'J') clear_from(screen, screen->row, screen->column);
            else if (final == 'K') {
                for (int c = screen->column; c < COLUMNS; c++) screen->cells[screen->row][c] = ' ';
            } else check(0, "unexpected escape sequence");
            if (screen->column >= COLUMNS) screen->column = COLUMNS - 1;
            check(screen->row >= START_ROW, "cursor moved above the prompt's row");
            screen->pending_wrap = 0;
            i = j;
            continue;
        }
        if (byte == '\r') {
            screen->column = 0;
            screen->pending_wrap = 0;
        } else if (byte == '\n') {
            screen->row++;
            screen->pending_wrap = 0;
        } else {
            if (screen->pending_wrap) {
                screen->row++;
                screen->column = 0;
                screen->pending_wrap = 0;
            }
            if (screen->row < ROWS) screen->cells[screen->row][screen->column] = byte;
            if (screen->column == COLUMNS - 1) screen->pending_wrap = 1;
            else screen->column++;
        }
    }
}

static void redraw(Screen *screen, ReplLineView *view, const char *buffer, size_t cursor) {
    char *bytes = nullptr;
    size_t length = 0;
    FILE *out = open_memstream(&bytes, &length);
    if (out == nullptr) {
        perror("open_memstream");
        exit(1);
    }
    (void)repl_redraw_line(out, view, COLUMNS, "{> ", buffer, cursor);
    fclose(out);
    apply(screen, bytes, length);
    free(bytes);
}

/* The rows from the prompt's first row should read `text` laid out at COLUMNS
 * wide, with every later row blank. */
static void check_screen(const Screen *screen, const char *text, const char *what) {
    const size_t length = strlen(text);
    for (int r = START_ROW; r < ROWS; r++) {
        for (int c = 0; c < COLUMNS; c++) {
            const size_t index = (size_t)(r - START_ROW) * COLUMNS + (size_t)c;
            const char expected = index < length ? text[index] : ' ';
            if (screen->cells[r][c] != expected) {
                fprintf(stderr, "repl_line_view_test: %s: row %d column %d is '%c', expected '%c'\n",
                        what, r, c, screen->cells[r][c], expected);
                failures++;
                return;
            }
        }
    }
}

static void check_cursor(const Screen *screen, size_t position, const char *what) {
    const int row = START_ROW + (int)(position / COLUMNS);
    const int column = (int)(position % COLUMNS);
    if (screen->row != row || screen->column != column) {
        fprintf(stderr, "repl_line_view_test: %s: cursor at %d,%d, expected %d,%d\n", what,
                screen->row, screen->column, row, column);
        failures++;
    }
}

int main(void) {
    Screen screen;
    memset(screen.cells, ' ', sizeof screen.cells);
    screen.row = START_ROW;
    screen.column = 0;
    screen.pending_wrap = 0;
    apply(&screen, "{> ", 3);
    ReplLineView view = {};

    /* Typing one character at a time, past two full rows. */
    const char *typed = "abcdefghijklmnopqrstuvw";
    char buffer[64] = "";
    char line[80];
    for (size_t i = 0; typed[i] != '\0'; i++) {
        buffer[i] = typed[i];
        buffer[i + 1] = '\0';
        redraw(&screen, &view, buffer, i + 1);
        snprintf(line, sizeof line, "{> %s", buffer);
        check_screen(&screen, line, "typing");
        check_cursor(&screen, 3 + i + 1, "typing");
    }

    /* Moving left across a row boundary, then back to the end. */
    const size_t length = strlen(buffer);
    for (size_t cursor = length; cursor-- > 0;) {
        redraw(&screen, &view, buffer, cursor);
        check_screen(&screen, "{> abcdefghijklmnopqrstuvw", "moving left");
        check_cursor(&screen, 3 + cursor, "moving left");
    }
    redraw(&screen, &view, buffer, length);
    check_cursor(&screen, 3 + length, "end");

    /* Deleting from the middle of the second row pulls the third row back up. */
    memmove(buffer + 9, buffer + 10, length - 9);
    redraw(&screen, &view, buffer, 9);
    check_screen(&screen, "{> abcdefghiklmnopqrstuvw", "delete in the middle");
    check_cursor(&screen, 12, "delete in the middle");

    /* Shrinking below one row clears the rows it no longer uses. */
    buffer[3] = '\0';
    redraw(&screen, &view, buffer, 3);
    check_screen(&screen, "{> abc", "shrink");
    check_cursor(&screen, 6, "shrink");

    /* Filling a row exactly leaves the cursor at the start of the next. */
    strcpy(buffer, "abcdefg");
    redraw(&screen, &view, buffer, 7);
    check_screen(&screen, "{> abcdefg", "exact fill");
    check_cursor(&screen, 10, "exact fill");

    if (failures > 0) return 1;
    puts("repl line view tests passed");
    return 0;
}
