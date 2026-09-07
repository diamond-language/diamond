#include "semver.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int count = 0;

static void check(bool condition, const char *description) {
    if (!condition) {
        fprintf(stderr, "semver_test: FAILED: %s\n", description);
        exit(1);
    }
    count++;
}

static Semver must_parse(const char *text) {
    Semver v;
    if (!semver_parse(text, &v)) {
        fprintf(stderr, "semver_test: expected \"%s\" to parse\n", text);
        exit(1);
    }
    return v;
}

static SemverConstraint must_parse_constraint(const char *text) {
    SemverConstraint c;
    if (!semver_constraint_parse(text, &c)) {
        fprintf(stderr, "semver_test: expected constraint \"%s\" to parse\n", text);
        exit(1);
    }
    return c;
}

int main(void) {
    /* --- parsing: accepted forms --- */
    {
        Semver v = must_parse("1.2.3");
        check(v.major == 1 && v.minor == 2 && v.patch == 3, "1.2.3 fields");
        check(v.prerelease[0] == '\0' && v.build[0] == '\0', "1.2.3 has no prerelease/build");
    }
    {
        Semver v = must_parse("v1.2.3");
        check(v.major == 1 && v.minor == 2 && v.patch == 3, "v1.2.3 fields (leading v stripped)");
    }
    {
        Semver v = must_parse("V2.0.0");
        check(v.major == 2, "V2.0.0 (capital V) fields");
    }
    {
        Semver v = must_parse("0.0.0");
        check(v.major == 0 && v.minor == 0 && v.patch == 0, "0.0.0 fields (bare zero is not a leading zero)");
    }
    {
        Semver v = must_parse("1.2.3-alpha.1");
        check(strcmp(v.prerelease, "alpha.1") == 0, "1.2.3-alpha.1 prerelease");
        check(v.build[0] == '\0', "1.2.3-alpha.1 has no build");
    }
    {
        Semver v = must_parse("1.2.3+build.5");
        check(v.prerelease[0] == '\0', "1.2.3+build.5 has no prerelease");
        check(strcmp(v.build, "build.5") == 0, "1.2.3+build.5 build");
    }
    {
        Semver v = must_parse("1.2.3-rc.1+build.5");
        check(strcmp(v.prerelease, "rc.1") == 0, "1.2.3-rc.1+build.5 prerelease");
        check(strcmp(v.build, "build.5") == 0, "1.2.3-rc.1+build.5 build");
    }

    /* --- parsing: rejected forms --- */
    {
        Semver v;
        check(!semver_parse("", &v), "empty string rejected");
        check(!semver_parse("1.2", &v), "1.2 (missing patch) rejected");
        check(!semver_parse("1.2.3.4", &v), "1.2.3.4 (extra field) rejected");
        check(!semver_parse("01.2.3", &v), "01.2.3 (leading zero major) rejected");
        check(!semver_parse("1.02.3", &v), "1.02.3 (leading zero minor) rejected");
        check(!semver_parse("1.2.03", &v), "1.2.03 (leading zero patch) rejected");
        check(!semver_parse("1.2.3-", &v), "1.2.3- (empty prerelease) rejected");
        check(!semver_parse("1.2.3+", &v), "1.2.3+ (empty build) rejected");
        check(!semver_parse("1.2.3-alpha..beta", &v), "1.2.3-alpha..beta (empty identifier) rejected");
        check(!semver_parse("1.2.3-01", &v), "1.2.3-01 (leading zero numeric prerelease id) rejected");
        check(!semver_parse("1.2.3-alpha_beta", &v), "1.2.3-alpha_beta (invalid char) rejected");
        check(!semver_parse("abc", &v), "abc rejected");
        check(!semver_parse("1.2.3 ", &v), "trailing space rejected");
    }

    /* --- format round-trips --- */
    {
        char buffer[64];
        check(semver_format(&(Semver){.major = 1, .minor = 2, .patch = 3}, buffer, sizeof buffer) &&
                strcmp(buffer, "1.2.3") == 0, "format 1.2.3");
        Semver v = must_parse("1.2.3-rc.1+build.5");
        check(semver_format(&v, buffer, sizeof buffer) && strcmp(buffer, "1.2.3-rc.1+build.5") == 0,
            "format round-trip 1.2.3-rc.1+build.5");
        Semver stripped_v = must_parse("V1.2.3");
        check(semver_format(&stripped_v, buffer, sizeof buffer) && strcmp(buffer, "1.2.3") == 0,
            "format drops the leading v/V");
    }

    /* --- precedence comparison (semver.org section 11 examples) --- */
    {
        Semver a = must_parse("1.0.0"), b = must_parse("2.0.0");
        check(semver_compare(&a, &b) < 0, "1.0.0 < 2.0.0");
        check(semver_compare(&b, &a) > 0, "2.0.0 > 1.0.0");
    }
    {
        Semver a = must_parse("1.2.0"), b = must_parse("1.10.0");
        check(semver_compare(&a, &b) < 0, "1.2.0 < 1.10.0 (numeric, not lexical)");
    }
    {
        Semver a = must_parse("1.0.0-alpha"), b = must_parse("1.0.0");
        check(semver_compare(&a, &b) < 0, "1.0.0-alpha < 1.0.0");
    }
    {
        Semver a = must_parse("1.0.0-alpha"), b = must_parse("1.0.0-alpha.1");
        check(semver_compare(&a, &b) < 0, "1.0.0-alpha < 1.0.0-alpha.1 (fewer fields)");
    }
    {
        Semver a = must_parse("1.0.0-alpha.1"), b = must_parse("1.0.0-alpha.beta");
        check(semver_compare(&a, &b) < 0, "1.0.0-alpha.1 < 1.0.0-alpha.beta (numeric < alphanumeric)");
    }
    {
        Semver a = must_parse("1.0.0-alpha.beta"), b = must_parse("1.0.0-beta");
        check(semver_compare(&a, &b) < 0, "1.0.0-alpha.beta < 1.0.0-beta (lexical)");
    }
    {
        Semver a = must_parse("1.0.0-beta.2"), b = must_parse("1.0.0-beta.11");
        check(semver_compare(&a, &b) < 0, "1.0.0-beta.2 < 1.0.0-beta.11 (numeric identifier, not lexical)");
    }
    {
        Semver a = must_parse("1.0.0-rc.1"), b = must_parse("1.0.0");
        check(semver_compare(&a, &b) < 0, "1.0.0-rc.1 < 1.0.0");
    }
    {
        Semver a = must_parse("1.0.0+build1"), b = must_parse("1.0.0+build2");
        check(semver_compare(&a, &b) == 0, "build metadata never affects precedence");
    }

    /* --- range parsing + satisfies --- */
    {
        SemverConstraint c = must_parse_constraint("^1.2.3");
        check(semver_satisfies(&(Semver){.major = 1, .minor = 2, .patch = 3}, &c), "^1.2.3 satisfies 1.2.3");
        check(semver_satisfies(&(Semver){.major = 1, .minor = 9, .patch = 9}, &c), "^1.2.3 satisfies 1.9.9");
        check(!semver_satisfies(&(Semver){.major = 1, .minor = 2, .patch = 2}, &c), "^1.2.3 rejects 1.2.2");
        check(!semver_satisfies(&(Semver){.major = 2, .minor = 0, .patch = 0}, &c), "^1.2.3 rejects 2.0.0");
    }
    {
        SemverConstraint c = must_parse_constraint("^0.2.3");
        check(semver_satisfies(&(Semver){.major = 0, .minor = 2, .patch = 9}, &c), "^0.2.3 satisfies 0.2.9");
        check(!semver_satisfies(&(Semver){.major = 0, .minor = 3, .patch = 0}, &c), "^0.2.3 rejects 0.3.0");
    }
    {
        SemverConstraint c = must_parse_constraint("^0.0.3");
        check(semver_satisfies(&(Semver){.major = 0, .minor = 0, .patch = 3}, &c), "^0.0.3 satisfies 0.0.3");
        check(!semver_satisfies(&(Semver){.major = 0, .minor = 0, .patch = 4}, &c), "^0.0.3 rejects 0.0.4");
    }
    {
        SemverConstraint c = must_parse_constraint("~1.2.3");
        check(semver_satisfies(&(Semver){.major = 1, .minor = 2, .patch = 99}, &c), "~1.2.3 satisfies 1.2.99");
        check(!semver_satisfies(&(Semver){.major = 1, .minor = 2, .patch = 2}, &c), "~1.2.3 rejects 1.2.2");
        check(!semver_satisfies(&(Semver){.major = 1, .minor = 3, .patch = 0}, &c), "~1.2.3 rejects 1.3.0");
    }
    {
        SemverConstraint c = must_parse_constraint(">=1.0.0 <2.0.0");
        check(semver_satisfies(&(Semver){.major = 1, .minor = 0, .patch = 0}, &c), ">=1.0.0 <2.0.0 satisfies 1.0.0");
        check(semver_satisfies(&(Semver){.major = 1, .minor = 9, .patch = 9}, &c), ">=1.0.0 <2.0.0 satisfies 1.9.9");
        check(!semver_satisfies(&(Semver){.major = 2, .minor = 0, .patch = 0}, &c), ">=1.0.0 <2.0.0 rejects 2.0.0");
        check(!semver_satisfies(&(Semver){.major = 0, .minor = 9, .patch = 9}, &c), ">=1.0.0 <2.0.0 rejects 0.9.9");
    }
    {
        SemverConstraint c = must_parse_constraint("1.2.3");
        check(semver_satisfies(&(Semver){.major = 1, .minor = 2, .patch = 3}, &c), "exact 1.2.3 satisfies 1.2.3");
        check(!semver_satisfies(&(Semver){.major = 1, .minor = 2, .patch = 4}, &c), "exact 1.2.3 rejects 1.2.4");
    }
    {
        SemverConstraint c = must_parse_constraint(">=1.0.0 <=1.5.0");
        check(semver_satisfies(&(Semver){.major = 1, .minor = 5, .patch = 0}, &c), ">=1.0.0 <=1.5.0 satisfies 1.5.0 (inclusive)");
        check(!semver_satisfies(&(Semver){.major = 1, .minor = 5, .patch = 1}, &c), ">=1.0.0 <=1.5.0 rejects 1.5.1");
    }
    {
        /* A prerelease version never satisfies anything, even when its
         * own [major,minor,patch] is numerically in range. */
        SemverConstraint c = must_parse_constraint(">=1.0.0 <2.0.0");
        Semver prerelease = must_parse("1.5.0-beta");
        check(!semver_satisfies(&prerelease, &c), "1.5.0-beta never satisfies a constraint");
    }

    /* --- range parsing: rejected forms --- */
    {
        SemverConstraint c;
        check(!semver_constraint_parse("", &c), "empty constraint rejected");
        check(!semver_constraint_parse("   ", &c), "whitespace-only constraint rejected");
        check(!semver_constraint_parse("^1.2", &c), "^1.2 (partial version) rejected");
        check(!semver_constraint_parse(">=1.0.0 <2.0.0 <3.0.0", &c), "three terms / duplicate upper bound rejected");
        check(!semver_constraint_parse("=1.0.0 >=2.0.0", &c), "exact combined with another term rejected");
        check(!semver_constraint_parse(">=1.0.0 >=2.0.0", &c), "two lower bounds rejected");
        check(!semver_constraint_parse("<=1.0.0 <=2.0.0", &c), "two upper bounds rejected");
        check(!semver_constraint_parse("not-a-version", &c), "garbage constraint rejected");
    }

    /* --- intersection --- */
    {
        SemverConstraint a = must_parse_constraint("^1.2.0");
        SemverConstraint b = must_parse_constraint("^1.5.0");
        SemverConstraint result;
        check(semver_constraint_intersect(&a, &b, &result), "^1.2.0 and ^1.5.0 intersect");
        check(semver_satisfies(&(Semver){.major = 1, .minor = 5, .patch = 0}, &result),
            "^1.2.0 ^ ^1.5.0 satisfies 1.5.0");
        check(semver_satisfies(&(Semver){.major = 1, .minor = 9, .patch = 9}, &result),
            "^1.2.0 ^ ^1.5.0 satisfies 1.9.9");
        check(!semver_satisfies(&(Semver){.major = 1, .minor = 4, .patch = 0}, &result),
            "^1.2.0 ^ ^1.5.0 rejects 1.4.0");
        check(!semver_satisfies(&(Semver){.major = 2, .minor = 0, .patch = 0}, &result),
            "^1.2.0 ^ ^1.5.0 rejects 2.0.0");
    }
    {
        SemverConstraint a = must_parse_constraint(">=1.0.0");
        SemverConstraint b = must_parse_constraint("<=1.5.0");
        SemverConstraint result;
        check(semver_constraint_intersect(&a, &b, &result), ">=1.0.0 and <=1.5.0 intersect");
        check(semver_satisfies(&(Semver){.major = 1, .minor = 0, .patch = 0}, &result), "intersection satisfies 1.0.0");
        check(semver_satisfies(&(Semver){.major = 1, .minor = 5, .patch = 0}, &result), "intersection satisfies 1.5.0");
        check(!semver_satisfies(&(Semver){.major = 1, .minor = 5, .patch = 1}, &result), "intersection rejects 1.5.1");
        check(!semver_satisfies(&(Semver){.major = 0, .minor = 9, .patch = 9}, &result), "intersection rejects 0.9.9");
    }
    {
        /* ^1.x.x is [1.0.0, 2.0.0); ^2.x.x is [2.0.0, 3.0.0) -- disjoint. */
        SemverConstraint a = must_parse_constraint("^1.0.0");
        SemverConstraint b = must_parse_constraint("^2.0.0");
        SemverConstraint result;
        check(!semver_constraint_intersect(&a, &b, &result), "^1.0.0 and ^2.0.0 do not intersect");
    }
    {
        SemverConstraint a = must_parse_constraint("1.2.3");
        SemverConstraint b = must_parse_constraint("1.2.4");
        SemverConstraint result;
        check(!semver_constraint_intersect(&a, &b, &result), "two different exact constraints do not intersect");
    }

    /* --- constraint format --- */
    {
        char buffer[128];
        SemverConstraint c = must_parse_constraint(">=1.2.3 <2.0.0");
        check(semver_constraint_format(&c, buffer, sizeof buffer) && strcmp(buffer, ">=1.2.3 <2.0.0") == 0,
            "format >=1.2.3 <2.0.0");
        SemverConstraint exact = must_parse_constraint("1.2.3");
        check(semver_constraint_format(&exact, buffer, sizeof buffer) && strcmp(buffer, "1.2.3") == 0,
            "format exact 1.2.3");
        SemverConstraint unbounded = {0};
        check(semver_constraint_format(&unbounded, buffer, sizeof buffer) && strcmp(buffer, "*") == 0,
            "format unbounded constraint");
    }

    printf("%d semver tests passed\n", count);
    return 0;
}
