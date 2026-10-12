/* Unit and corpus checks for lsp/formatting.c: the formatter may only change
 * whitespace (the token stream before and after must be identical), must be
 * idempotent, and applies the spacing, blank-line and range rules documented
 * in docs/lsp.md.
 *
 *   formatting_test               built-in cases only
 *   formatting_test FILE...       also run the invariants over each file */
#include "formatting.h"
#include "json.h"
#include "lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Edit {
    size_t start, end;
    const char *text;
    size_t text_length;
} Edit;

static size_t offset_of(const char *text, size_t length, size_t line, size_t column) {
    size_t current = 0, position = 0;
    while (current < line && position < length) {
        if (text[position] == '\n') current++;
        position++;
    }
    return position + column;
}

static int edit_order(const void *left, const void *right) {
    const Edit *a = left, *b = right;
    if (a->start != b->start) return a->start < b->start ? 1 : -1;
    return a->end < b->end ? 1 : (a->end > b->end ? -1 : 0);
}

/* Applies a TextEdit[] and returns the new text (malloc'd), or nullptr. */
static char *apply(const char *text, size_t length, const JsonValue *edits, size_t *out_length) {
    const size_t count = edits->as.array.count;
    Edit *list = malloc((count == 0 ? 1 : count) * sizeof *list);
    for (size_t index = 0; index < count; index++) {
        const JsonValue *edit = edits->as.array.items[index];
        const JsonValue *range = json_object_get(edit, "range");
        const JsonValue *start = json_object_get(range, "start");
        const JsonValue *end = json_object_get(range, "end");
        double sl = 0, sc = 0, el = 0, ec = 0;
        json_as_number(json_object_get(start, "line"), &sl);
        json_as_number(json_object_get(start, "character"), &sc);
        json_as_number(json_object_get(end, "line"), &el);
        json_as_number(json_object_get(end, "character"), &ec);
        list[index].start = offset_of(text, length, (size_t)sl, (size_t)sc);
        list[index].end = offset_of(text, length, (size_t)el, (size_t)ec);
        json_as_string(json_object_get(edit, "newText"), &list[index].text,
                       &list[index].text_length);
    }
    qsort(list, count, sizeof *list, edit_order);
    char *result = malloc(length + 1 + count * 4 + 64);
    memcpy(result, text, length);
    size_t result_length = length;
    for (size_t index = 0; index < count; index++) {
        if (list[index].end > result_length || list[index].start > list[index].end) {
            free(list);
            free(result);
            return nullptr;
        }
        const size_t tail = result_length - list[index].end;
        memmove(result + list[index].start + list[index].text_length,
                result + list[index].end, tail);
        memcpy(result + list[index].start, list[index].text, list[index].text_length);
        result_length = list[index].start + list[index].text_length + tail;
    }
    free(list);
    result[result_length] = '\0';
    *out_length = result_length;
    return result;
}

static int failures = 0;

static void expect(const char *name, const char *input, const char *expected) {
    JsonValue *edits = formatting_compute(input, strlen(input));
    if (edits == nullptr || edits->kind != JSON_ARRAY) {
        printf("FAIL %s: formatter declined\n", name);
        failures++;
        json_free(edits);
        return;
    }
    size_t out_length = 0;
    char *output = apply(input, strlen(input), edits, &out_length);
    json_free(edits);
    if (output == nullptr || strcmp(output, expected) != 0) {
        printf("FAIL %s\n  expected: %s\n  actual:   %s\n", name, expected,
               output == nullptr ? "(edit error)" : output);
        failures++;
    }
    free(output);
}

static bool same_tokens(const char *left, const char *right) {
    DiamondLexer a, b;
    diamond_lexer_init(&a, left);
    diamond_lexer_init(&b, right);
    for (;;) {
        DiamondToken x = diamond_lexer_next(&a);
        DiamondToken y = diamond_lexer_next(&b);
        /* Blank-line collapsing legitimately changes how many NEWLINE tokens
         * separate two statements; everything else must match exactly. */
        while (x.kind == DIAMOND_TOKEN_NEWLINE) x = diamond_lexer_next(&a);
        while (y.kind == DIAMOND_TOKEN_NEWLINE) y = diamond_lexer_next(&b);
        if (x.kind != y.kind || (x.kind != DIAMOND_TOKEN_EOF && x.kind != DIAMOND_TOKEN_NEWLINE &&
            (x.span.length != y.span.length ||
             memcmp(left + x.span.start, right + y.span.start, x.span.length) != 0))) {
            printf("  first difference near line %zu: %.*s | %.*s\n", x.span.line,
                   (int)x.span.length, left + x.span.start, (int)y.span.length,
                   right + y.span.start);
            return false;
        }
        if (x.kind == DIAMOND_TOKEN_EOF) return true;
    }
}

static int corpus_declined = 0, corpus_checked = 0;

static void check_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == nullptr) return;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = malloc((size_t)size + 1);
    const size_t got = fread(text, 1, (size_t)size, file);
    fclose(file);
    text[got] = '\0';
    JsonValue *edits = formatting_compute(text, got);
    if (edits == nullptr) {
        printf("FAIL %s: allocation failure\n", path);
        failures++;
    } else if (edits->kind != JSON_ARRAY) {
        corpus_declined++;
    } else {
        corpus_checked++;
        size_t out_length = 0;
        char *once = apply(text, got, edits, &out_length);
        if (once == nullptr) {
            printf("FAIL %s: overlapping or out-of-range edits\n", path);
            failures++;
        } else {
            if (!same_tokens(text, once)) {
                printf("FAIL %s: token stream changed\n", path);
                failures++;
            }
            JsonValue *again = formatting_compute(once, out_length);
            if (again != nullptr && again->kind == JSON_ARRAY && again->as.array.count != 0) {
                size_t shown = 0;
                char *dump = json_serialize(again, &shown);
                printf("  second pass edits: %.300s\n", dump);
                free(dump);
            }
            if (again == nullptr || again->kind != JSON_ARRAY || again->as.array.count != 0) {
                printf("FAIL %s: not idempotent (%zu further edits)\n", path,
                       again != nullptr && again->kind == JSON_ARRAY ? again->as.array.count : 0);
                failures++;
            }
            json_free(again);
            free(once);
        }
    }
    json_free(edits);
    free(text);
}

int main(int argc, char **argv) {
    expect("assignment", "x=1\n", "x = 1\n");
    expect("comparison", "if a==b\n  c\nend\n", "if a == b\n  c\nend\n");
    expect("compound", "x+=1\n", "x += 1\n");
    expect("logical", "y = a&&b\n", "y = a && b\n");
    expect("comma after", "f(a,b,c)\n", "f(a, b, c)\n");
    expect("comma before", "f(a , b)\n", "f(a, b)\n");
    expect("trailing comma", "x = [1,2,]\n", "x = [1, 2,]\n");
    expect("arithmetic untouched", "x = a+b*c-d\n", "x = a+b*c-d\n");
    expect("alignment kept", "a    = 1\nbbbb = 2\n", "a    = 1\nbbbb = 2\n");
    expect("operator method", "class A\n  def ==(o)\n    true\n  end\nend\n",
           "class A\n  def ==(o)\n    true\n  end\nend\n");
    expect("setter", "class A\n  def value=(v)\n    @v = v\n  end\nend\n",
           "class A\n  def value=(v)\n    @v = v\n  end\nend\n");
    expect("index setter", "class A\n  def []=(k, v)\n  end\nend\n",
           "class A\n  def []=(k, v)\n  end\nend\n");
    expect("endless def", "def f() = 1\n", "def f() = 1\n");
    expect("default arg", "def f(a=1, b = 2)\n  a\nend\n", "def f(a = 1, b = 2)\n  a\nend\n");
    expect("string untouched", "x = \"a,b==c\"\n", "x = \"a,b==c\"\n");
    expect("comment untouched", "x = 1 # a,b==c\n", "x = 1 # a,b==c\n");
    expect("blank lines", "a = 1\n\n\n\nb = 2\n", "a = 1\n\nb = 2\n");
    expect("blank lines with spaces", "a = 1\n  \n\n \nb = 2\n", "a = 1\n\nb = 2\n");
    expect("one blank kept", "a = 1\n\nb = 2\n", "a = 1\n\nb = 2\n");
    expect("trailing blank lines", "a = 1\n\n\n\n", "a = 1\n\n");
    expect("blank in string", "x = \"a\n\n\n\nb\"\n", "x = \"a\n\n\n\nb\"\n");

    /* Range: only lines inside it are touched, indentation still comes from the whole file. */
    {
        const char *input = "def f()\nx=1\n  y=2\nend\nz=3\n";
        JsonValue *edits = formatting_compute_range(input, strlen(input), 3, 3);
        size_t out_length = 0;
        char *output = edits != nullptr && edits->kind == JSON_ARRAY
            ? apply(input, strlen(input), edits, &out_length) : nullptr;
        if (output == nullptr || strcmp(output, "def f()\nx=1\n  y = 2\nend\nz=3\n") != 0) {
            printf("FAIL range\n  actual: %s\n", output == nullptr ? "(none)" : output);
            failures++;
        }
        free(output);
        json_free(edits);
    }

    for (int index = 1; index < argc; index++) check_file(argv[index]);
    if (argc > 1)
        printf("corpus: %d files checked, %d declined\n", corpus_checked, corpus_declined);
    if (failures != 0) {
        printf("%d formatting test(s) failed\n", failures);
        return 1;
    }
    printf("formatting tests passed\n");
    return 0;
}
