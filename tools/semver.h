/* Semantic Versioning 2.0.0 (semver.org) parsing, precedence comparison,
 * and range/constraint handling for `facet` -- see docs/roadmap.md's
 * "Real semver dependency resolution for facet". Deliberately standalone:
 * no dependency on facet.c, the Diamond compiler, or the VM, since this
 * is plain string/number parsing with no need for any of that (and
 * `docs/packages.md`'s own `diamond.cut` `version` field is a plain
 * String -- nothing here needs to run *inside* the language either).
 *
 * There is no hosted registry (see docs/roadmap.md): a dependency's
 * available versions come from its own repository's git tags, so the
 * only "database" this module ever consults is whatever list of tag
 * strings a caller already fetched via `git ls-remote --tags`.
 */
#ifndef DIAMOND_SEMVER_H
#define DIAMOND_SEMVER_H

#include <stdbool.h>
#include <stddef.h>

enum {
    /* Generous for real-world prerelease/build strings ("alpha.1",
     * "20260907.abcdef") without inviting a fixed-size overflow. */
    SEMVER_MAX_IDENTIFIERS = 256,
};

typedef struct Semver {
    unsigned long major, minor, patch;
    /* "" (empty) when absent -- a version with no prerelease has higher
     * precedence than the same major.minor.patch with one (semver.org
     * section 11). */
    char prerelease[SEMVER_MAX_IDENTIFIERS];
    /* "" when absent. Build metadata (semver.org section 10) never
     * affects precedence or satisfies/intersect -- carried only so
     * semver_format can round-trip a version that had one. */
    char build[SEMVER_MAX_IDENTIFIERS];
} Semver;

/* Parses `text` as a strict semver version, optionally prefixed with a
 * single leading 'v'/'V' (the conventional git tag style, "v1.2.3") --
 * needed since "tags that parse as semver" (docs/roadmap.md) is exactly
 * how a dependency's available versions get discovered, and real-world
 * tags are `v1.2.3` far more often than bare `1.2.3`. Strict otherwise:
 * major/minor/patch must be plain non-negative integers with no leading
 * zero (except the single digit "0" itself), matching semver.org's own
 * BNF grammar exactly -- a tag that doesn't parse is simply not a
 * version, not something to coerce. Returns false (leaving *out
 * unspecified) on any malformed input. */
bool semver_parse(const char *text, Semver *out);

/* Formats `v` back into `buffer` (NAME.MINOR.PATCH[-prerelease][+build]),
 * no leading 'v'. Returns false (buffer contents unspecified) only if
 * buffer_size is too small. */
bool semver_format(const Semver *v, char *buffer, size_t buffer_size);

/* Precedence comparison per semver.org section 11: numeric major/minor/
 * patch first, then prerelease identifiers (numeric identifiers compare
 * numerically and always precede alphanumeric ones; alphanumeric
 * identifiers compare byte-wise (ASCII); a version with more prerelease
 * fields has higher precedence than a prefix-equal one with fewer; no
 * prerelease at all outranks any prerelease). Build metadata is never
 * consulted. Returns -1, 0, or 1. */
int semver_compare(const Semver *a, const Semver *b);

/* A range: everything on [min, max] subject to min_inclusive/
 * max_inclusive, with either bound optionally absent (has_min/has_max
 * false meaning "unbounded on that side"). An *exact* constraint
 * ("1.2.3", satisfied only by that one version) is represented as
 * has_min && has_max with min == max and both inclusive -- not a
 * separate case, so every consumer (satisfies/intersect/format) has
 * exactly one shape to handle. */
typedef struct SemverConstraint {
    bool has_min;
    Semver min;
    bool min_inclusive;
    bool has_max;
    Semver max;
    bool max_inclusive;
} SemverConstraint;

/* Parses one of:
 *   - `^1.2.3`   -- caret: >=1.2.3, <(next version that would be a
 *                   breaking change under semver's own left-most-
 *                   non-zero-digit rule) -- ^1.2.3 allows up to <2.0.0,
 *                   ^0.2.3 allows up to <0.3.0, ^0.0.3 allows up to
 *                   <0.0.4.
 *   - `~1.2.3`   -- tilde: >=1.2.3, <1.3.0 (patch-level only).
 *   - one or two whitespace-separated comparator terms, each
 *     `(>=|<=|>|<|=)?X.Y.Z` (a bare `X.Y.Z` with no operator means
 *     `=X.Y.Z`) -- e.g. `>=1.0.0 <2.0.0`, or a lone `1.2.3` for an
 *     exact-only constraint.
 * `^`/`~` always take a single, fully-specified X.Y.Z (a partial
 * version like `~1.2` is deliberately not supported -- a real git tag
 * this resolves against is already fully specified by construction, so
 * there is no real-world case this would help). Returns false on any
 * unparseable or self-contradictory input (two lower bounds, an exact
 * combined with anything else, ...). */
bool semver_constraint_parse(const char *text, SemverConstraint *out);

/* True iff `version` lies within `constraint`. A prerelease version
 * never satisfies any constraint here (deliberately simpler than full
 * semver.org section 9's opt-in prerelease-matching rule -- Diamond
 * dependencies are expected to depend on release versions; revisit if
 * a real package ever needs to pin a prerelease). */
bool semver_satisfies(const Semver *version, const SemverConstraint *constraint);

/* Intersects `a` and `b` (the AND of both, needed whenever two
 * different requesters in a dependency graph constrain the same cut --
 * docs/roadmap.md's own resolver design) into `*out`. Returns false
 * when the intersection is empty (no version can satisfy both), in
 * which case `*out` is left unspecified -- the caller already has `a`
 * and `b` themselves to report in that case, so this doesn't attempt
 * to explain *why* they conflict. */
bool semver_constraint_intersect(const SemverConstraint *a, const SemverConstraint *b,
    SemverConstraint *out);

/* Formats `constraint` back into a human-readable range string for an
 * error message (">=1.2.3 <2.0.0", exactly "1.2.3" for an exact
 * constraint, "*" for an unconstrained/empty one). Returns false
 * (buffer contents unspecified) only if buffer_size is too small. */
bool semver_constraint_format(const SemverConstraint *constraint, char *buffer, size_t buffer_size);

#endif
