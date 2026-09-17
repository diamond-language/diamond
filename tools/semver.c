#include "semver.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool is_identifier_char(char c) {
    return (bool)isalnum((unsigned char)c) || c == '-';
}

static bool is_all_digits(const char *text, size_t length) {
    if (length == 0) return false;
    for (size_t i = 0; i < length; i++)
        if (!isdigit((unsigned char)text[i])) return false;
    return true;
}

/* Parses a plain, unsigned numeric identifier (major/minor/patch itself,
 * not a prerelease identifier) starting at *cursor, rejecting a leading
 * zero unless the whole identifier is exactly "0" -- semver.org's own
 * BNF grammar for these three fields. Advances *cursor past the digits
 * consumed on success. */
static bool parse_numeric_field(const char **cursor, unsigned long *out) {
    const char *start = *cursor;
    if (!isdigit((unsigned char)*start)) return false;
    if (start[0] == '0' && isdigit((unsigned char)start[1])) return false;
    char *end = NULL;
    unsigned long value = strtoul(start, &end, 10);
    if (end == start) return false;
    *cursor = end;
    *out = value;
    return true;
}

/* Validates and copies one dot-separated identifier run (a prerelease
 * or build string, the part after '-' or '+') from *cursor into buffer,
 * stopping at `stop` (another special character the caller's grammar
 * ends this run at, e.g. '+' while scanning a prerelease, or '\0' for
 * build, which always runs to end of string) or end of string.
 * `check_leading_zero` applies semver.org's prerelease-only rule (an
 * all-digit identifier must not have a leading zero unless it's exactly
 * "0") -- build identifiers have no such rule. Every identifier must be
 * non-empty and use only [0-9A-Za-z-]. */
static bool copy_identifier_run(const char **cursor, char stop, bool check_leading_zero,
        char *buffer, size_t buffer_size) {
    const char *start = *cursor;
    const char *identifier_start = *cursor;
    while (**cursor != '\0' && **cursor != stop) {
        char c = **cursor;
        if (c == '.') {
            size_t identifier_length = (size_t)(*cursor - identifier_start);
            if (identifier_length == 0) return false;
            if (check_leading_zero && is_all_digits(identifier_start, identifier_length) &&
                    identifier_length > 1 && identifier_start[0] == '0')
                return false;
            identifier_start = *cursor + 1;
        } else if (!is_identifier_char(c)) {
            return false;
        }
        (*cursor)++;
    }
    size_t last_identifier_length = (size_t)(*cursor - identifier_start);
    if (last_identifier_length == 0) return false;
    if (check_leading_zero && is_all_digits(identifier_start, last_identifier_length) &&
            last_identifier_length > 1 && identifier_start[0] == '0')
        return false;

    size_t total_length = (size_t)(*cursor - start);
    if (total_length == 0 || total_length >= buffer_size) return false;
    memcpy(buffer, start, total_length);
    buffer[total_length] = '\0';
    return true;
}

bool semver_parse(const char *text, Semver *out) {
    if (text == NULL || out == NULL) return false;
    *out = (Semver){0};
    const char *cursor = text;
    if (*cursor == 'v' || *cursor == 'V') cursor++;

    if (!parse_numeric_field(&cursor, &out->major)) return false;
    if (*cursor != '.') return false;
    cursor++;
    if (!parse_numeric_field(&cursor, &out->minor)) return false;
    if (*cursor != '.') return false;
    cursor++;
    if (!parse_numeric_field(&cursor, &out->patch)) return false;

    if (*cursor == '-') {
        cursor++;
        if (!copy_identifier_run(&cursor, '+', true, out->prerelease, sizeof out->prerelease))
            return false;
    }
    if (*cursor == '+') {
        cursor++;
        if (!copy_identifier_run(&cursor, '\0', false, out->build, sizeof out->build))
            return false;
    }
    return *cursor == '\0';
}

bool semver_format(const Semver *v, char *buffer, size_t buffer_size) {
    if (v == NULL || buffer == NULL) return false;
    int written = snprintf(buffer, buffer_size, "%lu.%lu.%lu%s%s%s%s",
        v->major, v->minor, v->patch,
        v->prerelease[0] != '\0' ? "-" : "", v->prerelease,
        v->build[0] != '\0' ? "+" : "", v->build);
    return written >= 0 && (size_t)written < buffer_size;
}

static int compare_unsigned_long(unsigned long a, unsigned long b) {
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

/* Compares two single dot-separated prerelease identifiers per
 * semver.org section 11.4: numeric identifiers compare numerically and
 * always precede (compare lower than) alphanumeric ones; alphanumeric
 * identifiers compare byte-wise (ASCII). Both identifiers are already
 * known-valid (semver_parse rejected anything else), so no further
 * validation happens here. */
static int compare_prerelease_identifier(const char *a, size_t a_length,
        const char *b, size_t b_length) {
    bool a_numeric = is_all_digits(a, a_length);
    bool b_numeric = is_all_digits(b, b_length);
    if (a_numeric && b_numeric) {
        char a_buffer[32] = {0}, b_buffer[32] = {0};
        size_t a_copy = a_length < sizeof a_buffer - 1 ? a_length : sizeof a_buffer - 1;
        size_t b_copy = b_length < sizeof b_buffer - 1 ? b_length : sizeof b_buffer - 1;
        memcpy(a_buffer, a, a_copy);
        memcpy(b_buffer, b, b_copy);
        return compare_unsigned_long(strtoul(a_buffer, NULL, 10), strtoul(b_buffer, NULL, 10));
    }
    if (a_numeric != b_numeric) return a_numeric ? -1 : 1;
    size_t shorter = a_length < b_length ? a_length : b_length;
    int cmp = memcmp(a, b, shorter);
    if (cmp != 0) return cmp < 0 ? -1 : 1;
    return compare_unsigned_long(a_length, b_length);
}

static int compare_prerelease(const char *a, const char *b) {
    const char *a_cursor = a, *b_cursor = b;
    for (;;) {
        bool a_done = *a_cursor == '\0', b_done = *b_cursor == '\0';
        if (a_done && b_done) return 0;
        /* Fewer prerelease fields is lower precedence when every
         * preceding field already compared equal (semver.org 11.4.4). */
        if (a_done) return -1;
        if (b_done) return 1;
        const char *a_start = a_cursor;
        while (*a_cursor != '\0' && *a_cursor != '.') a_cursor++;
        const char *b_start = b_cursor;
        while (*b_cursor != '\0' && *b_cursor != '.') b_cursor++;
        int cmp = compare_prerelease_identifier(a_start, (size_t)(a_cursor - a_start),
            b_start, (size_t)(b_cursor - b_start));
        if (cmp != 0) return cmp;
        if (*a_cursor == '.') a_cursor++;
        if (*b_cursor == '.') b_cursor++;
    }
}

int semver_compare(const Semver *a, const Semver *b) {
    int cmp = compare_unsigned_long(a->major, b->major);
    if (cmp != 0) return cmp;
    cmp = compare_unsigned_long(a->minor, b->minor);
    if (cmp != 0) return cmp;
    cmp = compare_unsigned_long(a->patch, b->patch);
    if (cmp != 0) return cmp;
    bool a_has_prerelease = a->prerelease[0] != '\0';
    bool b_has_prerelease = b->prerelease[0] != '\0';
    if (a_has_prerelease != b_has_prerelease) return a_has_prerelease ? -1 : 1;
    if (!a_has_prerelease) return 0;
    return compare_prerelease(a->prerelease, b->prerelease);
}

/* The version just past the highest one `^v` allows -- semver's own
 * "don't change the left-most non-zero digit" rule: ^1.2.3 allows up to
 * (exclusive) 2.0.0, ^0.2.3 allows up to 0.3.0, ^0.0.3 allows up to
 * 0.0.4. Always inclusive-of-nothing (an exclusive upper bound). */
static Semver caret_upper_bound(Semver v) {
    v.prerelease[0] = '\0';
    v.build[0] = '\0';
    if (v.major > 0) {
        v.major++;
        v.minor = 0;
        v.patch = 0;
    } else if (v.minor > 0) {
        v.minor++;
        v.patch = 0;
    } else {
        v.patch++;
    }
    return v;
}

static Semver tilde_upper_bound(Semver v) {
    v.prerelease[0] = '\0';
    v.build[0] = '\0';
    v.minor++;
    v.patch = 0;
    return v;
}

typedef enum { COMPARE_OP_EXACT, COMPARE_OP_GTE, COMPARE_OP_LTE, COMPARE_OP_GT, COMPARE_OP_LT } CompareOp;

static bool parse_comparator_term(const char *term, CompareOp *op, Semver *version) {
    if (strncmp(term, ">=", 2) == 0) {
        *op = COMPARE_OP_GTE;
        return semver_parse(term + 2, version);
    }
    if (strncmp(term, "<=", 2) == 0) {
        *op = COMPARE_OP_LTE;
        return semver_parse(term + 2, version);
    }
    if (term[0] == '>') {
        *op = COMPARE_OP_GT;
        return semver_parse(term + 1, version);
    }
    if (term[0] == '<') {
        *op = COMPARE_OP_LT;
        return semver_parse(term + 1, version);
    }
    if (term[0] == '=') {
        *op = COMPARE_OP_EXACT;
        return semver_parse(term + 1, version);
    }
    *op = COMPARE_OP_EXACT;
    return semver_parse(term, version);
}

/* Folds one already-parsed comparator term into `out`, rejecting
 * anything that would give a bound two different meanings (a second
 * lower bound, an exact combined with any other term at all). */
static bool apply_comparator(SemverConstraint *out, CompareOp op, const Semver *version) {
    switch (op) {
        case COMPARE_OP_EXACT:
            if (out->has_min || out->has_max) return false;
            out->has_min = true;
            out->has_max = true;
            out->min = *version;
            out->max = *version;
            out->min_inclusive = true;
            out->max_inclusive = true;
            return true;
        case COMPARE_OP_GTE:
        case COMPARE_OP_GT:
            /* Also correctly rejects a second term following an EXACT
             * one above: EXACT already sets has_min itself. */
            if (out->has_min) return false;
            out->has_min = true;
            out->min = *version;
            out->min_inclusive = (op == COMPARE_OP_GTE);
            return true;
        case COMPARE_OP_LTE:
        case COMPARE_OP_LT:
            if (out->has_max) return false;
            out->has_max = true;
            out->max = *version;
            out->max_inclusive = (op == COMPARE_OP_LTE);
            return true;
    }
    return false;
}

bool semver_constraint_parse(const char *text, SemverConstraint *out) {
    if (text == NULL || out == NULL) return false;
    *out = (SemverConstraint){0};

    while (isspace((unsigned char)*text)) text++;
    if (*text == '\0') return false;

    if (*text == '^' || *text == '~') {
        bool caret = *text == '^';
        Semver version;
        if (!semver_parse(text + 1, &version)) return false;
        out->has_min = true;
        out->min = version;
        out->min_inclusive = true;
        out->has_max = true;
        out->max = caret ? caret_upper_bound(version) : tilde_upper_bound(version);
        out->max_inclusive = false;
        return true;
    }

    enum { MAX_TERMS = 4 };
    char buffer[256];
    if (strlen(text) >= sizeof buffer) return false;
    strcpy(buffer, text);

    size_t term_count = 0;
    char *cursor = buffer;
    while (*cursor != '\0') {
        while (isspace((unsigned char)*cursor)) cursor++;
        if (*cursor == '\0') break;
        char *term_start = cursor;
        while (*cursor != '\0' && !isspace((unsigned char)*cursor)) cursor++;
        bool at_end = *cursor == '\0';
        if (!at_end) *cursor++ = '\0';
        if (term_count == MAX_TERMS) return false;
        CompareOp op;
        Semver version;
        if (!parse_comparator_term(term_start, &op, &version)) return false;
        if (!apply_comparator(out, op, &version)) return false;
        term_count++;
        if (at_end) break;
    }
    return term_count > 0;
}

bool semver_satisfies(const Semver *version, const SemverConstraint *constraint) {
    if (version == NULL || constraint == NULL) return false;
    /* Deliberately simpler than semver.org section 9's opt-in
     * prerelease-matching rule -- see semver.h's own comment. */
    if (version->prerelease[0] != '\0') return false;
    if (constraint->has_min) {
        int cmp = semver_compare(version, &constraint->min);
        if (cmp < 0) return false;
        if (cmp == 0 && !constraint->min_inclusive) return false;
    }
    if (constraint->has_max) {
        int cmp = semver_compare(version, &constraint->max);
        if (cmp > 0) return false;
        if (cmp == 0 && !constraint->max_inclusive) return false;
    }
    return true;
}

bool semver_constraint_intersect(const SemverConstraint *a, const SemverConstraint *b,
        SemverConstraint *out) {
    if (a == NULL || b == NULL || out == NULL) return false;
    SemverConstraint result = {0};

    if (a->has_min && b->has_min) {
        int cmp = semver_compare(&a->min, &b->min);
        if (cmp > 0) {
            result.min = a->min;
            result.min_inclusive = a->min_inclusive;
        } else if (cmp < 0) {
            result.min = b->min;
            result.min_inclusive = b->min_inclusive;
        } else {
            result.min = a->min;
            result.min_inclusive = a->min_inclusive && b->min_inclusive;
        }
        result.has_min = true;
    } else if (a->has_min) {
        result.has_min = true;
        result.min = a->min;
        result.min_inclusive = a->min_inclusive;
    } else if (b->has_min) {
        result.has_min = true;
        result.min = b->min;
        result.min_inclusive = b->min_inclusive;
    }

    if (a->has_max && b->has_max) {
        int cmp = semver_compare(&a->max, &b->max);
        if (cmp < 0) {
            result.max = a->max;
            result.max_inclusive = a->max_inclusive;
        } else if (cmp > 0) {
            result.max = b->max;
            result.max_inclusive = b->max_inclusive;
        } else {
            result.max = a->max;
            result.max_inclusive = a->max_inclusive && b->max_inclusive;
        }
        result.has_max = true;
    } else if (a->has_max) {
        result.has_max = true;
        result.max = a->max;
        result.max_inclusive = a->max_inclusive;
    } else if (b->has_max) {
        result.has_max = true;
        result.max = b->max;
        result.max_inclusive = b->max_inclusive;
    }

    if (result.has_min && result.has_max) {
        int cmp = semver_compare(&result.min, &result.max);
        if (cmp > 0) return false;
        if (cmp == 0 && !(result.min_inclusive && result.max_inclusive)) return false;
    }
    *out = result;
    return true;
}

bool semver_constraint_format(const SemverConstraint *constraint, char *buffer, size_t buffer_size) {
    if (constraint == NULL || buffer == NULL) return false;
    if (!constraint->has_min && !constraint->has_max) {
        if (buffer_size < 2) return false;
        strcpy(buffer, "*");
        return true;
    }
    if (constraint->has_min && constraint->has_max && constraint->min_inclusive &&
            constraint->max_inclusive && semver_compare(&constraint->min, &constraint->max) == 0) {
        return semver_format(&constraint->min, buffer, buffer_size);
    }
    char min_text[SEMVER_MAX_IDENTIFIERS * 2] = {0};
    char max_text[SEMVER_MAX_IDENTIFIERS * 2] = {0};
    if (constraint->has_min && !semver_format(&constraint->min, min_text, sizeof min_text))
        return false;
    if (constraint->has_max && !semver_format(&constraint->max, max_text, sizeof max_text))
        return false;
    int written;
    if (constraint->has_min && constraint->has_max) {
        written = snprintf(buffer, buffer_size, "%s%s %s%s",
            constraint->min_inclusive ? ">=" : ">", min_text,
            constraint->max_inclusive ? "<=" : "<", max_text);
    } else if (constraint->has_min) {
        written = snprintf(buffer, buffer_size, "%s%s", constraint->min_inclusive ? ">=" : ">", min_text);
    } else {
        written = snprintf(buffer, buffer_size, "%s%s", constraint->max_inclusive ? "<=" : "<", max_text);
    }
    return written >= 0 && (size_t)written < buffer_size;
}
