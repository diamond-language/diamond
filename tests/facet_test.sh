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

echo "23 facet tests passed"
