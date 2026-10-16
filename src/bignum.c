/* Needed transitively for vm.h's own <ucontext.h> use (ucontext_t):
 * under -std=c23 (not -std=gnu23), GCC defines __STRICT_ANSI__, which
 * blocks musl's own "default to _BSD_SOURCE+_XOPEN_SOURCE=700 when
 * nothing else is set" fallback (musl's features.h) -- glibc is more
 * lenient here (defaults to roughly the same exposure regardless), so
 * this only surfaced building against musl (see docs/portability.md). Must come before bignum.h's own first #include
 * (vm.h) reaches any libc header -- once <features.h> is processed
 * once per translation unit, a later #define here has no effect. */
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#define __BSD_VISIBLE 1
#define _DARWIN_C_SOURCE
#include "bignum.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Base 10^9 limbs -- see object.h's DiamondBignum for why. All the
 * "magnitude_*" helpers below operate on bare sign-less uint32_t[]
 * digit arrays (little-endian, trimmed -- no trailing zero limbs); the
 * public, sign-aware functions at the bottom combine sign handling,
 * canonicalization (does the true result fit in int64_t?), and GC
 * allocation on top of them.
 *
 * Allocation-failure convention: any function below that can allocate
 * and might fail returns DIAMOND_NIL as a sentinel -- arithmetic can
 * never legitimately produce NIL, so this is unambiguous. Callers in
 * vm.c check for it exactly the way every other allocation site in
 * this codebase checks a `nullptr` return, just adapted to a
 * DiamondValue-returning API. */

enum { DIAMOND_BIGNUM_BASE = 1000000000u };

static size_t magnitude_trim(const uint32_t *limbs, size_t count) {
    while (count > 0 && limbs[count - 1] == 0) count--;
    return count;
}

static int magnitude_compare(const uint32_t *a, size_t a_count,
                             const uint32_t *b, size_t b_count) {
    if (a_count != b_count) return a_count < b_count ? -1 : 1;
    for (size_t i = a_count; i > 0; i--) {
        if (a[i - 1] != b[i - 1]) return a[i - 1] < b[i - 1] ? -1 : 1;
    }
    return 0;
}

/* result must have room for max(a_count,b_count)+1 limbs. */
static size_t magnitude_add(const uint32_t *a, size_t a_count,
                            const uint32_t *b, size_t b_count, uint32_t *result) {
    const size_t count = a_count > b_count ? a_count : b_count;
    uint64_t carry = 0;
    size_t index = 0;
    for (; index < count; index++) {
        uint64_t sum = carry;
        if (index < a_count) sum += a[index];
        if (index < b_count) sum += b[index];
        result[index] = (uint32_t)(sum % DIAMOND_BIGNUM_BASE);
        carry = sum / DIAMOND_BIGNUM_BASE;
    }
    if (carry) result[index++] = (uint32_t)carry;
    return index;
}

/* Requires a >= b (as magnitudes). result must have room for a_count limbs. */
static size_t magnitude_subtract(const uint32_t *a, size_t a_count,
                                 const uint32_t *b, size_t b_count, uint32_t *result) {
    int64_t borrow = 0;
    for (size_t index = 0; index < a_count; index++) {
        int64_t diff = (int64_t)a[index] - borrow -
            (index < b_count ? (int64_t)b[index] : 0);
        if (diff < 0) { diff += DIAMOND_BIGNUM_BASE; borrow = 1; } else borrow = 0;
        result[index] = (uint32_t)diff;
    }
    return magnitude_trim(result, a_count);
}

/* result must have room for a_count+1 limbs. */
static size_t magnitude_multiply_scalar(const uint32_t *a, size_t a_count,
                                        uint32_t scalar, uint32_t *result) {
    uint64_t carry = 0;
    size_t index = 0;
    for (; index < a_count; index++) {
        const uint64_t product = (uint64_t)a[index] * scalar + carry;
        result[index] = (uint32_t)(product % DIAMOND_BIGNUM_BASE);
        carry = product / DIAMOND_BIGNUM_BASE;
    }
    if (carry) result[index++] = (uint32_t)carry;
    return magnitude_trim(result, index);
}

/* result must have room for a_count+b_count limbs. */
static size_t magnitude_multiply(const uint32_t *a, size_t a_count,
                                 const uint32_t *b, size_t b_count, uint32_t *result) {
    for (size_t index = 0; index < a_count + b_count; index++) result[index] = 0;
    for (size_t i = 0; i < a_count; i++) {
        uint64_t carry = 0;
        size_t j = 0;
        for (; j < b_count; j++) {
            const uint64_t product =
                (uint64_t)a[i] * b[j] + result[i + j] + carry;
            result[i + j] = (uint32_t)(product % DIAMOND_BIGNUM_BASE);
            carry = product / DIAMOND_BIGNUM_BASE;
        }
        size_t k = i + j;
        while (carry) {
            const uint64_t sum = result[k] + carry;
            result[k] = (uint32_t)(sum % DIAMOND_BIGNUM_BASE);
            carry = sum / DIAMOND_BIGNUM_BASE;
            k++;
        }
    }
    return magnitude_trim(result, a_count + b_count);
}

/* Long division, most-significant limb first, finding each quotient
 * digit via binary search (0..BASE-1) rather than the classic
 * next-limb-estimate trick -- simpler to get right, and division isn't
 * a hot path for a research language's bignum support. quotient needs
 * room for a_count limbs; remainder needs room for b_count+1 limbs
 * (the +1 is transient headroom during the per-digit shift-in step,
 * trimmed away before returning). Returns false only on allocation
 * failure (the scratch buffer for trial products). */
static bool magnitude_divide(const uint32_t *a, size_t a_count,
                             const uint32_t *b, size_t b_count,
                             uint32_t *quotient, size_t *quotient_count,
                             uint32_t *remainder, size_t *remainder_count) {
    uint32_t *scratch = malloc((b_count + 1) * sizeof(uint32_t));
    if (scratch == nullptr) return false;
    size_t rem_count = 0;
    for (size_t i = a_count; i > 0; i--) {
        for (size_t k = rem_count; k > 0; k--) remainder[k] = remainder[k - 1];
        remainder[0] = a[i - 1];
        rem_count = magnitude_trim(remainder, rem_count + 1);

        uint32_t low = 0, high = DIAMOND_BIGNUM_BASE - 1, digit = 0;
        while (low <= high) {
            const uint32_t mid = low + (high - low) / 2;
            const size_t scratch_count =
                magnitude_multiply_scalar(b, b_count, mid, scratch);
            if (magnitude_compare(scratch, scratch_count, remainder, rem_count) <= 0) {
                digit = mid;
                if (mid == DIAMOND_BIGNUM_BASE - 1) break;
                low = mid + 1;
            } else {
                if (mid == 0) break;
                high = mid - 1;
            }
        }
        quotient[i - 1] = digit;
        const size_t product_count =
            magnitude_multiply_scalar(b, b_count, digit, scratch);
        rem_count = magnitude_subtract(remainder, rem_count, scratch,
                                       product_count, remainder);
    }
    free(scratch);
    *quotient_count = magnitude_trim(quotient, a_count);
    *remainder_count = rem_count;
    return true;
}

static size_t int64_to_magnitude(int64_t value, uint32_t limbs[3]) {
    /* INT64_MIN's magnitude doesn't fit in int64_t itself (its true
     * magnitude is 2^63, one past INT64_MAX) -- accumulate via uint64_t
     * throughout so the INT64_MIN case is handled the same as any
     * other value, no special-casing needed. */
    uint64_t magnitude = value < 0 ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
    size_t count = 0;
    while (magnitude > 0) {
        limbs[count++] = (uint32_t)(magnitude % DIAMOND_BIGNUM_BASE);
        magnitude /= DIAMOND_BIGNUM_BASE;
    }
    return count;
}

/* Returns true and sets *out if the magnitude fits in int64_t. */
static bool magnitude_to_int64(const uint32_t *limbs, size_t count,
                               bool negative, int64_t *out) {
    if (count > 3) return false;
    uint64_t magnitude = 0;
    for (size_t i = count; i > 0; i--) {
        if (magnitude > (UINT64_MAX - limbs[i - 1]) / DIAMOND_BIGNUM_BASE) return false;
        magnitude = magnitude * DIAMOND_BIGNUM_BASE + limbs[i - 1];
    }
    if (negative) {
        if (magnitude > (uint64_t)INT64_MAX + 1) return false;
        *out = magnitude == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)magnitude;
    } else {
        if (magnitude > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)magnitude;
    }
    return true;
}

static DiamondBignum *bignum_alloc(DiamondVm *vm, bool negative,
                                   const uint32_t *limbs, size_t limb_count) {
    if (!maybe_collect(vm)) return nullptr;
    DiamondBignum *bignum =
        malloc(sizeof(DiamondBignum) + limb_count * sizeof(uint32_t));
    if (bignum == nullptr) return nullptr;
    bignum->object = (DiamondObject){.next = vm->young_objects, .kind = DIAMOND_OBJECT_BIGNUM};
    bignum->negative = negative;
    bignum->limb_count = limb_count;
    memcpy(bignum->limbs, limbs, limb_count * sizeof(uint32_t));
    vm->young_objects = &bignum->object;
    vm->bytes_allocated += sizeof(DiamondBignum) + limb_count * sizeof(uint32_t);
    return bignum;
}

/* Canonicalize a computed (sign, magnitude) pair into a DiamondValue:
 * a plain Int if it fits int64_t, else a freshly-allocated bignum. This
 * is the *only* allocation site any public function below ever reaches
 * (operand widening via diamond_int_view never allocates), so there's
 * never a second allocation that could sweep away an unrooted first
 * result -- see bignum.h's DiamondIntView doc comment. */
static DiamondValue bignum_canonicalize(DiamondVm *vm, bool negative,
                                 const uint32_t *limbs, size_t limb_count) {
    int64_t small = 0;
    if (limb_count == 0) return DIAMOND_INT(0);
    if (magnitude_to_int64(limbs, limb_count, negative, &small)) {
        return DIAMOND_INT(small);
    }
    DiamondBignum *bignum = bignum_alloc(vm, negative, limbs, limb_count);
    if (bignum == nullptr) return DIAMOND_NIL;
    return DIAMOND_OBJECT(bignum);
}

void diamond_int_view_int64(int64_t value, DiamondIntView *out) {
    out->negative = value < 0;
    out->limb_count = int64_to_magnitude(value, out->small_limbs);
    out->limbs = out->small_limbs;
}

void diamond_int_view(DiamondValue value, DiamondIntView *out) {
    if (value.kind == DIAMOND_VALUE_OBJECT &&
        value.as.object->kind == DIAMOND_OBJECT_BIGNUM) {
        const DiamondBignum *bignum = (const DiamondBignum *)value.as.object;
        out->negative = bignum->negative;
        out->limbs = bignum->limbs;
        out->limb_count = bignum->limb_count;
        return;
    }
    diamond_int_view_int64(value.as.integer, out);
}

DiamondValue diamond_bignum_add(DiamondVm *vm, DiamondIntView left,
                                DiamondIntView right) {
    const size_t capacity =
        (left.limb_count > right.limb_count ? left.limb_count
                                            : right.limb_count) + 1;
    uint32_t *result = malloc(capacity * sizeof(uint32_t));
    if (result == nullptr) return DIAMOND_NIL;
    DiamondValue value;
    if (left.negative == right.negative) {
        const size_t count = magnitude_add(left.limbs, left.limb_count,
            right.limbs, right.limb_count, result);
        value = bignum_canonicalize(vm, left.negative, result, count);
    } else {
        const int comparison = magnitude_compare(left.limbs, left.limb_count,
            right.limbs, right.limb_count);
        if (comparison == 0) {
            value = DIAMOND_INT(0);
        } else if (comparison > 0) {
            const size_t count = magnitude_subtract(left.limbs, left.limb_count,
                right.limbs, right.limb_count, result);
            value = bignum_canonicalize(vm, left.negative, result, count);
        } else {
            const size_t count = magnitude_subtract(right.limbs, right.limb_count,
                left.limbs, left.limb_count, result);
            value = bignum_canonicalize(vm, right.negative, result, count);
        }
    }
    free(result);
    return value;
}

DiamondValue diamond_bignum_subtract(DiamondVm *vm, DiamondIntView left,
                                     DiamondIntView right) {
    right.negative = !right.negative;
    return diamond_bignum_add(vm, left, right);
}

DiamondValue diamond_bignum_negate(DiamondVm *vm, DiamondIntView value) {
    return bignum_canonicalize(vm, !value.negative, value.limbs, value.limb_count);
}

DiamondValue diamond_bignum_multiply(DiamondVm *vm, DiamondIntView left,
                                     DiamondIntView right) {
    const size_t capacity = left.limb_count + right.limb_count;
    uint32_t *result = malloc((capacity == 0 ? 1 : capacity) * sizeof(uint32_t));
    if (result == nullptr) return DIAMOND_NIL;
    const size_t count = magnitude_multiply(left.limbs, left.limb_count,
        right.limbs, right.limb_count, result);
    const DiamondValue value =
        bignum_canonicalize(vm, left.negative != right.negative, result, count);
    free(result);
    return value;
}

DiamondValue diamond_bignum_divide_truncated(DiamondVm *vm,
        DiamondIntView left, DiamondIntView right) {
    uint32_t *quotient = malloc((left.limb_count == 0 ? 1 : left.limb_count) *
                                sizeof(uint32_t));
    uint32_t *remainder = malloc((right.limb_count + 1) * sizeof(uint32_t));
    if (quotient == nullptr || remainder == nullptr) {
        free(quotient); free(remainder);
        return DIAMOND_NIL;
    }
    size_t quotient_count = 0, remainder_count = 0;
    const bool ok = magnitude_divide(left.limbs, left.limb_count,
        right.limbs, right.limb_count, quotient, &quotient_count,
        remainder, &remainder_count);
    DiamondValue value = DIAMOND_NIL;
    if (ok) {
        value = bignum_canonicalize(vm, left.negative != right.negative,
                             quotient, quotient_count);
    }
    free(quotient); free(remainder);
    return value;
}

DiamondValue diamond_bignum_modulo_floored(DiamondVm *vm,
        DiamondIntView left, DiamondIntView right) {
    uint32_t *quotient = malloc((left.limb_count == 0 ? 1 : left.limb_count) *
                                sizeof(uint32_t));
    uint32_t *remainder = malloc((right.limb_count + 1) * sizeof(uint32_t));
    uint32_t *complement = malloc((right.limb_count + 1) * sizeof(uint32_t));
    if (quotient == nullptr || remainder == nullptr || complement == nullptr) {
        free(quotient); free(remainder); free(complement);
        return DIAMOND_NIL;
    }
    size_t quotient_count = 0, remainder_count = 0;
    const bool ok = magnitude_divide(left.limbs, left.limb_count,
        right.limbs, right.limb_count, quotient, &quotient_count,
        remainder, &remainder_count);
    DiamondValue value = DIAMOND_NIL;
    if (ok) {
        remainder_count = magnitude_trim(remainder, remainder_count);
        if (remainder_count == 0) {
            value = DIAMOND_INT(0);
        } else if (left.negative == right.negative) {
            /* The truncated remainder already takes the divisor's sign. */
            value = bignum_canonicalize(vm, right.negative, remainder,
                                        remainder_count);
        } else {
            /* Opposite signs: floored modulo is |right| - |remainder|,
             * carrying the divisor's sign (-7 % 3 == 3 - 1 == 2). */
            const size_t count = magnitude_subtract(right.limbs,
                right.limb_count, remainder, remainder_count, complement);
            value = bignum_canonicalize(vm, right.negative, complement, count);
        }
    }
    free(quotient); free(remainder); free(complement);
    return value;
}

int diamond_bignum_compare(DiamondIntView left, DiamondIntView right) {
    if (left.negative != right.negative) return left.negative ? -1 : 1;
    const int magnitude_comparison = magnitude_compare(
        left.limbs, left.limb_count, right.limbs, right.limb_count);
    return left.negative ? -magnitude_comparison : magnitude_comparison;
}

double diamond_bignum_to_double(const DiamondBignum *value) {
    double result = 0.0;
    for (size_t i = value->limb_count; i > 0; i--) {
        result = result * (double)DIAMOND_BIGNUM_BASE + (double)value->limbs[i - 1];
    }
    return value->negative ? -result : result;
}

DiamondValue diamond_bignum_from_double(DiamondVm *vm, double value) {
    if (value == 0.0) return DIAMOND_INT(0);
    const bool negative = value < 0.0;
    int exponent = 0;
    const double mantissa = frexp(fabs(value), &exponent);
    /* frexp gives 0.5 <= mantissa < 1.0; scaling by 2^53 turns it into
     * an exact 53-bit integer (a double's full mantissa precision),
     * with `exponent - 53` leftover powers of 2 still to apply. */
    const int64_t scaled_mantissa = (int64_t)ldexp(mantissa, 53);
    enum { MAX_LIMBS = 50 }; /* comfortably covers any finite double's ~309 decimal digits */
    uint32_t current[MAX_LIMBS];
    size_t count = int64_to_magnitude(scaled_mantissa, current);
    /* Only called (see vm.c's DIAMOND_OP_TO_INT) once the caller has
     * already confirmed the value doesn't fit int64_t, i.e. magnitude
     * >= 2^63, so exponent >= 63 and this shift is always positive in
     * practice -- repeated doubling rather than a smarter shift
     * algorithm, matching this library's cold-path simplicity-first
     * tradeoff (at most ~1000 iterations for the largest finite double). */
    uint32_t doubled[MAX_LIMBS];
    for (int shift = exponent - 53; shift > 0; shift--) {
        count = magnitude_add(current, count, current, count, doubled);
        memcpy(current, doubled, count * sizeof(uint32_t));
    }
    return bignum_canonicalize(vm, negative, current, count);
}

uint64_t diamond_bignum_hash(const DiamondBignum *value) {
    /* Same mixing shape as the rest of the VM's hash_mix64 (see
     * hash_value in vm.c) -- not exposed as a shared helper there, so
     * this is a self-contained equivalent rather than a new header
     * dependency for one function. */
    uint64_t hash = value->negative ? 0x9E3779B97F4A7C15ULL : 0xD6E8FEB86659FD93ULL;
    for (size_t i = 0; i < value->limb_count; i++) {
        hash ^= value->limbs[i];
        hash *= 0xFF51AFD7ED558CCDULL;
        hash ^= hash >> 33;
    }
    return hash;
}

size_t diamond_bignum_string_length(const DiamondBignum *value) {
    /* Sign (maybe) + up to 9 digits per limb + nul. */
    return (value->negative ? 1 : 0) + value->limb_count * 9 + 1;
}

size_t diamond_bignum_to_string(const DiamondBignum *value, char *out, size_t out_size) {
    (void)out_size;
    size_t position = 0;
    if (value->negative) out[position++] = '-';
    position += (size_t)snprintf(out + position, 21, "%u",
        value->limbs[value->limb_count - 1]);
    for (size_t i = value->limb_count - 1; i > 0; i--) {
        position += (size_t)snprintf(out + position, 10, "%09u", value->limbs[i - 1]);
    }
    return position;
}

DiamondValue diamond_bignum_from_decimal_digits(DiamondVm *vm, const char *digits,
        size_t length, bool negative) {
    const size_t limb_capacity = length / 9 + 1;
    uint32_t *limbs = malloc(limb_capacity * sizeof(uint32_t));
    if (limbs == nullptr) return DIAMOND_NIL;
    size_t limb_count = 0;
    /* Process the digit string in 9-digit chunks from the least
     * significant end, same shape as diamond_bignum_to_string's output
     * but in reverse. */
    size_t end = length;
    while (end > 0) {
        const size_t chunk_length = end >= 9 ? 9 : end;
        const size_t start = end - chunk_length;
        uint32_t chunk = 0;
        for (size_t i = start; i < end; i++) chunk = chunk * 10 + (uint32_t)(digits[i] - '0');
        limbs[limb_count++] = chunk;
        end = start;
    }
    const size_t trimmed = magnitude_trim(limbs, limb_count);
    const DiamondValue value = bignum_canonicalize(vm, negative, limbs, trimmed);
    free(limbs);
    return value;
}

/* --- Bitwise operators, shifts and radix conversion ---
 *
 * Limbs are base 10^9 (see above), so bit-level work goes through a base-2^32
 * form: the magnitude is converted to little-endian 32-bit words, operated on,
 * and converted back. Negative values follow Ruby's infinite two's-complement
 * semantics (-1 & x == x, ~0 sign-extends), which is what a fixed-width
 * hash or checksum written against a bignum expects. All scratch buffers are
 * malloc'd; the only GC allocation is the final result, as everywhere above. */

/* Divides a base-10^9 magnitude (modified in place) by 2^32 and returns the
 * remainder. */
static uint32_t magnitude_divide_word(uint32_t *limbs, size_t *count) {
    uint64_t remainder = 0;
    for (size_t i = *count; i > 0; i--) {
        const uint64_t current = remainder * DIAMOND_BIGNUM_BASE + limbs[i - 1];
        limbs[i - 1] = (uint32_t)(current >> 32);
        remainder = current & 0xFFFFFFFFull;
    }
    *count = magnitude_trim(limbs, *count);
    return (uint32_t)remainder;
}

/* Converts to base-2^32 words. Returns the word count (0 for zero), or
 * SIZE_MAX on allocation failure; *out is malloc'd with room for one extra
 * word that callers use for the sign. */
static size_t magnitude_to_words(const uint32_t *limbs, size_t count, uint32_t **out) {
    /* Each base-10^9 limb is < 2^30, so 30 bits per limb bounds the length. */
    const size_t capacity = (count * 30 + 31) / 32 + 2;
    uint32_t *words = calloc(capacity, sizeof(uint32_t));
    uint32_t *scratch = malloc((count == 0 ? 1 : count) * sizeof(uint32_t));
    if (words == nullptr || scratch == nullptr) {
        free(words);
        free(scratch);
        return SIZE_MAX;
    }
    memcpy(scratch, limbs, count * sizeof(uint32_t));
    size_t remaining = magnitude_trim(scratch, count);
    size_t produced = 0;
    while (remaining > 0) words[produced++] = magnitude_divide_word(scratch, &remaining);
    free(scratch);
    *out = words;
    return produced;
}

/* Replaces `limbs` (base 10^9, capacity at least count + 2) with
 * limbs * scalar + addend, returning the new count. */
static size_t magnitude_multiply_add(uint32_t *limbs, size_t count,
                                     uint32_t scalar, uint32_t addend) {
    uint64_t carry = addend;
    for (size_t i = 0; i < count; i++) {
        const uint64_t product = (uint64_t)limbs[i] * scalar + carry;
        limbs[i] = (uint32_t)(product % DIAMOND_BIGNUM_BASE);
        carry = product / DIAMOND_BIGNUM_BASE;
    }
    while (carry > 0) {
        limbs[count++] = (uint32_t)(carry % DIAMOND_BIGNUM_BASE);
        carry /= DIAMOND_BIGNUM_BASE;
    }
    return count;
}

/* Canonical value from a magnitude held as base-2^32 words. */
static DiamondValue words_to_value(DiamondVm *vm, bool negative,
                                   const uint32_t *words, size_t word_count) {
    while (word_count > 0 && words[word_count - 1] == 0) word_count--;
    if (word_count == 0) return DIAMOND_INT(0);
    /* 2^32 is just above 4.29 * 10^9: at most 2 limbs per word, plus slack. */
    uint32_t *limbs = calloc(word_count * 2 + 3, sizeof(uint32_t));
    if (limbs == nullptr) return DIAMOND_NIL;
    size_t count = 0;
    for (size_t i = word_count; i > 0; i--) {
        /* limbs * 2^32 + word, as two multiplies by 2^16 then an add (the
         * scalar must stay below 2^32). */
        count = magnitude_multiply_add(limbs, count, 65536u, 0);
        count = magnitude_multiply_add(limbs, count, 65536u, 0);
        uint64_t carry = words[i - 1];
        size_t index = 0;
        while (carry > 0) {
            if (index == count) limbs[count++] = 0;
            const uint64_t sum = (uint64_t)limbs[index] + carry;
            limbs[index] = (uint32_t)(sum % DIAMOND_BIGNUM_BASE);
            carry = sum / DIAMOND_BIGNUM_BASE;
            index++;
        }
    }
    const DiamondValue value = bignum_canonicalize(vm, negative, limbs, magnitude_trim(limbs, count));
    free(limbs);
    return value;
}

/* Two's-complement form of a signed value in exactly `width` words. */
static bool value_to_twos_complement(DiamondIntView view, size_t width, uint32_t *out) {
    uint32_t *words = nullptr;
    const size_t used = magnitude_to_words(view.limbs, view.limb_count, &words);
    if (used == SIZE_MAX) return false;
    memset(out, 0, width * sizeof(uint32_t));
    memcpy(out, words, (used < width ? used : width) * sizeof(uint32_t));
    free(words);
    if (view.negative) {
        uint64_t carry = 1;
        for (size_t i = 0; i < width; i++) {
            const uint64_t sum = (uint64_t)(uint32_t)~out[i] + carry;
            out[i] = (uint32_t)sum;
            carry = sum >> 32;
        }
    }
    return true;
}

static size_t view_word_count(DiamondIntView view) {
    /* 30 bits per limb, rounded up, plus one word for the sign. */
    return (view.limb_count * 30 + 31) / 32 + 2;
}

DiamondValue diamond_bignum_bitwise(DiamondVm *vm, DiamondBignumBitwiseOp op,
                                    DiamondIntView left, DiamondIntView right) {
    const size_t a_width = view_word_count(left), b_width = view_word_count(right);
    const size_t width = a_width > b_width ? a_width : b_width;
    uint32_t *a = malloc(width * sizeof(uint32_t));
    uint32_t *b = malloc(width * sizeof(uint32_t));
    if (a == nullptr || b == nullptr ||
        !value_to_twos_complement(left, width, a) ||
        !value_to_twos_complement(right, width, b)) {
        free(a);
        free(b);
        return DIAMOND_NIL;
    }
    for (size_t i = 0; i < width; i++) {
        a[i] = op == DIAMOND_BIGNUM_BITWISE_AND ? (a[i] & b[i])
             : op == DIAMOND_BIGNUM_BITWISE_OR ? (a[i] | b[i]) : (a[i] ^ b[i]);
    }
    free(b);
    const bool negative = (a[width - 1] & 0x80000000u) != 0;
    if (negative) {
        uint64_t carry = 1;
        for (size_t i = 0; i < width; i++) {
            const uint64_t sum = (uint64_t)(uint32_t)~a[i] + carry;
            a[i] = (uint32_t)sum;
            carry = sum >> 32;
        }
    }
    const DiamondValue value = words_to_value(vm, negative, a, width);
    free(a);
    return value;
}

DiamondValue diamond_bignum_shift_left(DiamondVm *vm, DiamondIntView value, uint64_t bits) {
    uint32_t *words = nullptr;
    const size_t used = magnitude_to_words(value.limbs, value.limb_count, &words);
    if (used == SIZE_MAX) return DIAMOND_NIL;
    if (used == 0) {
        free(words);
        return DIAMOND_INT(0);
    }
    const size_t whole = (size_t)(bits / 32);
    const unsigned part = (unsigned)(bits % 32);
    const size_t width = used + whole + 2;
    uint32_t *shifted = calloc(width, sizeof(uint32_t));
    if (shifted == nullptr) {
        free(words);
        return DIAMOND_NIL;
    }
    for (size_t i = 0; i < used; i++) {
        const uint64_t spread = (uint64_t)words[i] << part;
        shifted[i + whole] |= (uint32_t)spread;
        shifted[i + whole + 1] |= (uint32_t)(spread >> 32);
    }
    free(words);
    const DiamondValue result = words_to_value(vm, value.negative, shifted, width);
    free(shifted);
    return result;
}

DiamondValue diamond_bignum_shift_right(DiamondVm *vm, DiamondIntView value, uint64_t bits) {
    uint32_t *words = nullptr;
    const size_t used = magnitude_to_words(value.limbs, value.limb_count, &words);
    if (used == SIZE_MAX) return DIAMOND_NIL;
    if (used == 0) {
        free(words);
        return DIAMOND_INT(0);
    }
    const size_t whole = bits / 32 > (uint64_t)used ? used : (size_t)(bits / 32);
    const unsigned part = (unsigned)(bits % 32);
    /* Floor semantics: a negative value rounds toward minus infinity, so it
     * gains one when any shifted-out bit was set. */
    bool lost = false;
    for (size_t i = 0; i < whole && i < used; i++) lost |= words[i] != 0;
    if (whole < used && part != 0) lost |= (words[whole] & ((1u << part) - 1u)) != 0;
    size_t count = 0;
    uint32_t *result = calloc(used + 1, sizeof(uint32_t));
    if (result == nullptr) {
        free(words);
        return DIAMOND_NIL;
    }
    for (size_t i = whole; i < used; i++) {
        uint64_t current = words[i] >> part;
        if (part != 0 && i + 1 < used) current |= ((uint64_t)words[i + 1] << (32 - part)) & 0xFFFFFFFFull;
        result[count++] = (uint32_t)current;
    }
    free(words);
    if (value.negative && lost) {
        size_t index = 0;
        for (; index < count; index++) {
            if (++result[index] != 0) break;
        }
        if (index == count) result[count++] = 1;
    }
    const DiamondValue out = words_to_value(vm, value.negative, result, count);
    free(result);
    return out;
}

char *diamond_bignum_to_radix_string(DiamondIntView value, int radix, size_t *length) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    /* Every limb is below 2^30, so no more than 30 digits per limb (radix 2). */
    const size_t capacity = value.limb_count * 30 + 3;
    char *reversed = malloc(capacity);
    uint32_t *scratch = malloc((value.limb_count == 0 ? 1 : value.limb_count) * sizeof(uint32_t));
    if (reversed == nullptr || scratch == nullptr) {
        free(reversed);
        free(scratch);
        return nullptr;
    }
    memcpy(scratch, value.limbs, value.limb_count * sizeof(uint32_t));
    size_t remaining = magnitude_trim(scratch, value.limb_count);
    size_t produced = 0;
    while (remaining > 0) {
        uint64_t carry = 0;
        for (size_t i = remaining; i > 0; i--) {
            const uint64_t current = carry * DIAMOND_BIGNUM_BASE + scratch[i - 1];
            scratch[i - 1] = (uint32_t)(current / (uint32_t)radix);
            carry = current % (uint32_t)radix;
        }
        remaining = magnitude_trim(scratch, remaining);
        reversed[produced++] = digits[carry];
    }
    free(scratch);
    if (produced == 0) reversed[produced++] = '0';
    if (value.negative) reversed[produced++] = '-';
    for (size_t i = 0; i < produced / 2; i++) {
        const char swap = reversed[i];
        reversed[i] = reversed[produced - 1 - i];
        reversed[produced - 1 - i] = swap;
    }
    reversed[produced] = '\0';
    *length = produced;
    return reversed;
}

DiamondValue diamond_bignum_from_radix_digits(DiamondVm *vm, const char *digits,
        size_t length, int radix, bool negative) {
    /* A digit is at most 6 bits for radix 36, a limb holds 29.9 bits. */
    const size_t capacity = length * 6 / 29 + 3;
    uint32_t *limbs = calloc(capacity, sizeof(uint32_t));
    if (limbs == nullptr) return DIAMOND_NIL;
    size_t count = 0;
    for (size_t i = 0; i < length; i++) {
        const char c = digits[i];
        const uint32_t digit = c >= '0' && c <= '9' ? (uint32_t)(c - '0')
            : c >= 'a' && c <= 'z' ? (uint32_t)(c - 'a' + 10) : (uint32_t)(c - 'A' + 10);
        count = magnitude_multiply_add(limbs, count, (uint32_t)radix, digit);
    }
    const DiamondValue value = bignum_canonicalize(vm, negative, limbs, magnitude_trim(limbs, count));
    free(limbs);
    return value;
}
