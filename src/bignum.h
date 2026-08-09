#ifndef DIAMOND_BIGNUM_H
#define DIAMOND_BIGNUM_H

#include "vm.h"

/* Arbitrary-precision Int support: a DiamondBignum only ever exists to
 * represent a value that doesn't fit in int64_t (see object.h) -- every
 * function below that produces a new value canonicalizes the result,
 * returning a plain DIAMOND_INT(...) instead of a DiamondBignum whenever
 * the true result fits back in int64_t range. Callers never need to
 * check this themselves.
 *
 * DiamondIntView lets every arithmetic function below take either
 * representation an Int can have -- a small int64_t or an existing
 * DiamondBignum -- without ever needing to heap-allocate just to
 * *widen* a small operand first. That matters for GC safety, not just
 * convenience: combining two operands where widening the first
 * allocates a temporary bignum, unrooted anywhere a GC root scan would
 * find it, and widening the second *also* allocates and can trigger a
 * collection, would sweep the first temporary out from under the
 * second call. Building a view is always a stack-local, non-allocating
 * operation, so every function here does at most one allocation total
 * (the final result, only if it doesn't fit int64_t) -- never two in a
 * row, so this class of bug can't arise here at all.
 *
 * Built via an out-parameter, not returned by value: a DiamondIntView
 * backed by its own small_limbs is self-referential (.limbs points at
 * small_limbs, a member of the very same struct), and a struct return
 * copies the fields but does not relocate that pointer -- the caller's
 * copy's .limbs would keep pointing at the callee's already-dead stack
 * slot. Writing directly into the caller's own storage sidesteps the
 * copy (and the dangling pointer) entirely. */
typedef struct DiamondIntView {
    bool negative;
    const uint32_t *limbs;
    size_t limb_count;
    uint32_t small_limbs[3]; /* backing storage when limbs points here */
} DiamondIntView;

/* `value` must satisfy is_int_value (kind==DIAMOND_VALUE_INT, or a
 * bignum object) -- never allocates. */
void diamond_int_view(DiamondValue value, DiamondIntView *out);
void diamond_int_view_int64(int64_t value, DiamondIntView *out);

DiamondValue diamond_bignum_add(DiamondVm *vm, DiamondIntView left,
                                DiamondIntView right);
DiamondValue diamond_bignum_subtract(DiamondVm *vm, DiamondIntView left,
                                     DiamondIntView right);
DiamondValue diamond_bignum_multiply(DiamondVm *vm, DiamondIntView left,
                                     DiamondIntView right);
/* Truncates toward zero, matching Int division's existing convention.
 * `right` must not be zero -- callers check DIAMOND_VM_DIVISION_BY_ZERO
 * themselves first, same as the existing int64 division path. */
DiamondValue diamond_bignum_divide_truncated(DiamondVm *vm,
    DiamondIntView left, DiamondIntView right);
DiamondValue diamond_bignum_negate(DiamondVm *vm, DiamondIntView value);

/* Sign-then-magnitude comparison: <0, 0, or >0. */
int diamond_bignum_compare(DiamondIntView left, DiamondIntView right);

double diamond_bignum_to_double(const DiamondBignum *value);
uint64_t diamond_bignum_hash(const DiamondBignum *value);

/* Truncating double -> Int/bignum conversion, for to_i on a Float
 * outside int64_t range (see DIAMOND_OP_TO_INT in vm.c). Precondition:
 * `value` is finite. Any finite double already has no fractional part
 * once its magnitude is large enough to need a bignum at all (IEEE-754
 * doubles run out of fractional precision well before 2^63), so this
 * never needs to separately truncate a fraction -- it's exact. */
DiamondValue diamond_bignum_from_double(DiamondVm *vm, double value);

/* Decimal string conversion. diamond_bignum_to_string writes into `out`
 * (an already-sized buffer, see diamond_bignum_string_length for how
 * large it needs to be) and returns the digit count written (excluding
 * the nul terminator it also writes). diamond_bignum_string_length
 * returns a safe upper bound so callers can size a buffer before
 * calling diamond_bignum_to_string. */
size_t diamond_bignum_string_length(const DiamondBignum *value);
size_t diamond_bignum_to_string(const DiamondBignum *value, char *out, size_t out_size);

/* Parses a run of ASCII decimal digits (no sign, no separators -- the
 * caller has already stripped/validated those, matching how
 * String#to_i's existing digit-accumulation loop works) into a
 * canonicalized Int/bignum value. */
DiamondValue diamond_bignum_from_decimal_digits(DiamondVm *vm, const char *digits,
    size_t length, bool negative);

#endif
