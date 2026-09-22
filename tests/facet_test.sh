#!/usr/bin/env bash
set -euo pipefail

# facet is tested against local git repositories only - git clone works
# identically against a local path as it does against a remote URL, so
# this never touches the network (matching how the HTTP tests avoid the
# real internet by running an in-process server instead).

diamond="$(realpath ./build/diamond)"
facet="$(realpath ./build/facet)"

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
grep -q "missing a valid String 'git' key" "$error_file"
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
printf '{"name": "checkcut", "version": "1.2.0", "summary": "A checked cut", "license": "MIT", "dependencies": {"logger": "^0.4.0"}}\n' > checkcut/diamond.cut
printf '# Checkcut\n' > checkcut/README.md
printf 'MIT License\n' > checkcut/LICENSE
printf 'def checkcut_value() = 1\n' > checkcut/lib/checkcut.di
"$facet" check checkcut >/dev/null

rm checkcut/LICENSE
error_file="$(mktemp)"
if "$facet" check checkcut >/dev/null 2>"$error_file"; then
    echo "facet check accepted a cut without a license file" >&2
    exit 1
fi
grep -q "needs a regular LICENSE" "$error_file"
rm -f "$error_file"
printf 'MIT License\n' > checkcut/LICENSE

printf '{"name": "checkcut", "version": "1.2.0", "summary": "A checked cut", "license": "MIT", "dependencies": {"logger": {"git": "example", "version": "^0.4.0"}}}\n' > checkcut/diamond.cut
error_file="$(mktemp)"
if "$facet" check checkcut >/dev/null 2>"$error_file"; then
    echo "facet check accepted a Git dependency in a release manifest" >&2
    exit 1
fi
grep -q "dependency 'logger' needs a valid SemVer range String" "$error_file"
rm -f "$error_file"

printf '{"name": "checkcut", "version": "1.2.0", "summary": "A checked cut", "license": "MIT"}\n' > checkcut/diamond.cut
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

echo "74 facet tests passed"
