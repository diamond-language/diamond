#!/usr/bin/env bash
# Self-hosting Phase 3 differential harness: for every
# tests/parser_cases/*.di file, compares the real C-compiled-and-run
# result against the same source run through selfhost/parser.di's
# Diamond-language Parser + the native ProgramBuilder bridge. The curated
# positive corpus grows with the supported grammar; parser_error_cases
# separately locks matching rejection diagnostics into both compilers.
# See docs/roadmap.md's self-hosting Phase 3 entry.
#
# Part of `make test-self-host`, not `make test-all`: self-hosting is in
# minimal-compat maintenance mode (docs/roadmap.md) while the native
# language is still moving, so this full corpus is opt-in/periodic rather
# than run on every push. tests/self_host_smoke.sh -- part of test-all --
# covers the two bootstrap checks that used to live at the end of this
# file, as a cheap "still basically works" signal.
#
# The two corpora are driven by selfhost/parser_positive_suite.di and
# selfhost/parser_error_suite.di, Minitest suites that require parser.di
# exactly once each instead of once per case -- see parser_positive_suite.di's
# own comment for why. That only saves the native recompile, though: the
# positive suite's own per-case cost is dominated by the self-hosted
# Parser re-parsing all of lib/core.di through the interpreter for every
# case (confirmed by direct profiling -- ProgramBuilder#run's own verify+
# execute cost is ~1ms), which is inherent to what parse_and_run_with_core
# does and not something a test-harness change fixes. The one-off
# scenarios below (source maps, require/loaded-file limits,
# redefine_method) each run only once already, so they stay as direct
# per-scenario `diamond` invocations.
set -euo pipefail

diamond=./build/diamond

case_files=(tests/parser_cases/*.di)
"$diamond" selfhost/parser_positive_suite.di "${case_files[@]}"

error_case_files=(tests/parser_error_cases/*.di)
"$diamond" selfhost/parser_error_suite.di "${error_case_files[@]}"

depth_dir="$(mktemp -d)"
trap 'rm -rf "$depth_dir"' EXIT
for depth in $(seq 0 128); do
    if [[ "$depth" -eq 128 ]]; then
        printf '42\n' >"$depth_dir/f$depth.di"
    else
        printf 'require "f%s"\n' "$((depth + 1))" >"$depth_dir/f$depth.di"
    fi
done
if "$diamond" "$depth_dir/f0.di" >/tmp/diamond-parser-native.out 2>&1; then
    echo "native require-depth limit unexpectedly succeeded" >&2
    exit 1
fi
grep -Fq "require nesting limit reached" /tmp/diamond-parser-native.out
if echo "$depth_dir/f0.di" | "$diamond" selfhost/parser_check.di \
        >/tmp/diamond-parser-selfhost.out 2>&1; then
    echo "self-hosted require-depth limit unexpectedly succeeded" >&2
    exit 1
fi
grep -Fq "require nesting limit reached" /tmp/diamond-parser-selfhost.out
rm -rf "$depth_dir"
trap - EXIT

echo "require-depth differential case passed"

files_dir="$(mktemp -d)"
trap 'rm -rf "$files_dir"' EXIT
: >"$files_dir/main.di"
for file_index in $(seq 0 127); do
    printf '42\n' >"$files_dir/f$file_index.di"
    printf 'require "f%s"\n' "$file_index" >>"$files_dir/main.di"
done
if "$diamond" "$files_dir/main.di" >/tmp/diamond-parser-native.out 2>&1; then
    echo "native loaded-file limit unexpectedly succeeded" >&2
    exit 1
fi
grep -Fq "loaded-file limit reached" /tmp/diamond-parser-native.out
if echo "$files_dir/main.di" | "$diamond" selfhost/parser_check.di \
        >/tmp/diamond-parser-selfhost.out 2>&1; then
    echo "self-hosted loaded-file limit unexpectedly succeeded" >&2
    exit 1
fi
grep -Fq "loaded-file limit reached" /tmp/diamond-parser-selfhost.out
rm -rf "$files_dir"
trap - EXIT

echo "loaded-file differential case passed"

crlf_compile_dir="$(mktemp -d)"
trap 'rm -rf "$crlf_compile_dir"' EXIT
printf 'if true\r\n  1\r\n' >"$crlf_compile_dir/broken.di"
printf 'require "broken"\r\n' >"$crlf_compile_dir/main.di"
native_crlf_compile="$($diamond "$crlf_compile_dir/main.di" 2>&1 || true)"
selfhost_crlf_compile="$(echo "$crlf_compile_dir/main.di" | \
    $diamond selfhost/parser_check.di 2>&1 || true)"
for output in "$native_crlf_compile" "$selfhost_crlf_compile"; do
    grep -Fq "broken.di:2:1" <<<"$output"
done
rm -rf "$crlf_compile_dir"
trap - EXIT

echo "CRLF compile source-map differential case passed"

runtime_fixture="tests/parser_runtime_fixtures/mapped_runtime_main.di"
native_runtime="$($diamond "$runtime_fixture" 2>&1 || true)"
selfhost_runtime="$(echo "$runtime_fixture" | $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_runtime" "$selfhost_runtime"; do
    grep -Fq "at mapped_explode:2:10" <<<"$output"
    grep -Fq "at tests/parser_runtime_fixtures/mapped_runtime_main.di:1:19" <<<"$output"
done

echo "runtime source-map differential case passed"

crlf_runtime_dir="$(mktemp -d)"
trap 'rm -rf "$crlf_runtime_dir"' EXIT
printf 'def crlf_explode(value)\r\n  value[4]\r\nend\r\n' \
    >"$crlf_runtime_dir/runtime.di"
printf 'require "runtime"\r\ncrlf_explode([1])\r\n' \
    >"$crlf_runtime_dir/main.di"
native_crlf_runtime="$($diamond "$crlf_runtime_dir/main.di" 2>&1 || true)"
selfhost_crlf_runtime="$(echo "$crlf_runtime_dir/main.di" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_crlf_runtime" "$selfhost_crlf_runtime"; do
    grep -Fq "at crlf_explode:2:10" <<<"$output"
    grep -Fq "at $crlf_runtime_dir/main.di:" <<<"$output"
done
rm -rf "$crlf_runtime_dir"
trap - EXIT

echo "CRLF runtime source-map differential case passed"

stress_native_runtime="$(DIAMOND_STRESS_GC=1 $diamond "$runtime_fixture" 2>&1 || true)"
stress_selfhost_runtime="$(echo "$runtime_fixture" | DIAMOND_STRESS_GC=1 \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$stress_native_runtime" "$stress_selfhost_runtime"; do
    grep -Fq "at mapped_explode:2:10" <<<"$output"
    grep -Fq "at tests/parser_runtime_fixtures/mapped_runtime_main.di:1:19" \
        <<<"$output"
done

echo "stress-GC runtime source-map differential case passed"

nested_runtime_fixture="tests/parser_runtime_fixtures/nested_runtime_main.di"
native_nested_runtime="$($diamond "$nested_runtime_fixture" 2>&1 || true)"
selfhost_nested_runtime="$(echo "$nested_runtime_fixture" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_nested_runtime" "$selfhost_nested_runtime"; do
    grep -Fq "at nested_explode:2:10" <<<"$output"
    grep -Fq "at nested_runtime_mid:1:46" <<<"$output"
    grep -Fq "at tests/parser_runtime_fixtures/nested_runtime_main.di:" <<<"$output"
done

echo "nested runtime source-map differential case passed"

duplicate_runtime_fixture="tests/parser_runtime_fixtures/duplicate_runtime_main.di"
native_duplicate_runtime="$($diamond "$duplicate_runtime_fixture" 2>&1 || true)"
selfhost_duplicate_runtime="$(echo "$duplicate_runtime_fixture" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_duplicate_runtime" "$selfhost_duplicate_runtime"; do
    grep -Fq "at mapped_explode:2:10" <<<"$output"
    grep -Fq "at tests/parser_runtime_fixtures/duplicate_runtime_main.di:" <<<"$output"
    [[ "$(grep -Fc "at mapped_explode:" <<<"$output")" -eq 1 ]]
done

echo "duplicate-require source-map differential case passed"

package_runtime_fixture="tests/parser_runtime_fixtures/package_runtime_main.di"
native_package_runtime="$($diamond "$package_runtime_fixture" 2>&1 || true)"
selfhost_package_runtime="$(echo "$package_runtime_fixture" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_package_runtime" "$selfhost_package_runtime"; do
    grep -Fq "at package_explode:2:10" <<<"$output"
    grep -Fq "at tests/parser_runtime_fixtures/package_runtime_main.di:" <<<"$output"
done

echo "package runtime source-map differential case passed"

package_local_runtime_fixture="tests/parser_runtime_fixtures/package_local_runtime_main.di"
native_package_local_runtime="$($diamond "$package_local_runtime_fixture" 2>&1 || true)"
selfhost_package_local_runtime="$(echo "$package_local_runtime_fixture" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_package_local_runtime" "$selfhost_package_local_runtime"; do
    grep -Fq "at package_helper_explode:2:10" <<<"$output"
    grep -Fq "at package_runtime_mid:" <<<"$output"
    grep -Fq "at tests/parser_runtime_fixtures/package_local_runtime_main.di:" <<<"$output"
done

echo "package-local runtime source-map differential case passed"

duplicate_package_fixture="tests/parser_runtime_fixtures/duplicate_package_main.di"
native_duplicate_package="$($diamond "$duplicate_package_fixture" 2>&1 || true)"
selfhost_duplicate_package="$(echo "$duplicate_package_fixture" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_duplicate_package" "$selfhost_duplicate_package"; do
    grep -Fq "at package_explode:2:10" <<<"$output"
    [[ "$(grep -Fc "at package_explode:" <<<"$output")" -eq 1 ]]
done

echo "duplicate-package source-map differential case passed"

stress_package_runtime="$(echo "$package_local_runtime_fixture" | \
    DIAMOND_STRESS_GC=1 $diamond selfhost/parser_run.di 2>&1 || true)"
grep -Fq "at package_helper_explode:2:10" <<<"$stress_package_runtime"
grep -Fq "at package_runtime_mid:" <<<"$stress_package_runtime"
grep -Fq "at tests/parser_runtime_fixtures/package_local_runtime_main.di:" \
    <<<"$stress_package_runtime"

echo "stress-GC package source-map case passed"

interpolation_runtime_fixture="tests/parser_runtime_fixtures/interpolation_runtime_main.di"
native_interpolation_runtime="$($diamond "$interpolation_runtime_fixture" 2>&1 || true)"
selfhost_interpolation_runtime="$(echo "$interpolation_runtime_fixture" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_interpolation_runtime" "$selfhost_interpolation_runtime"; do
    grep -Fq "at interpolation_explode:2:" <<<"$output"
    grep -Fq "at tests/parser_runtime_fixtures/interpolation_runtime_main.di:" \
        <<<"$output"
done

echo "interpolation runtime source-map differential case passed"

stress_interpolation_runtime="$(echo "$interpolation_runtime_fixture" | \
    DIAMOND_STRESS_GC=1 $diamond selfhost/parser_run.di 2>&1 || true)"
grep -Fq "at interpolation_explode:2:" <<<"$stress_interpolation_runtime"
grep -Fq "at tests/parser_runtime_fixtures/interpolation_runtime_main.di:" \
    <<<"$stress_interpolation_runtime"

echo "stress-GC interpolation source-map case passed"

nested_interpolation_fixture="tests/parser_runtime_fixtures/nested_interpolation_main.di"
native_nested_interpolation="$($diamond "$nested_interpolation_fixture" 2>&1 || true)"
selfhost_nested_interpolation="$(echo "$nested_interpolation_fixture" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_nested_interpolation" "$selfhost_nested_interpolation"; do
    grep -Fq "at nested_interpolation_explode:2:19" <<<"$output"
    grep -Fq "at nested_interpolation_mid:1:66" <<<"$output"
    grep -Fq "at tests/parser_runtime_fixtures/nested_interpolation_main.di:" \
        <<<"$output"
done

echo "nested interpolation runtime source-map differential case passed"

duplicate_interpolation_fixture="tests/parser_runtime_fixtures/duplicate_interpolation_main.di"
native_duplicate_interpolation="$($diamond "$duplicate_interpolation_fixture" 2>&1 || true)"
selfhost_duplicate_interpolation="$(echo "$duplicate_interpolation_fixture" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_duplicate_interpolation" "$selfhost_duplicate_interpolation"; do
    grep -Fq "at interpolation_explode:2:19" <<<"$output"
    [[ "$(grep -Fc "at interpolation_explode:" <<<"$output")" -eq 1 ]]
done

echo "duplicate-require interpolation source-map differential case passed"

crlf_interpolation_dir="$(mktemp -d)"
trap 'rm -rf "$crlf_interpolation_dir"' EXIT
printf 'def crlf_interpolation_explode(value)\r\n  "value=#{value[4]}"\r\nend\r\n' \
    >"$crlf_interpolation_dir/runtime.di"
printf 'require "runtime"\r\ncrlf_interpolation_explode([1])\r\n' \
    >"$crlf_interpolation_dir/main.di"
native_crlf_interpolation="$($diamond "$crlf_interpolation_dir/main.di" 2>&1 || true)"
selfhost_crlf_interpolation="$(echo "$crlf_interpolation_dir/main.di" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_crlf_interpolation" "$selfhost_crlf_interpolation"; do
    grep -Fq "at crlf_interpolation_explode:2:19" <<<"$output"
    grep -Fq "at $crlf_interpolation_dir/main.di:" <<<"$output"
done
rm -rf "$crlf_interpolation_dir"
trap - EXIT

echo "CRLF interpolation runtime source-map differential case passed"

package_interpolation_runtime_fixture="tests/parser_runtime_fixtures/package_interpolation_runtime_main.di"
native_package_interpolation_runtime="$($diamond "$package_interpolation_runtime_fixture" 2>&1 || true)"
selfhost_package_interpolation_runtime="$(echo "$package_interpolation_runtime_fixture" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_package_interpolation_runtime" "$selfhost_package_interpolation_runtime"; do
    grep -Fq "at interpolation_pkg_explode:2:19" <<<"$output"
    grep -Fq "at $package_interpolation_runtime_fixture:" <<<"$output"
done

echo "package interpolation runtime source-map differential case passed"

package_local_interpolation_runtime_fixture="tests/parser_runtime_fixtures/package_local_interpolation_runtime_main.di"
native_package_local_interpolation_runtime="$($diamond "$package_local_interpolation_runtime_fixture" 2>&1 || true)"
selfhost_package_local_interpolation_runtime="$(echo "$package_local_interpolation_runtime_fixture" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_package_local_interpolation_runtime" \
        "$selfhost_package_local_interpolation_runtime"; do
    grep -Fq "at interpolation_local_helper_explode:2:19" <<<"$output"
    grep -Fq "at interpolation_local_pkg_mid:1:75" <<<"$output"
    grep -Fq "at $package_local_interpolation_runtime_fixture:" <<<"$output"
done

echo "package-local interpolation runtime source-map differential case passed"

duplicate_package_interpolation_fixture="tests/parser_runtime_fixtures/duplicate_package_interpolation_main.di"
native_duplicate_package_interpolation="$($diamond "$duplicate_package_interpolation_fixture" 2>&1 || true)"
selfhost_duplicate_package_interpolation="$(echo "$duplicate_package_interpolation_fixture" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_duplicate_package_interpolation" \
        "$selfhost_duplicate_package_interpolation"; do
    grep -Fq "at interpolation_pkg_explode:2:19" <<<"$output"
    [[ "$(grep -Fc "at interpolation_pkg_explode:" <<<"$output")" -eq 1 ]]
done

echo "duplicate-package interpolation source-map differential case passed"

crlf_interpolation_diagnostic_dir="$(mktemp -d)"
trap 'rm -rf "$crlf_interpolation_diagnostic_dir"' EXIT
printf 'value = 1\r\nother = 2\r\nbroken = "value #{other"\r\n' \
    >"$crlf_interpolation_diagnostic_dir/broken.di"
printf 'require "broken"\r\n' >"$crlf_interpolation_diagnostic_dir/main.di"
native_crlf_interpolation_diagnostic="$($diamond "$crlf_interpolation_diagnostic_dir/main.di" 2>&1 || true)"
selfhost_crlf_interpolation_diagnostic="$(echo "$crlf_interpolation_diagnostic_dir/main.di" | \
    $diamond selfhost/parser_check.di 2>&1 || true)"
for output in "$native_crlf_interpolation_diagnostic" "$selfhost_crlf_interpolation_diagnostic"; do
    grep -Fq "broken.di:3:10" <<<"$output"
done
rm -rf "$crlf_interpolation_diagnostic_dir"
trap - EXIT

echo "CRLF unterminated-interpolation diagnostic differential case passed"

redefine_method_dir="$(mktemp -d)"
trap 'rm -rf "$redefine_method_dir"' EXIT
cat > "$redefine_method_dir/main.di" <<'PROGRAM'
def make_replacement()
  def replacement()
    99
  end
  replacement
end

class Shape
  def area()
    1
  end
end
s = Shape.new()
Shape.redefine_method("area", make_replacement())
PROGRAM
native_redefine_method="$($diamond "$redefine_method_dir/main.di" 2>&1 || true)"
selfhost_redefine_method="$(echo "$redefine_method_dir/main.di" | \
    $diamond selfhost/parser_run.di 2>&1 || true)"
for output in "$native_redefine_method" "$selfhost_redefine_method"; do
    grep -Fq "redefine_method callable must be a method of 'Shape'" <<<"$output"
done
rm -rf "$redefine_method_dir"
trap - EXIT

echo "redefine_method dispatch differential case passed"

# The full success path (a `def` nested directly inside a method body,
# used as redefine_method's replacement): needs its own register-0-self
# reservation distinct from the enclosing method's, set up by
# compile_definition's self_offset handling and confirmed against
# tests/cases/legacy_0093.di/legacy_0094.di's own real-world use of this
# exact pattern. See docs/roadmap.md's self-hosting Phase 3 entry.
redefine_method_success_dir="$(mktemp -d)"
trap 'rm -rf "$redefine_method_success_dir"' EXIT
cat > "$redefine_method_success_dir/main.di" <<'PROGRAM'
class Shape
  def initialize(width, height)
    @width = width
    @height = height
  end
  def area()
    @width * @height
  end
  def self.square_area_patch()
    def square_area()
      @width * @width
    end
    square_area
  end
end
s = Shape.new(3, 4)
puts(s.area())
Shape.redefine_method("area", Shape.square_area_patch())
puts(s.area())
PROGRAM
native_redefine_method_success="$($diamond "$redefine_method_success_dir/main.di" 2>&1 || true)"
selfhost_redefine_method_success="$(echo "$redefine_method_success_dir/main.di" | \
    $diamond selfhost/parser_run.di 2>&1 | sed '$d' || true)"
if [[ "$selfhost_redefine_method_success" != "$native_redefine_method_success" ]]; then
    echo "redefine_method success-path differential mismatch" >&2
    echo "  expected: $native_redefine_method_success" >&2
    echo "  actual:   $selfhost_redefine_method_success" >&2
    exit 1
fi
rm -rf "$redefine_method_success_dir"
trap - EXIT

echo "redefine_method success-path differential case passed"

# Bootstrap checks (self-parse, self-run) now live in
# tests/self_host_smoke.sh, which runs as part of `make test-all` even
# though this full file doesn't -- see this file's own header comment.
