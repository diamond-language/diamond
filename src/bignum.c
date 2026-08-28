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
    maybe_collect(vm);
    DiamondBignum *bignum =
        malloc(sizeof(DiamondBignum) + limb_count * sizeof(uint32_t));
    if (bignum == nullptr) return nullptr;
    bignum->object = (DiamondObject){.next = vm->objects, .kind = DIAMOND_OBJECT_BIGNUM};
    bignum->negative = negative;
    bignum->limb_count = limb_count;
    memcpy(bignum->limbs, limbs, limb_count * sizeof(uint32_t));
    vm->objects = &bignum->object;
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
