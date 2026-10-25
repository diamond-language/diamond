#!/usr/bin/env bash
# Checks a `make MINIMAL=1` build (no SQLite, PostgreSQL, MySQL/MariaDB, zlib, TLS or
# OpenSSL): the features it dropped fail with a clear error, Digest/HMAC/Base64/SecureRandom
# still work through the portable code, and every case that does not use a dropped feature
# still produces its expected output. Build first: make clean && make MINIMAL=1 debug
set -uo pipefail
cd "$(dirname "$0")/.."
diamond="${DIAMOND_BIN:-build/diamond}"
# Cases assert through their final top-level value, which a script run
# prints only with DIAMOND_PRINT_RESULT set.
export DIAMOND_PRINT_RESULT=1

# 1. Each dropped feature raises an error that names it, instead of crashing or linking.
fail=0
expect_error() {
    local snippet="$1" fragment="$2" output
    output="$(timeout 20 "$diamond" -e "$snippet" 2>&1 </dev/null)"
    if [[ "$output" != *"$fragment"* ]]; then
        echo "FAIL: $snippet" >&2
        echo "  expected an error containing: $fragment" >&2
        echo "  got: $output" >&2
        fail=1
    fi
}
expect_error 'SQLite3.open(":memory:")' "SQLite3 support is not compiled into this build"
expect_error 'PostgreSQL.open("host=localhost")' "PostgreSQL support is not compiled into this build"
expect_error 'MySQL.open("localhost", "u", "p", "d", 3306)' "MySQL support is not compiled into this build"
expect_error 'Gzip.compress("hello")' "Gzip is not compiled into this build"
expect_error 'TLSServer.listen(8443, "cert.pem", "key.pem")' "TLS support is not compiled into this build"
expect_error 'Cipher.encrypt("01234567890123456789012345678901", "x")' "Cipher is not compiled into this build"

# The portable replacements for OpenSSL's digests must match the known answers.
expect_output() {
    local snippet="$1" want="$2" output
    output="$(timeout 20 "$diamond" -e "$snippet" 2>&1 </dev/null | head -1)"
    if [[ "$output" != "$want" ]]; then
        echo "FAIL: $snippet" >&2
        echo "  expected: $want" >&2
        echo "  got: $output" >&2
        fail=1
    fi
}
expect_output 'puts(Digest.sha256("abc"))' "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
expect_output 'puts(Digest.sha1("abc"))' "a9993e364706816aba3e25717850c26c9cd0d89d"
expect_output 'puts(HMAC.sha256("Jefe", "what do ya want for nothing?"))' "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
expect_output 'puts(Base64.encode("Man is distinguished"))' "TWFuIGlzIGRpc3Rpbmd1aXNoZWQ="
expect_output 'puts(Base64.decode("YQ=="))' "a"
expect_output 'puts(SecureRandom.hex(8).length())' "16"
[[ $fail -eq 0 ]] || exit 1

# 2. Everything that does not use a dropped feature behaves as it does in a full build.
uses_dropped='SQLite3|PostgreSQL|MySQL|Gzip|TLS|Cipher|ActiveRecord|active_record|Mjolnir|mjolnir|Arel|arel|require_cut|database_config|Repository|Jobs\b|jobs'
checked=0
for source in tests/cases/*.d6; do
    base="${source%.d6}"
    [[ -f "$base.expected" ]] || continue
    grep -Eq "$uses_dropped" "$source" && continue
    env_args=(); flag_args=(); line=
    if [[ -f "$base.env" ]]; then
        while IFS= read -r line; do [[ -n "$line" ]] && env_args+=("$line"); done < "$base.env"
    fi
    if [[ -f "$base.flags" ]]; then
        while IFS= read -r line; do [[ -n "$line" ]] && flag_args+=("$line"); done < "$base.flags"
    fi
    actual="$(timeout 60 env "${env_args[@]}" "$diamond" "${flag_args[@]}" "$source" 2>/dev/null </dev/null)"
    if [[ "$actual" != "$(cat "$base.expected")" ]]; then
        echo "FAIL: $source" >&2
        fail=1
    fi
    checked=$((checked + 1))
done
echo "minimal build: $checked cases checked"
exit $fail
