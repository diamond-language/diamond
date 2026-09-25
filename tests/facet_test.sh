#!/usr/bin/env bash
set -euo pipefail

# facet is tested against local git repositories only - git clone works
# identically against a local path as it does against a remote URL, so
# this never touches the network (matching how the HTTP tests avoid the
# real internet by running an in-process server instead).

diamond="$(realpath ./build/diamond)"
facet="$(realpath ./build/facet)"
source_root="$(pwd)"

export GIT_AUTHOR_NAME=facet-test GIT_AUTHOR_EMAIL=facet-test@example.com
export GIT_COMMITTER_NAME=facet-test GIT_COMMITTER_EMAIL=facet-test@example.com

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

commit_repo() {
    git init -q -b main "$1"
    (cd "$1" && git add -A && git commit -q -m "commit")
}

# --- basic single-dependency install ---

mkdir greeter_repo
cat > greeter_repo/greeter.di <<'EOF'
def greet(name)
  "hello, " + name
end
EOF
echo '{"name": "greeter", "version": "1.0.0"}' > greeter_repo/diamond.cut
commit_repo greeter_repo
(cd greeter_repo && git tag v1.0.0)
greeter_v1="$(cd greeter_repo && git rev-parse v1.0.0)"

mkdir project1
cat > project1/diamond.cut <<EOF
{"name": "myapp", "dependencies": {"greeter": {"git": "$work/greeter_repo", "tag": "v1.0.0"}}}
EOF
(cd project1 && "$facet" install >/dev/null)

[[ -f project1/cuts/greeter/greeter.di ]]
[[ ! -d project1/cuts/greeter/.git ]]
[[ -f project1/facet.lock ]]
grep -q '"source": "git"' project1/facet.lock

# Older Git locks without a source key remain readable. Unknown source
# types and fields, and mixed source fields, fail before installation.
cp project1/facet.lock project1/facet.lock.new
sed 's/"source": "git", //' project1/facet.lock.new > project1/facet.lock
(cd project1 && "$facet" install >/dev/null)
mv project1/facet.lock.new project1/facet.lock
cp project1/facet.lock project1/facet.lock.good
sed 's/"source": "git"/"source": "registry"/' project1/facet.lock.good > project1/facet.lock
if (cd project1 && "$facet" install) >/dev/null 2>&1; then
    echo "facet accepted mixed Git and registry lock fields" >&2
    exit 1
fi
sed 's/"source": "git"/"source": "git", "sha256": "unchecked"/' project1/facet.lock.good > project1/facet.lock
if (cd project1 && "$facet" install) >/dev/null 2>&1; then
    echo "facet accepted an unknown lockfile field" >&2
    exit 1
fi
mv project1/facet.lock.good project1/facet.lock
cp project1/facet.lock project1/facet.lock.git

# A locked registry install fetches only the digest-addressed artifact, verifies
# its locked size and digest, validates the archive, and replaces the cut from
# staging. A fake curl keeps the test local while exercising the argv contract.
mkdir -p registry_source/greeter/lib fake_bin
printf '%s\n' '{"name": "greeter", "version": "1.2.3", "summary": "Registry greeter", "license": "MIT", "maintainers": [{"name": "Test", "contact": "test@example.com"}]}' > registry_source/greeter/diamond.cut
printf '%s\n' '# Registry greeter' > registry_source/greeter/README.md
printf '%s\n' 'MIT' > registry_source/greeter/LICENSE
printf '%s\n' 'def greet(name)' '  "registry hello, " + name' 'end' > registry_source/greeter/lib/greeter.di
"$facet" pack registry_source/greeter registry_greeter.tar >/dev/null
digest="$(sha256sum registry_greeter.tar | cut -d' ' -f1)"
archive_size="$(stat -c %s registry_greeter.tar)"
cat > fake_bin/curl <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
output=
url=
while (($#)); do
    case "$1" in
        --output) output="$2"; shift 2 ;;
        --) url="$2"; break ;;
        *) shift ;;
    esac
done
printf '%s\n' "$url" > "$FACET_TEST_CURL_URL"
cp "$FACET_TEST_ARCHIVE" "$output"
EOF
chmod +x fake_bin/curl
export FACET_TEST_ARCHIVE="$work/registry_greeter.tar"
export FACET_TEST_CURL_URL="$work/registry_url"
saved_path="$PATH"
export PATH="$work/fake_bin:$PATH"
cat > project1/facet.lock <<EOF
{"greeter": {"source": "registry", "registry": "https://cuts.example/api", "version": "1.2.3", "sha256": "$digest", "size": $archive_size}}
EOF
(cd project1 && "$facet" install >/dev/null)
grep -q '^https://cuts.example/api/v1/blobs/sha256/'"$digest"'$' registry_url
actual="$(cd project1 && "$diamond" -e 'require_cut "greeter"
greet("world")')"
[[ "$actual" == "registry hello, world" ]]

# Publishing sends a verified archive and bearer credential to the configured
# cut endpoint. The fake client inspects curl's private config file locally.
mkdir -p publish_bin
cat > publish_bin/curl <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
config=
while (($#)); do
    if [[ "$1" == "--config" ]]; then config="$2"; shift 2; else shift; fi
done
grep -q 'Authorization: Bearer publish-token' "$config"
grep -q 'Content-Type: application/octet-stream' "$config"
printf '%s\n' published > "$FACET_TEST_PUBLISH_OK"
EOF
chmod +x publish_bin/curl
export FACET_TEST_PUBLISH_OK="$work/publish-ok"
export PATH="$work/publish_bin:$saved_path"
"$facet" publish registry_source/greeter --registry https://cuts.example/api \
    --token publish-token >/dev/null
grep -q '^published$' "$FACET_TEST_PUBLISH_OK"
export PATH="$work/fake_bin:$saved_path"
unset FACET_TEST_PUBLISH_OK

# Failed locked installs leave the previously verified cut in place.
bad_digest="$(printf '0%.0s' {1..64})"
cat > project1/facet.lock <<EOF
{"greeter": {"source": "registry", "registry": "https://cuts.example/api", "version": "1.2.3", "sha256": "$bad_digest", "size": $archive_size}}
EOF
if (cd project1 && "$facet" install) >/dev/null 2>&1; then
    echo "facet installed an archive with the wrong locked digest" >&2
    exit 1
fi
actual="$(cd project1 && "$diamond" -e 'require_cut "greeter"
greet("world")')"
[[ "$actual" == "registry hello, world" ]]

cat > project1/facet.lock <<EOF
{"greeter": {"source": "registry", "registry": "https://cuts.example/api", "version": "1.2.3", "sha256": "$digest", "size": $((archive_size + 1))}}
EOF
if (cd project1 && "$facet" install) >/dev/null 2>&1; then
    echo "facet installed an archive with the wrong locked size" >&2
    exit 1
fi
actual="$(cd project1 && "$diamond" -e 'require_cut "greeter"
greet("world")')"
[[ "$actual" == "registry hello, world" ]]

# A valid archive whose manifest identity disagrees with the lock is also
# rejected before the installed cut is replaced.
sed -i 's/"version": "1.2.3"/"version": "1.2.4"/' registry_source/greeter/diamond.cut
"$facet" pack registry_source/greeter registry_other.tar >/dev/null
other_digest="$(sha256sum registry_other.tar | cut -d' ' -f1)"
other_size="$(stat -c %s registry_other.tar)"
export FACET_TEST_ARCHIVE="$work/registry_other.tar"
cat > project1/facet.lock <<EOF
{"greeter": {"source": "registry", "registry": "https://cuts.example/api", "version": "1.2.3", "sha256": "$other_digest", "size": $other_size}}
EOF
if (cd project1 && "$facet" install) >/dev/null 2>&1; then
    echo "facet installed an archive with the wrong manifest identity" >&2
    exit 1
fi
actual="$(cd project1 && "$diamond" -e 'require_cut "greeter"
greet("world")')"
[[ "$actual" == "registry hello, world" ]]
export FACET_TEST_ARCHIVE="$work/registry_greeter.tar"

check_bad_registry_lock() {
    printf '%s\n' "$1" > project1/facet.lock
    if (cd project1 && "$facet" install) >/dev/null 2>registry_error; then
        echo "facet accepted invalid registry lock: $2" >&2
        exit 1
    fi
    if ! grep -q "invalid registry fields" registry_error; then
        echo "facet did not reject invalid registry fields: $2" >&2
        cat registry_error >&2
        exit 1
    fi
}
check_bad_registry_lock "{\"greeter\": {\"source\": \"registry\", \"registry\": \"http://cuts.example\", \"version\": \"1.2.3\", \"sha256\": \"$digest\", \"size\": 12345}}" "non-HTTPS URL"
check_bad_registry_lock "{\"greeter\": {\"source\": \"registry\", \"registry\": \"https://cuts.example\", \"version\": \"v1.2.3\", \"sha256\": \"$digest\", \"size\": 12345}}" "noncanonical version"
check_bad_registry_lock "{\"greeter\": {\"source\": \"registry\", \"registry\": \"https://cuts.example\", \"version\": \"1.2.3\", \"sha256\": \"ABC\", \"size\": 12345}}" "invalid digest"
check_bad_registry_lock "{\"greeter\": {\"source\": \"registry\", \"registry\": \"https://cuts.example\", \"version\": \"1.2.3\", \"sha256\": \"$digest\", \"size\": 0}}" "zero size"
check_bad_registry_lock "{\"greeter\": {\"source\": \"registry\", \"registry\": \"https://cuts.example\", \"version\": \"1.2.3\", \"sha256\": \"$digest\", \"size\": \"12345\"}}" "string size"
check_bad_registry_lock "{\"greeter\": {\"source\": \"registry\", \"registry\": \"https://cuts.example\", \"version\": \"1.2.3\", \"sha256\": \"$digest\", \"size\": 18446744073709551616}}" "overflowing size"
mv project1/facet.lock.git project1/facet.lock
export PATH="$saved_path"
unset FACET_TEST_ARCHIVE FACET_TEST_CURL_URL
(cd project1 && "$facet" install >/dev/null)

actual="$(cd project1 && "$diamond" -e 'require_cut "greeter"
greet("world")')"
[[ "$actual" == "hello, world" ]]

# --- facet install respects an existing lock; facet update re-resolves ---

cat > greeter_repo/greeter.di <<'EOF'
def greet(name)
  "hi, " + name
end
EOF
(cd greeter_repo && git add -A && git commit -q -m "change greeting" &&
    git tag -d v1.0.0 >/dev/null && git tag v1.0.0)

(cd project1 && "$facet" install >/dev/null)
actual="$(cd project1 && "$diamond" -e 'require_cut "greeter"
greet("world")')"
[[ "$actual" == "hello, world" ]]

(cd project1 && "$facet" update >/dev/null)
actual="$(cd project1 && "$diamond" -e 'require_cut "greeter"
greet("world")')"
[[ "$actual" == "hi, world" ]]

# --- transitive dependency chain: a depends on b depends on c ---

mkdir c_repo
printf 'def c_value()\n  3\nend\n' > c_repo/c.di
echo '{"name": "c"}' > c_repo/diamond.cut
commit_repo c_repo
(cd c_repo && git tag v1.0.0)

mkdir b_repo
printf 'require_cut "c"\ndef b_value()\n  c_value() + 1\nend\n' > b_repo/b.di
cat > b_repo/diamond.cut <<EOF
{"name": "b", "dependencies": {"c": {"git": "$work/c_repo", "tag": "v1.0.0"}}}
EOF
commit_repo b_repo
(cd b_repo && git tag v1.0.0)

mkdir a_repo
printf 'require_cut "b"\ndef a_value()\n  b_value() + 1\nend\n' > a_repo/a.di
cat > a_repo/diamond.cut <<EOF
{"name": "a", "dependencies": {"b": {"git": "$work/b_repo", "tag": "v1.0.0"}}}
EOF
commit_repo a_repo
(cd a_repo && git tag v1.0.0)

mkdir project2
cat > project2/diamond.cut <<EOF
{"name": "myapp", "dependencies": {"a": {"git": "$work/a_repo", "tag": "v1.0.0"}}}
EOF
(cd project2 && "$facet" install >/dev/null)
[[ -f project2/cuts/a/a.di ]]
[[ -f project2/cuts/b/b.di ]]
[[ -f project2/cuts/c/c.di ]]
actual="$(cd project2 && "$diamond" -e 'require_cut "a"
a_value()')"
[[ "$actual" == "5" ]]

# --- conflicting refs for the same package name across two requesters ---

mkdir shared_repo
echo 'shared' > shared_repo/shared.di
echo '{"name": "shared"}' > shared_repo/diamond.cut
commit_repo shared_repo
(cd shared_repo && git tag v1.0.0)

mkdir y_repo
printf 'require_cut "shared"\n' > y_repo/y.di
cat > y_repo/diamond.cut <<EOF
{"name": "y", "dependencies": {"shared": {"git": "$work/shared_repo", "branch": "main"}}}
EOF
commit_repo y_repo

mkdir project3
cat > project3/diamond.cut <<EOF
{"name": "myapp", "dependencies": {"shared": {"git": "$work/shared_repo", "tag": "v1.0.0"}, "y": {"git": "$work/y_repo", "branch": "main"}}}
EOF
error_file="$(mktemp)"
if (cd project3 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly succeeded on conflicting dependency refs" >&2
    exit 1
fi
grep -q "conflicting dependency 'shared'" "$error_file"
grep -q "myapp" "$error_file"
grep -q "'y'" "$error_file"
rm -f "$error_file"

# --- a dependency whose own diamond.cut declares the wrong name is rejected ---

mkdir mislabeled_repo
echo 'x' > mislabeled_repo/x.di
echo '{"name": "not-what-it-is-called"}' > mislabeled_repo/diamond.cut
commit_repo mislabeled_repo

mkdir project4
cat > project4/diamond.cut <<EOF
{"name": "myapp", "dependencies": {"thing": {"git": "$work/mislabeled_repo", "branch": "main"}}}
EOF
error_file="$(mktemp)"
if (cd project4 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly accepted a mislabeled dependency" >&2
    exit 1
fi
grep -q "declares name 'not-what-it-is-called', expected 'thing'" "$error_file"
rm -f "$error_file"

# --- malformed dependency shapes are rejected before any cloning happens ---

mkdir project5
echo '{"name": "myapp", "dependencies": "oops"}' > project5/diamond.cut
error_file="$(mktemp)"
if (cd project5 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly accepted a non-Hash dependencies key" >&2
    exit 1
fi
grep -q "'dependencies' must be a Hash" "$error_file"
rm -f "$error_file"

echo '{"name": "myapp", "dependencies": {"x": {"git": "url"}}}' > project5/diamond.cut
error_file="$(mktemp)"
if (cd project5 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly accepted a dependency with no ref key" >&2
    exit 1
fi
grep -q "must specify exactly one of tag/branch/commit" "$error_file"
rm -f "$error_file"

echo '{"name": "myapp", "dependencies": {"x": {"git": "url", "tag": "a", "branch": "b"}}}' > project5/diamond.cut
error_file="$(mktemp)"
if (cd project5 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly accepted a dependency with ambiguous ref keys" >&2
    exit 1
fi
grep -q "must specify exactly one of tag/branch/commit" "$error_file"
rm -f "$error_file"

echo '{"name": "myapp", "dependencies": {"x": {"tag": "a"}}}' > project5/diamond.cut
error_file="$(mktemp)"
if (cd project5 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly accepted a dependency with no git key" >&2
    exit 1
fi
grep -q "must specify exactly one of git or registry" "$error_file"
rm -f "$error_file"

# none of the above malformed-manifest cases should ever have cloned anything
[[ ! -e project5/cuts/.facet-tmp ]] || [[ -z "$(ls -A project5/cuts/.facet-tmp 2>/dev/null)" ]]

# --- a manifest's git/ref fields reach git's own argv as plain positional
# strings, never shell-interpreted or parsed as git options -- a
# dependency declaring a "git" or ref value that starts with '-' must
# fail as an invalid repository/revision, not get silently parsed as a
# git flag (the well-known git argument-injection class: e.g. an
# "--upload-pack=..." value can otherwise reach arbitrary command
# execution through git's own hook/pack mechanisms). ---

mkdir project7
cat > project7/diamond.cut <<'EOF'
{"name": "myapp", "dependencies": {"x": {"git": "--upload-pack=touch /tmp/facet_injection_probe", "tag": "main"}}}
EOF
rm -f /tmp/facet_injection_probe
error_file="$(mktemp)"
if (cd project7 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly accepted an option-like 'git' value" >&2
    rm -f "$error_file"
    exit 1
fi
[[ ! -e /tmp/facet_injection_probe ]] || {
    echo "facet install executed an injected git option's command" >&2
    rm -f /tmp/facet_injection_probe "$error_file"
    exit 1
}
rm -f "$error_file"

mkdir project8
cat > project8/diamond.cut <<EOF
{"name": "myapp", "dependencies": {"greeter": {"git": "$work/greeter_repo", "tag": "--upload-pack=touch /tmp/facet_injection_probe"}}}
EOF
error_file="$(mktemp)"
if (cd project8 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly accepted an option-like ref value" >&2
    rm -f "$error_file"
    exit 1
fi
[[ ! -e /tmp/facet_injection_probe ]] || {
    echo "facet install executed an injected git option's command" >&2
    rm -f /tmp/facet_injection_probe "$error_file"
    exit 1
}
rm -f "$error_file"

# --- no diamond.cut at all ---

mkdir project6
error_file="$(mktemp)"
if (cd project6 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly succeeded with no diamond.cut" >&2
    exit 1
fi
grep -q "no diamond.cut found" "$error_file"
rm -f "$error_file"

# --- bare invocation and unknown subcommand print usage ---

error_file="$(mktemp)"
if "$facet" >/dev/null 2>"$error_file"; then
    echo "facet with no arguments unexpectedly succeeded" >&2
    exit 1
fi
grep -q "usage: facet" "$error_file"
rm -f "$error_file"

# --- version constraint: resolves to the highest tag satisfying the
# constraint, not just the newest commit or the first tag found ---

mkdir semver_repo
echo 'def semver_value() = 1' > semver_repo/semver.di
echo '{"name": "semver"}' > semver_repo/diamond.cut
commit_repo semver_repo
(cd semver_repo && git tag v1.0.0)
echo 'def semver_value() = 2' > semver_repo/semver.di
(cd semver_repo && git add -A && git commit -q -m "1.2.0" && git tag v1.2.0)
echo 'def semver_value() = 3' > semver_repo/semver.di
(cd semver_repo && git add -A && git commit -q -m "2.0.0" && git tag v2.0.0)

mkdir project9
cat > project9/diamond.cut <<EOF
{"name": "myapp", "dependencies": {"semver": {"git": "$work/semver_repo", "version": "^1.0.0"}}}
EOF
(cd project9 && "$facet" install >/dev/null)
actual="$(cd project9 && "$diamond" -e 'require_cut "semver"
semver_value()')"
[[ "$actual" == "2" ]]
grep -q '"version": "v1.2.0"' project9/facet.lock

# --- compatible version constraints from two different requesters
# intersect instead of hard-conflicting ---

mkdir uses_semver_a_repo
printf 'require_cut "semver"\ndef a_value() = semver_value()\n' > uses_semver_a_repo/uses_semver_a.di
cat > uses_semver_a_repo/diamond.cut <<EOF
{"name": "uses_semver_a", "dependencies": {"semver": {"git": "$work/semver_repo", "version": ">=1.0.0 <2.0.0"}}}
EOF
commit_repo uses_semver_a_repo
(cd uses_semver_a_repo && git tag v1.0.0)

mkdir project10
cat > project10/diamond.cut <<EOF
{"name": "myapp", "dependencies": {"uses_semver_a": {"git": "$work/uses_semver_a_repo", "tag": "v1.0.0"}, "semver": {"git": "$work/semver_repo", "version": "^1.2.0"}}}
EOF
(cd project10 && "$facet" install >/dev/null)
actual="$(cd project10 && "$diamond" -e 'require_cut "uses_semver_a"
a_value()')"
[[ "$actual" == "2" ]]

# --- incompatible version constraints hard-error, naming both requesters
# and both ranges, without ever cloning a version ---

mkdir uses_semver_b_repo
printf 'require_cut "semver"\n' > uses_semver_b_repo/uses_semver_b.di
cat > uses_semver_b_repo/diamond.cut <<EOF
{"name": "uses_semver_b", "dependencies": {"semver": {"git": "$work/semver_repo", "version": "^2.0.0"}}}
EOF
commit_repo uses_semver_b_repo
(cd uses_semver_b_repo && git tag v1.0.0)

mkdir project11
cat > project11/diamond.cut <<EOF
{"name": "myapp", "dependencies": {"uses_semver_b": {"git": "$work/uses_semver_b_repo", "tag": "v1.0.0"}, "semver": {"git": "$work/semver_repo", "version": "^1.0.0"}}}
EOF
error_file="$(mktemp)"
if (cd project11 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly succeeded on incompatible version constraints" >&2
    exit 1
fi
grep -q "conflicting dependency 'semver'" "$error_file"
grep -q "no version can satisfy both" "$error_file"
rm -f "$error_file"

# --- mixing an exact ref and a version constraint for the same
# dependency is a hard error, in both discovery orders ---

mkdir project12
cat > project12/diamond.cut <<EOF
{"name": "myapp", "dependencies": {"semver": {"git": "$work/semver_repo", "tag": "v1.0.0"}, "uses_semver_a": {"git": "$work/uses_semver_a_repo", "tag": "v1.0.0"}}}
EOF
error_file="$(mktemp)"
if (cd project12 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly succeeded mixing an exact ref and a version constraint" >&2
    exit 1
fi
grep -q "mixing an exact ref and a version constraint" "$error_file"
rm -f "$error_file"

mkdir project13
cat > project13/diamond.cut <<EOF
{"name": "myapp", "dependencies": {"uses_semver_a": {"git": "$work/uses_semver_a_repo", "tag": "v1.0.0"}, "semver": {"git": "$work/semver_repo", "tag": "v1.0.0"}}}
EOF
error_file="$(mktemp)"
if (cd project13 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly succeeded mixing a version constraint and an exact ref" >&2
    exit 1
fi
grep -q "mixing an exact ref and a version constraint" "$error_file"
rm -f "$error_file"

# --- a malformed version constraint is rejected at manifest-parse time,
# before any cloning happens ---

mkdir project14
echo '{"name": "myapp", "dependencies": {"x": {"git": "url", "version": "not-a-constraint"}}}' > project14/diamond.cut
error_file="$(mktemp)"
if (cd project14 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly accepted an invalid version constraint" >&2
    exit 1
fi
grep -q "invalid 'version' constraint" "$error_file"
rm -f "$error_file"

echo '{"name": "myapp", "dependencies": {"x": {"git": "url", "tag": "a", "version": "^1.0.0"}}}' > project14/diamond.cut
error_file="$(mktemp)"
if (cd project14 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly accepted both a tag and a version key" >&2
    exit 1
fi
grep -q "must specify exactly one of tag/branch/commit/version" "$error_file"
rm -f "$error_file"

# --- no tag satisfies the constraint at all ---

mkdir project15
cat > project15/diamond.cut <<EOF
{"name": "myapp", "dependencies": {"semver": {"git": "$work/semver_repo", "version": "^9.0.0"}}}
EOF
error_file="$(mktemp)"
if (cd project15 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly succeeded with no tag satisfying the constraint" >&2
    exit 1
fi
grep -q "no tag on '$work/semver_repo' satisfies" "$error_file"
rm -f "$error_file"

# --- facet init: writes a minimal manifest, refuses to clobber an
# existing one, and defaults the name from the current directory ---

mkdir project16
(cd project16 && "$facet" init explicit-name >/dev/null)
[[ "$(cd project16 && "$diamond" diamond.cut)" == "{name: explicit-name}" ]]

error_file="$(mktemp)"
if (cd project16 && "$facet" init another-name) >/dev/null 2>"$error_file"; then
    echo "facet init unexpectedly overwrote an existing diamond.cut" >&2
    exit 1
fi
grep -q "already exists" "$error_file"
rm -f "$error_file"
[[ "$(cd project16 && "$diamond" diamond.cut)" == "{name: explicit-name}" ]]

mkdir -p project17/my-default-name-dir
(cd project17/my-default-name-dir && "$facet" init >/dev/null)
[[ "$(cd project17/my-default-name-dir && "$diamond" diamond.cut)" == "{name: my-default-name-dir}" ]]

# --- facet add: appends a dependency and the result actually installs
# -- one real end-to-end check per ref kind (tag/branch/commit/version),
# not just a manifest-content assertion, since the interesting risk is
# write_manifest round-tripping the *right* key (see FacetRefKind) ---

mkdir project18
(cd project18 && "$facet" init myapp >/dev/null &&
    "$facet" add greeter --git "$work/greeter_repo" --tag v1.0.0 >/dev/null &&
    "$facet" install >/dev/null)
[[ "$(cd project18 && "$diamond" -e 'require_cut "greeter"
greet("world")')" == "hi, world" ]]

mkdir project19
(cd project19 && "$facet" init myapp >/dev/null &&
    "$facet" add greeter --git "$work/greeter_repo" --branch main >/dev/null &&
    "$facet" install >/dev/null)
[[ "$(cd project19 && "$diamond" -e 'require_cut "greeter"
greet("world")')" == "hi, world" ]]

mkdir project20
(cd project20 && "$facet" init myapp >/dev/null &&
    "$facet" add greeter --git "$work/greeter_repo" --commit "$greeter_v1" >/dev/null &&
    "$facet" install >/dev/null)
[[ "$(cd project20 && "$diamond" -e 'require_cut "greeter"
greet("world")')" == "hello, world" ]]

mkdir project21
(cd project21 && "$facet" init myapp >/dev/null &&
    "$facet" add semver --git "$work/semver_repo" --version "^1.0.0" >/dev/null &&
    "$facet" install >/dev/null)
[[ -f project21/cuts/semver/semver.di ]]

mkdir project21_registry
(cd project21_registry && "$facet" init myapp >/dev/null &&
    "$facet" add greeter --registry https://cuts.example/api \
        --version "^1.0.0" >/dev/null)
[[ "$(cd project21_registry && "$diamond" diamond.cut)" == \
    "{name: myapp, dependencies: {greeter: {registry: https://cuts.example/api, version: ^1.0.0}}}" ]]

# Registry update resolves metadata (including transitive dependencies) before
# downloading either archive, skips yanked versions, writes the full lock, and
# then installs both verified artifacts.
mkdir -p registry_source/logger/lib registry_metadata
printf '%s\n' '{"name": "logger", "version": "1.0.0", "summary": "Registry logger", "license": "MIT", "maintainers": [{"name": "Test", "contact": "test@example.com"}]}' > registry_source/logger/diamond.cut
printf '%s\n' '# Registry logger' > registry_source/logger/README.md
printf '%s\n' 'MIT' > registry_source/logger/LICENSE
printf '%s\n' 'def registry_log()' '  "logged"' 'end' > registry_source/logger/lib/logger.di
"$facet" pack registry_source/logger registry_logger.tar >/dev/null
logger_digest="$(sha256sum registry_logger.tar | cut -d' ' -f1)"
logger_size="$(stat -c %s registry_logger.tar)"
cat > registry_metadata/greeter-index <<'EOF'
{"protocol": 1, "versions": [{"version": "1.2.3", "yanked": false}, {"version": "1.3.0", "yanked": false}, {"version": "9.0.0", "yanked": true}]}
EOF
cat > registry_metadata/greeter-release <<EOF
{"protocol": 1, "name": "greeter", "version": "1.2.3", "dependencies": {"logger": "^1.0.0"}, "yanked": false, "archive": {"path": "/v1/blobs/sha256/$digest", "sha256": "$digest", "size": $archive_size}}
EOF
sed 's/"version": "1.2.3"/"version": "1.3.0"/' registry_metadata/greeter-release > registry_metadata/greeter-release-new
cat > registry_metadata/logger-index <<'EOF'
{"protocol": 1, "versions": [{"version": "1.0.0", "yanked": false}]}
EOF
cat > registry_metadata/logger-release <<EOF
{"protocol": 1, "name": "logger", "version": "1.0.0", "dependencies": {}, "yanked": false, "archive": {"path": "/v1/blobs/sha256/$logger_digest", "sha256": "$logger_digest", "size": $logger_size}}
EOF
cat > project21_registry/facet.lock <<EOF
{"greeter": {"source": "registry", "registry": "https://cuts.example/api", "version": "1.2.3", "sha256": "$digest", "size": $archive_size}}
EOF
cat > fake_bin/curl <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
output=
url=
while (($#)); do
    case "$1" in
        --output) output="$2"; shift 2 ;;
        --) url="$2"; break ;;
        *) shift ;;
    esac
done
case "$url" in
    */v1/cuts/greeter/versions) cp "$FACET_TEST_METADATA/greeter-index" "$output" ;;
    */v1/cuts/greeter/versions/1.2.3) cp "$FACET_TEST_METADATA/greeter-release" "$output" ;;
    */v1/cuts/greeter/versions/1.3.0) cp "$FACET_TEST_METADATA/greeter-release-new" "$output" ;;
    */v1/cuts/logger/versions) cp "$FACET_TEST_METADATA/logger-index" "$output" ;;
    */v1/cuts/logger/versions/1.0.0) cp "$FACET_TEST_METADATA/logger-release" "$output" ;;
    */v1/blobs/sha256/"$FACET_TEST_GREETER_DIGEST") cp "$FACET_TEST_GREETER_ARCHIVE" "$output" ;;
    */v1/blobs/sha256/"$FACET_TEST_LOGGER_DIGEST") cp "$FACET_TEST_LOGGER_ARCHIVE" "$output" ;;
    *) echo "unexpected registry URL: $url" >&2; exit 22 ;;
esac
EOF
chmod +x fake_bin/curl
saved_path="$PATH"
export PATH="$work/fake_bin:$PATH"
export FACET_TEST_METADATA="$work/registry_metadata"
export FACET_TEST_GREETER_DIGEST="$digest" FACET_TEST_LOGGER_DIGEST="$logger_digest"
export FACET_TEST_GREETER_ARCHIVE="$work/registry_greeter.tar"
export FACET_TEST_LOGGER_ARCHIVE="$work/registry_logger.tar"
(cd project21_registry && "$facet" update >/dev/null)
grep -q '"greeter": {"source": "registry"' project21_registry/facet.lock
grep -q '"logger": {"source": "registry"' project21_registry/facet.lock
[[ -f project21_registry/cuts/greeter/lib/greeter.di ]]
[[ -f project21_registry/cuts/logger/lib/logger.di ]]
export PATH="$saved_path"
unset FACET_TEST_METADATA FACET_TEST_GREETER_DIGEST FACET_TEST_LOGGER_DIGEST
unset FACET_TEST_GREETER_ARCHIVE FACET_TEST_LOGGER_ARCHIVE

# --- facet add: rejects a duplicate name, an ambiguous or missing ref,
# an invalid version constraint, and running before facet init ---

mkdir project22
(cd project22 && "$facet" init myapp >/dev/null &&
    "$facet" add greeter --git "$work/greeter_repo" --tag v1.0.0 >/dev/null)
error_file="$(mktemp)"
if (cd project22 && "$facet" add greeter --git "$work/greeter_repo" --tag v2.0.0) \
        >/dev/null 2>"$error_file"; then
    echo "facet add unexpectedly overwrote an existing dependency" >&2
    exit 1
fi
grep -q "already exists" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if (cd project22 && "$facet" add other --git "$work/greeter_repo") \
        >/dev/null 2>"$error_file"; then
    echo "facet add unexpectedly accepted no ref/version at all" >&2
    exit 1
fi
grep -q "specify exactly one of --tag, --branch, --commit, --version" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if (cd project22 && "$facet" add other --git "$work/greeter_repo" \
        --tag v1.0.0 --branch main) >/dev/null 2>"$error_file"; then
    echo "facet add unexpectedly accepted both a tag and a branch" >&2
    exit 1
fi
grep -q "specify exactly one of --tag, --branch, --commit, --version" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if (cd project22 && "$facet" add other --git "$work/greeter_repo" \
        --registry https://cuts.example --version '^1.0.0') \
        >/dev/null 2>"$error_file"; then
    echo "facet add unexpectedly accepted mixed sources" >&2
    exit 1
fi
grep -q "exactly one of --git or --registry" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if (cd project22 && "$facet" add other --registry http://cuts.example \
        --version '^1.0.0') >/dev/null 2>"$error_file"; then
    echo "facet add unexpectedly accepted an insecure registry" >&2
    exit 1
fi
grep -q "not a valid registry URL" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if (cd project22 && "$facet" add other --git "$work/greeter_repo" \
        --version "not-a-constraint") >/dev/null 2>"$error_file"; then
    echo "facet add unexpectedly accepted an invalid version constraint" >&2
    exit 1
fi
grep -q "not a valid version constraint" "$error_file"
rm -f "$error_file"

mkdir project23
error_file="$(mktemp)"
if (cd project23 && "$facet" add greeter --git "$work/greeter_repo" --tag v1.0.0) \
        >/dev/null 2>"$error_file"; then
    echo "facet add unexpectedly succeeded with no diamond.cut present" >&2
    exit 1
fi
grep -q "run 'facet init' first" "$error_file"
rm -f "$error_file"

# --- facet add: existing dependencies survive a second `add` unchanged
# (write_manifest regenerates the whole file, so this is the real check
# that it does so losslessly for every ref kind, not just the new one) ---

(cd project18 && "$facet" add other --git "$work/greeter_repo" --branch main >/dev/null)
[[ "$(cd project18 && "$diamond" diamond.cut)" == \
    "{name: myapp, dependencies: {greeter: {git: $work/greeter_repo, tag: v1.0.0}, other: {git: $work/greeter_repo, branch: main}}}" ]]

# --- real backtracking: a name resolved via one requester's own looser
# constraint, then constrained tighter by a requester whose own nested
# dependency is only discovered afterward (breadth-first: root's two
# version-constrained deps both go pending immediately with nothing
# queued yet, so `semver` resolves -- to v1.2.0, the highest match for
# root's own "^1.0.0" -- a full outer-loop iteration before `needs_old`
# resolves and its own clone's diamond.cut is ever read) resolves to the
# one tag satisfying *both* constraints instead of hard-erroring the way
# it used to (the exact scenario docs/roadmap.md's former "not attempted
# yet" gap named) ---

mkdir needs_old_repo
printf 'require_cut "semver"\ndef old_value() = semver_value()\n' > needs_old_repo/needs_old.di
cat > needs_old_repo/diamond.cut <<EOF
{"name": "needs_old", "dependencies": {"semver": {"git": "$work/semver_repo", "version": ">=1.0.0 <1.2.0"}}}
EOF
commit_repo needs_old_repo
(cd needs_old_repo && git tag v1.0.0)

mkdir project24
cat > project24/diamond.cut <<EOF
{"name": "myapp", "dependencies": {"semver": {"git": "$work/semver_repo", "version": "^1.0.0"}, "needs_old": {"git": "$work/needs_old_repo", "version": "^1.0.0"}}}
EOF
output_file="$(mktemp)"
(cd project24 && "$facet" install) >/dev/null 2>"$output_file"
! grep -q "is not supported yet" "$output_file"
rm -f "$output_file"
actual="$(cd project24 && "$diamond" -e 'require_cut "semver"
semver_value()')"
[[ "$actual" == "1" ]]
grep -q '"version": "v1.0.0"' project24/facet.lock

# --- ...but a genuinely disjoint pair of constraints (no tag could ever
# satisfy both, independent of resolution order) still hard-errors right
# away -- no restart wasted on an unsatisfiable graph ---

mkdir project25
cat > project25/diamond.cut <<EOF
{"name": "myapp", "dependencies": {"semver": {"git": "$work/semver_repo", "version": "^2.0.0"}, "needs_old": {"git": "$work/needs_old_repo", "version": "^1.0.0"}}}
EOF
error_file="$(mktemp)"
if (cd project25 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet install unexpectedly succeeded on an unsatisfiable version graph" >&2
    exit 1
fi
grep -q "conflicting dependency 'semver'" "$error_file"
grep -q "no version can satisfy both" "$error_file"
rm -f "$error_file"

# Metadata must not execute while facet reads a project manifest or lockfile.
mkdir project26
printf '{"name": "myapp"}\nraise "executed manifest"\n' > project26/diamond.cut
error_file="$(mktemp)"
if (cd project26 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet accepted executable manifest" >&2
    exit 1
fi
grep -q "must be data only" "$error_file"
! grep -q "executed manifest" "$error_file"
rm -f "$error_file"

printf '{"name": "myapp"}\n' > project26/diamond.cut
printf '{}\nraise "executed lock"\n' > project26/facet.lock
error_file="$(mktemp)"
if (cd project26 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet accepted executable lockfile" >&2
    exit 1
fi
grep -q "must be data only" "$error_file"
! grep -q "executed lock" "$error_file"
rm -f "$error_file"

printf '{"name": "#{raise(\"executed interpolation\")}"}\n' > project26/diamond.cut
rm -f project26/facet.lock
error_file="$(mktemp)"
if (cd project26 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet accepted interpolated manifest String" >&2
    exit 1
fi
grep -q "String interpolation is not allowed" "$error_file"
! grep -q "executed interpolation" "$error_file"
rm -f "$error_file"

printf '{"name": "myapp"' > project26/diamond.cut
error_file="$(mktemp)"
if (cd project26 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet accepted truncated manifest" >&2
    exit 1
fi
grep -q "expected ',' or closing delimiter" "$error_file"
rm -f "$error_file"

printf '{"name": "myapp", "name": "other"}\n' > project26/diamond.cut
error_file="$(mktemp)"
if (cd project26 && "$facet" install) >/dev/null 2>"$error_file"; then
    echo "facet accepted duplicate manifest key" >&2
    exit 1
fi
grep -q "duplicate Hash key" "$error_file"
rm -f "$error_file"

printf '{"name": "my\\u0061pp"}\n' > project26/diamond.cut
(cd project26 && "$facet" install >/dev/null)
[[ -f project26/facet.lock ]]

printf '{"name": "myapp"}\000{"name": "other"}\n' > project26/diamond.cut
error_file="$(mktemp)"
if (cd project26 && "$facet" update) >/dev/null 2>"$error_file"; then
    echo "facet accepted NUL in manifest" >&2
    exit 1
fi
grep -q "contains a NUL byte" "$error_file"
rm -f "$error_file"

# --- publishable cut preflight, independent of the project working directory ---
mkdir -p checkcut/lib
printf '{"name": "checkcut", "version": "1.2.0", "summary": "A checked cut", "license": "MIT", "maintainers": [{"name": "Test", "contact": "test@example.com"}], "dependencies": {"logger": "^0.4.0"}}\n' > checkcut/diamond.cut
printf '# Checkcut\n' > checkcut/README.md
printf 'MIT License\n' > checkcut/LICENSE
printf 'def checkcut_value() = 1\n' > checkcut/lib/checkcut.di
"$facet" check checkcut >/dev/null

printf 'require "../../http/lib/http"\n' > checkcut/lib/checkcut.di
error_file="$(mktemp)"
if "$facet" check checkcut >/dev/null 2>"$error_file"; then
    echo "facet check accepted an import outside the cut" >&2
    exit 1
fi
grep -q "imports outside the cut" "$error_file"
rm -f "$error_file"

printf 'require_cut "missing_cut"\n' > checkcut/lib/checkcut.di
error_file="$(mktemp)"
if "$facet" check checkcut >/dev/null 2>"$error_file"; then
    echo "facet check accepted an undeclared cut import" >&2
    exit 1
fi
grep -q "imports undeclared cut 'missing_cut'" "$error_file"
rm -f "$error_file"

printf 'require_cut "logger"\ndef checkcut_value() = 1\n' > checkcut/lib/checkcut.di
"$facet" check checkcut >/dev/null
printf 'def checkcut_value() = 1\n' > checkcut/lib/checkcut.di

rm checkcut/LICENSE
error_file="$(mktemp)"
if "$facet" check checkcut >/dev/null 2>"$error_file"; then
    echo "facet check accepted a cut without a license file" >&2
    exit 1
fi
grep -q "needs a regular LICENSE" "$error_file"
rm -f "$error_file"
printf 'MIT License\n' > checkcut/LICENSE

printf '{"name": "checkcut", "version": "1.2.0", "summary": "A checked cut", "license": "MIT", "maintainers": [{"name": "Test", "contact": "test@example.com"}], "dependencies": {"logger": {"git": "example", "version": "^0.4.0"}}}\n' > checkcut/diamond.cut
error_file="$(mktemp)"
if "$facet" check checkcut >/dev/null 2>"$error_file"; then
    echo "facet check accepted a Git dependency in a release manifest" >&2
    exit 1
fi
grep -q "dependency 'logger' needs a valid SemVer range String" "$error_file"
rm -f "$error_file"

# Source manifests must name public maintainers with an email or https contact.
for maintainers in '' ', "maintainers": []' \
    ', "maintainers": [{"name": "Test", "contact": "http://example.com"}]' \
    ', "maintainers": [{"name": "Test", "contact": "test@localhost"}]' \
    ', "maintainers": [{"name": "Test", "contact": "test@example.com", "role": "lead"}]' \
    ', "maintainers": [{"name": "A", "contact": "a@example.com"}, {"name": "B", "contact": "a@example.com"}]'; do
    printf '{"name": "checkcut", "version": "1.2.0", "summary": "A checked cut", "license": "MIT"%s}\n' "$maintainers" > checkcut/diamond.cut
    error_file="$(mktemp)"
    if "$facet" check checkcut >/dev/null 2>"$error_file"; then
        echo "facet check accepted invalid maintainers: $maintainers" >&2
        exit 1
    fi
    grep -q "maintainer" "$error_file"
    rm -f "$error_file"
done
printf '{"name": "checkcut", "version": "1.2.0", "summary": "A checked cut", "license": "MIT", "maintainers": [{"name": "Test \\"Q\\"", "contact": "test@example.com"}, {"name": "Site", "contact": "https://example.com/issues"}]}\n' > checkcut/diamond.cut
maintained_archive="$(mktemp -d)/checkcut.tar"
"$facet" pack checkcut "$maintained_archive" >/dev/null
[[ "$("$facet" verify "$maintained_archive" --json)" == *'"maintainers":[{"name":"Test \"Q\"","contact":"test@example.com"},{"name":"Site","contact":"https://example.com/issues"}]}' ]]
# Catalog display fields and the README come from the verified archive.
[[ "$("$facet" verify "$maintained_archive" --json)" == *'"summary":"A checked cut","license":"MIT","dependencies":'* ]]
[[ "$("$facet" verify "$maintained_archive" --readme)" == "$(cat checkcut/README.md)" ]]
if "$facet" verify "$maintained_archive" --json --readme >/dev/null 2>&1; then
    echo "facet verify accepted --json with --readme" >&2
    exit 1
fi
# Archives published before the field existed still verify, reporting none.
python3 - "$maintained_archive" <<'PY'
import re, sys
path = sys.argv[1]
data = open(path, 'rb').read()
match = re.search(rb', "maintainers": \[.*?\]\]?(?=\})', data)
assert match
open(path, 'wb').write(data[:match.start()] + b' ' * (match.end() - match.start()) + data[match.end():])
PY
[[ "$("$facet" verify "$maintained_archive" --json)" == *'"maintainers":[]}' ]]
rm -rf "$(dirname "$maintained_archive")"

printf '{"name": "checkcut", "version": "1.2.0", "summary": "A checked cut", "license": "MIT", "maintainers": [{"name": "Test", "contact": "test@example.com"}]}\n' > checkcut/diamond.cut
mkdir -p checkcut/lib/checkcut checkcut/tests
printf 'def helper() = 2\n' > checkcut/lib/checkcut/helper.di
printf 'ignored test\n' > checkcut/tests/test.di
files_output="$("$facet" check checkcut --files)"
grep -q '^lib/checkcut/helper.di$' <<<"$files_output"
! grep -q 'tests/test.di' <<<"$files_output"
[[ "$(printf '%s\n' "$files_output" | sed '/^facet:/d' | LC_ALL=C sort)" == \
   "$(printf '%s\n' "$files_output" | sed '/^facet:/d')" ]]

ln -s /etc/passwd checkcut/lib/checkcut/linked.di
error_file="$(mktemp)"
if "$facet" check checkcut >/dev/null 2>"$error_file"; then
    echo "facet check accepted symlink in runtime tree" >&2
    exit 1
fi
grep -q "unsafe file type or hardlink" "$error_file"
rm -f "$error_file"
rm checkcut/lib/checkcut/linked.di

printf 'stale bytecode\n' > checkcut/lib/checkcut/helper.dic
error_file="$(mktemp)"
if "$facet" check checkcut >/dev/null 2>"$error_file"; then
    echo "facet check accepted cache file in runtime tree" >&2
    exit 1
fi
grep -q "excluded or sensitive path" "$error_file"
rm -f "$error_file"
rm checkcut/lib/checkcut/helper.dic

mkdir checkcut/lib/Widgets checkcut/lib/widgets
error_file="$(mktemp)"
if "$facet" check checkcut >/dev/null 2>"$error_file"; then
    echo "facet check accepted case-colliding directories" >&2
    exit 1
fi
grep -q "case-insensitive path collision" "$error_file"
rm -f "$error_file"
rm -rf checkcut/lib/Widgets checkcut/lib/widgets

"$facet" pack checkcut one.tar > pack_output
tar -tf one.tar > packed_files
grep -q '^lib/checkcut/helper.di$' packed_files
! grep -q '^tests/' packed_files
grep -q '^sha256: ' pack_output
[[ "$(sed -n 's/^sha256: //p' pack_output)" == "$(sha256sum one.tar | cut -d' ' -f1)" ]]
touch -t 202001010000 checkcut/lib/checkcut/helper.di
"$facet" pack checkcut two.tar >/dev/null
cmp one.tar two.tar

error_file="$(mktemp)"
if "$facet" pack checkcut one.tar >/dev/null 2>"$error_file"; then
    echo "facet pack overwrote an existing archive" >&2
    exit 1
fi
grep -q "cannot create" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$facet" pack checkcut checkcut/package.tar >/dev/null 2>"$error_file"; then
    echo "facet pack wrote inside the cut directory" >&2
    exit 1
fi
grep -q "outside the cut directory" "$error_file"
rm -f "$error_file"

digest="$(sha256sum one.tar | cut -d' ' -f1)"
"$facet" verify one.tar --sha256 "$digest" > verify_output
grep -q '^facet: verified checkcut 1.2.0 (5 files)$' verify_output

# A crafted archive must not bypass the literal-import checks used by
# `facet check`. Change only payload bytes, keeping canonical tar headers.
for import in 'require "../../x"' 'require_cut "missing"'; do
    cp one.tar imported.tar
    python3 - imported.tar "$import" <<'PY'
import sys
import tarfile

path, statement = sys.argv[1:]
with tarfile.open(path) as archive:
    member = archive.getmember("lib/checkcut.di")
payload = (statement + "\n").encode().ljust(member.size, b" ")
assert len(payload) == member.size
with open(path, "r+b") as archive:
    archive.seek(member.offset_data)
    archive.write(payload)
PY
    error_file="$(mktemp)"
    if "$facet" verify imported.tar >/dev/null 2>"$error_file"; then
        echo "facet verify accepted an unsafe import" >&2
        exit 1
    fi
    grep -Eq 'imports outside the cut|imports undeclared cut' "$error_file"
    rm -f "$error_file"
done

error_file="$(mktemp)"
if "$facet" verify one.tar --sha256 "$(printf '0%.0s' {1..64})" >/dev/null 2>"$error_file"; then
    echo "facet verify accepted the wrong digest" >&2
    exit 1
fi
grep -q "does not match expected digest" "$error_file"
rm -f "$error_file"

cp one.tar malformed.tar
printf '2' | dd of=malformed.tar bs=1 seek=156 conv=notrunc status=none
error_file="$(mktemp)"
if "$facet" verify malformed.tar >/dev/null 2>"$error_file"; then
    echo "facet verify accepted a changed tar entry type" >&2
    exit 1
fi
grep -q "noncanonical or unsafe archive entry" "$error_file"
rm -f "$error_file"

cp one.tar trailing.tar
printf 'unexpected' >> trailing.tar
error_file="$(mktemp)"
if "$facet" verify trailing.tar >/dev/null 2>"$error_file"; then
    echo "facet verify accepted trailing archive bytes" >&2
    exit 1
fi
grep -q "invalid archive ending" "$error_file"
rm -f "$error_file"

# Dependency-free bundled cuts must remain packageable and load from their
# installed artifact layout, without access to the source checkout.
mkdir -p packaged_project/cuts
for name in arel http logger; do
    "$facet" check "$source_root/packages/$name" >/dev/null
    "$facet" pack "$source_root/packages/$name" "$name.tar" >/dev/null
    digest="$(sha256sum "$name.tar" | cut -d' ' -f1)"
    "$facet" verify "$name.tar" --sha256 "$digest" >/dev/null
    mkdir "packaged_project/cuts/$name"
    tar -xf "$name.tar" -C "packaged_project/cuts/$name"
done
(cd packaged_project && "$diamond" -e 'require_cut "arel"
require_cut "http"
require_cut "logger"
Arel.table("people").name()' >/dev/null)

for name in active_karma cookies database_config dials div graphql jobs \
            log_viewer multipart rack redis websocket; do
    "$facet" check "$source_root/packages/$name" >/dev/null
    "$facet" pack "$source_root/packages/$name" "$name.tar" >/dev/null
    digest="$(sha256sum "$name.tar" | cut -d' ' -f1)"
    "$facet" verify "$name.tar" --sha256 "$digest" >/dev/null
    mkdir "packaged_project/cuts/$name"
    tar -xf "$name.tar" -C "packaged_project/cuts/$name"
    (cd packaged_project && "$diamond" -e "require_cut \"$name\"" >/dev/null)
done

"$facet" check "$source_root/packages/network_safety" >/dev/null
"$facet" pack "$source_root/packages/network_safety" network_safety.tar >/dev/null
digest="$(sha256sum network_safety.tar | cut -d' ' -f1)"
"$facet" verify network_safety.tar --sha256 "$digest" >/dev/null
mkdir packaged_project/cuts/network_safety
tar -xf network_safety.tar -C packaged_project/cuts/network_safety
actual="$(cd packaged_project && "$diamond" -e 'require_cut "network_safety"
resolve_public_hostname("http://8.8.8.8/x")["host"]')"
[[ "$actual" == "8.8.8.8" ]]

"$facet" check "$source_root/packages/gremlin" >/dev/null
"$facet" pack "$source_root/packages/gremlin" gremlin.tar >/dev/null
digest="$(sha256sum gremlin.tar | cut -d' ' -f1)"
"$facet" verify gremlin.tar --sha256 "$digest" >/dev/null
mkdir packaged_project/cuts/gremlin
tar -xf gremlin.tar -C packaged_project/cuts/gremlin
(cd packaged_project && "$diamond" -e 'require_cut "gremlin"' >/dev/null)

"$source_root/tools/install_local_cuts.sh" packaged_project >/dev/null
for name in active_record active_auth active_discussion active_social active_tagging graphsql; do
    (cd packaged_project && "$diamond" -e "require_cut \"$name\"" >/dev/null)
done

echo "all facet tests passed"
