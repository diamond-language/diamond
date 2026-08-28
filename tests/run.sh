#!/usr/bin/env bash
set -euo pipefail

bash tests/collection_relay_contracts.sh

diamond=./build/diamond
diamond_abs="$(realpath "$diamond")"
run_cases_abs="$(realpath ./build/run_cases)"

actual="$($diamond --version)"
[[ "$actual" == "diamond 0.1.0" ]] || {
    echo "unexpected version output: $actual" >&2
    exit 1
}

[[ "$($diamond -v)" == "diamond 0.1.0" ]] || {
    echo "unexpected short version output" >&2
    exit 1
}

for help_flag in -h --help; do
    help_output="$($diamond "$help_flag")"
    [[ "$help_output" == *"Usage: diamond [OPTIONS] [FILE [ARGS...]]"* ]]
    [[ "$help_output" == *"-h, --help"* ]]
    [[ "$help_output" == *"-v, --version"* ]]
done

# Once program input has started, option-looking values belong to the script.
actual="$($diamond -e 'ARGV' -h --version)"
[[ "$actual" == "[-h, --version]" ]]

script_flags="$(mktemp)"
printf 'ARGV\n' >"$script_flags"
actual="$($diamond "$script_flags" -h --version)"
rm -f "$script_flags"
[[ "$actual" == "[-h, --version]" ]]

[[ "$("$diamond" -e $'20-2')" == 18 ]]
[[ "$("$diamond" -e $'20*2')" == 40 ]]
[[ "$("$diamond" -e $'20/2')" == 10 ]]
[[ "$("$diamond" -e $'20%3')" == 2 ]]

# ARGV: real trailing command-line arguments -- needs actual argv control,
# so (like Process.run and debugger() above/below) this lives here rather
# than in tests/cases/*.di, where run_cases.c always passes an empty ARGV.
actual="$("$diamond" -e $'puts(ARGV.length())\nputs(ARGV[0])\nputs(ARGV[1])' foo bar)"
[[ "$actual" == $'2\nfoo\nbar\nnil' ]]

actual="$("$diamond" -e 'ARGV')"
[[ "$actual" == "[]" ]]

file_argv="$(mktemp -d)/argv.di"
echo 'ARGV' >"$file_argv"
actual="$("$diamond" "$file_argv" one two three)"
[[ "$actual" == "[one, two, three]" ]]

actual="$("$diamond" --dump-bytecode -e 'puts(ARGV[0])' extra)"
[[ "$(tail -2 <<<"$actual" | head -1)" == "extra" ]]

# ENV: real environment-variable control, same reasoning as ARGV above.
actual="$(FOO_DIAMOND_TEST_VAR=hello "$diamond" -e 'ENV["FOO_DIAMOND_TEST_VAR"]')"
[[ "$actual" == "hello" ]]

actual="$("$diamond" -e 'ENV["DIAMOND_NONEXISTENT_VAR_XYZ"] == nil')"
[[ "$actual" == "true" ]]

error_file="$(mktemp)"
if "$diamond" -e '1 + )' 2>"$error_file"; then
    echo "invalid source unexpectedly succeeded" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q "error: expected expression" "$error_file"
rm -f "$error_file"

[[ "$("$diamond" -e $'join = [1, 2].join\njoin(",")')" == "1,2" ]]

if "$diamond" -e '1 / 0' >/dev/null 2>&1; then
    echo "division by zero unexpectedly succeeded" >&2
    exit 1
fi

if "$diamond" -e 'missing + 1' >/dev/null 2>&1; then
    echo "undefined local unexpectedly succeeded" >&2
    exit 1
fi

if "$diamond" -e $'def one(a)\n  a\nend\none()' >/dev/null 2>&1; then
    echo "wrong function arity unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'class Child < Missing\nend' >/dev/null 2>&1; then
    echo "undefined superclass unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'class Root\n  def value()\n    super()\n  end\nend' >/dev/null 2>&1; then
    echo "super without a superclass unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'def typed(x: String)\n  x\nend\ntyped(42)' >/dev/null 2>&1; then
    echo "typed parameter accepted wrong runtime type" >&2
    exit 1
fi

if "$diamond" -e $'def wrong() -> Bool\n  42\nend\nwrong()' >/dev/null 2>&1; then
    echo "typed return accepted wrong runtime type" >&2
    exit 1
fi

if "$diamond" -e $'def mystery(x: Missing)\n  x\nend' >/dev/null 2>&1; then
    echo "unknown annotation unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'def accept(x: Int | Nil)\n x\nend\naccept("bad")' >/dev/null 2>&1; then
    echo "nilable parameter accepted wrong non-nil type" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e $'def accept(x: Int | String)\n x\nend\naccept(true)' \
    >/dev/null 2>"$error_file"; then
    echo "general union accepted an unrelated type" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'expected Int | String, got Bool' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'def ints(values: Array[Int])\n values\nend\nints([1, "bad"])' \
    >/dev/null 2>"$error_file"; then
    echo "Array[Int] accepted an existing String element" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'expected Array\[Int\], got Array' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'def scores(values: Hash[String, Int])\n values\nend\nscores({"ok": "bad"})' \
    >/dev/null 2>"$error_file"; then
    echo "Hash[String, Int] accepted an existing String value" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'expected Hash\[String, Int\], got Hash' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'class Foo\n def bar(a, b)\n  a + b\n end\nend\nFoo.new().bar(1)' >/dev/null 2>"$error_file"; then
    echo "dynamic dispatch with too few arguments unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: wrong number of arguments' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'class Foo\n def bar(a, b)\n  a + b\n end\nend\nFoo.new().bar(1, 2, 3)' >/dev/null 2>"$error_file"; then
    echo "dynamic dispatch with too many arguments unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: wrong number of arguments' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'class Foo\n def bar(a, b)\n  a + b\n end\nend\nf = Foo.new()\nf.bar(1, 2)\nf.bar(1, 2)\nf.bar(1, 2)\nf.bar(1)' \
    >/dev/null 2>"$error_file"; then
    echo "monomorphic dispatch with wrong arity unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: wrong number of arguments' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'class Foo\n def bar(a, b)\n  a + b\n end\nend\ndef inner(x)\n x.bar(1)\nend\ndef outer(x)\n inner(x)\nend\nouter(Foo.new())' \
    >/dev/null 2>"$error_file"; then
    echo "nested dynamic dispatch arity error unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: wrong number of arguments' "$error_file"
grep -q 'at inner:7:' "$error_file"
grep -q 'at outer:10:' "$error_file"
rm -f "$error_file"

actual="$("$diamond" --dump-bytecode -e $'def head(values: Array[Int]) -> Int\n item = values[0]\n item\nend\nhead([42])')"
head_dump="$(sed -n '/^== head ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$head_dump")" == "1" ]]
grep -q 'INDEX_GET' <<<"$head_dump"

actual="$("$diamond" --dump-bytecode -e $'def nested(values: Array[Array[Int]]) -> Array[Int]\n values[0]\nend\nnested([[42]])')"
nested_dump="$(sed -n '/^== nested ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$nested_dump")" == "1" ]]

actual="$("$diamond" --dump-bytecode -e $'def lookup(values: Hash[String, Int]) -> Int | Nil\n values["answer"]\nend\nlookup({"answer": 42})')"
lookup_dump="$(sed -n '/^== lookup ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$lookup_dump")" == "1" ]]

if "$diamond" -e $'def unsafe(values: Hash[String, Int]) -> Int\n values["missing"]\nend' \
    >/dev/null 2>&1; then
    echo "hash lookup incorrectly omitted its possible Nil result" >&2
    exit 1
fi

actual="$("$diamond" --dump-bytecode -e $'def present(value: String | Nil) -> String\n if value != nil\n  value\n else\n  "fallback"\n end\nend\npresent(nil)')"
present_dump="$(sed -n '/^== present ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$present_dump")" == "1" ]]

# Control-flow joins synthesize reusable union metadata. Each function needs
# only its Bool parameter check; its declared return union is already proven.
actual="$("$diamond" --dump-bytecode -e $'class FlowDog\nend\nclass FlowCat\nend\ndef joined_if(flag: Bool) -> FlowDog | FlowCat\n value = if flag\n  FlowDog.new()\n else\n  FlowCat.new()\n end\n value\nend\njoined_if(true)')"
joined_if_dump="$(sed -n '/^== joined_if ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$joined_if_dump")" == "1" ]]

actual="$("$diamond" --dump-bytecode -e $'class FlowDog\nend\nclass FlowCat\nend\ndef joined_assignment(flag: Bool) -> FlowDog | FlowCat\n value = FlowDog.new()\n if flag\n  value = FlowDog.new()\n else\n  value = FlowCat.new()\n end\n value\nend\njoined_assignment(false)')"
joined_assignment_dump="$(sed -n '/^== joined_assignment ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$joined_assignment_dump")" == "1" ]]

actual="$("$diamond" --dump-bytecode -e $'class FlowDog\nend\nclass FlowCat\nend\ndef joined_ternary(flag: Bool) -> FlowDog | FlowCat\n flag ? FlowDog.new() : FlowCat.new()\nend\njoined_ternary(true)')"
joined_ternary_dump="$(sed -n '/^== joined_ternary ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$joined_ternary_dump")" == "1" ]]

actual="$("$diamond" --dump-bytecode -e $'class FlowDog\nend\nclass FlowCat\nend\ndef joined_case(value: Int) -> FlowDog | FlowCat\n case value\n when 1\n  FlowDog.new()\n else\n  FlowCat.new()\n end\nend\njoined_case(1)')"
joined_case_dump="$(sed -n '/^== joined_case ==$/,$p' <<<"$actual")"
grep -q 'CASE_MATCH' <<<"$joined_case_dump"
[[ "$(grep -c 'CHECK_TYPE' <<<"$joined_case_dump")" == "1" ]]

actual="$("$diamond" --dump-bytecode -e $'class FlowDog\nend\nclass FlowCat\nend\ndef joined_while(flag: Bool) -> FlowDog | FlowCat\n value = FlowCat.new()\n while flag\n  value = FlowDog.new()\n  break\n end\n value\nend\njoined_while(false)')"
joined_while_dump="$(sed -n '/^== joined_while ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$joined_while_dump")" == "1" ]]

actual="$("$diamond" --dump-bytecode -e $'class FlowDog\nend\nclass FlowCat\nend\ndef joined_break(flag: Bool) -> FlowDog | FlowCat\n loop\n  if flag\n   break FlowDog.new()\n  else\n   break FlowCat.new()\n  end\n end\nend\njoined_break(true)')"
joined_break_dump="$(sed -n '/^== joined_break ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$joined_break_dump")" == "1" ]]

actual="$("$diamond" --dump-bytecode -e $'def lookup(values: Hash[String, Int]) -> Int\n value = values["answer"]\n if value == nil\n  0\n else\n  value\n end\nend\nlookup({"answer": 42})')"
lookup_dump="$(sed -n '/^== lookup ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$lookup_dump")" == "1" ]]

actual="$("$diamond" --dump-bytecode -e $'def unstable(value: String | Nil, flag: Bool) -> String\n if flag\n  value = "changed"\n end\n value\nend\nunstable("ok", false)')"
unstable_dump="$(sed -n '/^== unstable ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$unstable_dump")" == "3" ]]

actual="$("$diamond" --dump-bytecode -e $'def text(value: String | Int) -> String\n if value is String\n  value\n else\n  "number"\n end\nend\ntext(42)')"
text_dump="$(sed -n '/^== text ==$/,$p' <<<"$actual")"
grep -q 'IS_TYPE.*String' <<<"$text_dump"
[[ "$(grep -c 'CHECK_TYPE' <<<"$text_dump")" == "1" ]]

actual="$("$diamond" --dump-bytecode -e $'def number(value: String | Int) -> Int\n if value is String\n  0\n else\n  value\n end\nend\nnumber(42)')"
number_dump="$(sed -n '/^== number ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$number_dump")" == "1" ]]

actual="$("$diamond" --dump-bytecode -e $'def compound(value: String | Int, flag: Bool) -> String\n if value is String && flag\n  value\n else\n  "fallback"\n end\nend\ncompound("ok", true)')"
compound_dump="$(sed -n '/^== compound ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$compound_dump")" == "2" ]]

if "$diamond" -e '42 is Missing' >/dev/null 2>&1; then
    echo "is accepted an unknown type" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e $'def run()\n def wrong()\n  42\n end\n array_map([], wrong)\nend\nrun()' \
    >/dev/null 2>"$error_file"; then
    echo "Callable[1] accepted a zero-arity closure" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'expected Callable\[1\], got Callable' "$error_file"
rm -f "$error_file"

actual="$("$diamond" --dump-bytecode -e '42')"
array_each_dump="$(sed -n '/^== array_each ==$/,/^== /p' <<<"$actual")"
grep -q 'CHECK_TYPE.*Callable\[1\]' <<<"$array_each_dump"

if "$diamond" -e $'def invalid(callback: Callable[17])\n callback\nend' \
    >/dev/null 2>&1; then
    echo "oversized Callable arity unexpectedly compiled" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e $'def typed(x: String | Nil)\n x\nend\ntyped(42)' \
    >/dev/null 2>"$error_file"; then
    echo "detailed primitive mismatch unexpectedly succeeded" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'runtime error: expected String | Nil, got Int' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'class Animal\nend\nclass Rock\nend\ndef adopt(x: Animal)\n x\nend\nadopt(Rock.new())' \
    >/dev/null 2>"$error_file"; then
    echo "detailed nominal mismatch unexpectedly succeeded" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'runtime error: expected Animal, got Rock' "$error_file"
rm -f "$error_file"

actual="$("$diamond" --dump-bytecode -e $'def literal() -> Int\n  42\nend\nliteral()')"
literal_dump="$(sed -n '/^== literal ==$/,$p' <<<"$actual")"
if grep -q 'CHECK_TYPE' <<<"$literal_dump"; then
    echo "provably redundant return guard was not eliminated" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e $'def wrong() -> Bool\n  42\nend\nwrong()' \
    >/dev/null 2>"$error_file"; then
    echo "provably wrong return unexpectedly compiled" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'expression cannot satisfy type annotation' "$error_file"
rm -f "$error_file"

actual="$("$diamond" --dump-bytecode -e $'def absent() -> String | Nil\n  nil\nend\nabsent()')"
absent_dump="$(sed -n '/^== absent ==$/,$p' <<<"$actual")"
if grep -q 'CHECK_TYPE' <<<"$absent_dump"; then
    echo "provably nil return retained a nilable guard" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e '[1][2]' >/dev/null 2>"$error_file"; then
    echo "out-of-bounds array read unexpectedly succeeded" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'index 2 out of bounds for Array of length 1' "$error_file"
rm -f "$error_file"

if "$diamond" -e $'def typed(value: Array)\n value\nend\ntyped("not an array")' \
    >/dev/null 2>&1; then
    echo "Array annotation accepted a string" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e $'values = [1]\nvalues[-1] = 2' >/dev/null 2>"$error_file"; then
    echo "negative indexed assignment unexpectedly succeeded" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'index -1 out of bounds for Array of length 1' "$error_file"
rm -f "$error_file"

if "$diamond" -e $'def bad(flag) -> Int\n  if flag\n    return "bad"\n  end\n  0\nend' \
    >/dev/null 2>&1; then
    echo "statically invalid explicit return unexpectedly compiled" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e $'def checked(value) -> Int\n  return value\nend\nchecked("bad")' \
    >/dev/null 2>"$error_file"; then
    echo "dynamic explicit return bypassed its guard" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'expected Int, got String' "$error_file"
rm -f "$error_file"

if "$diamond" -e 'return 1' >/dev/null 2>&1; then
    echo "top-level return unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e 'break' >/dev/null 2>&1; then
    echo "top-level break unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e 'next' >/dev/null 2>&1; then
    echo "top-level next unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'while true\n  next 42\nend' >/dev/null 2>&1; then
    echo "valued next unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e '1__000' >/dev/null 2>&1; then
    echo "malformed numeric separators unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e '1_' >/dev/null 2>&1; then
    echo "trailing numeric separator unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'def typed(value: Hash)\n value\nend\ntyped([])' \
    >/dev/null 2>&1; then
    echo "Hash annotation accepted an Array" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" tests/cases/stack_trace.di >/dev/null 2>"$error_file"; then
    echo "runtime stack trace unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: division by zero' "$error_file"
grep -q 'at divide:3:' "$error_file"
grep -q 'at invoke:8:' "$error_file"
grep -q 'at tests/cases/stack_trace.di:11:' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" tests/cases/stack_trace_through_ensure.di >/dev/null 2>"$error_file"; then
    echo "runtime stack trace through ensure unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: division by zero' "$error_file"
grep -q 'at divide:7:' "$error_file"
grep -q 'at invoke:15:' "$error_file"
grep -q 'at tests/cases/stack_trace_through_ensure.di:19:' "$error_file"
rm -f "$error_file"

actual="$("$diamond" --dump-bytecode -e '40 + 2')"
grep -Eq '^[0-9]{4} +1:[0-9]+ +ADD' <<<"$actual"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC=1 "$diamond" tests/cases/inline_cache.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'inline caches: 4 hits, 1 misses' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC_FAST=1 "$diamond" tests/cases/inline_cache.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'monomorphic dispatches: 3' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC_REWRITES=1 "$diamond" tests/cases/inline_cache.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'direct dispatch rewrites: 1' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_REPEAT=2 DIAMOND_TRACE_IC_EACH_RUN=1 "$diamond" \
    tests/cases/mono_deopt.di 2>"$error_file")"
[[ "$actual" == "82" ]]
grep -q 'run 1: inline caches: 1 hits, 2 misses, rewrites: 1' "$error_file"
grep -q 'run 2: inline caches: 1 hits, 2 misses, rewrites: 1' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_REPEAT=2 DIAMOND_INVALIDATE_IC_EACH_RUN=1 \
    DIAMOND_TRACE_IC_EACH_RUN=1 "$diamond" tests/cases/inline_cache.di \
    2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'run 1: inline caches: 4 hits, 1 misses, rewrites: 1' "$error_file"
grep -q 'run 2: inline caches: 4 hits, 1 misses, rewrites: 1' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_REPEAT=2 DIAMOND_TRACE_IC_REWRITES=1 "$diamond" \
    tests/cases/mono_deopt.di 2>"$error_file")"
[[ "$actual" == "82" ]]
grep -q 'direct dispatch rewrites: 1' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_REPEAT=2 DIAMOND_TRACE_IC_REWRITES=1 "$diamond" \
    tests/cases/inline_cache.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'direct dispatch rewrites: 1' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_IC_MONO_THRESHOLD=100 DIAMOND_TRACE_IC_REWRITES=1 \
    "$diamond" tests/cases/inline_cache.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'direct dispatch rewrites: 0' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC_PROBES=1 "$diamond" tests/cases/inline_cache.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'method cache probes: 1' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_IC_MONO_THRESHOLD=2 DIAMOND_TRACE_IC_FAST=1 "$diamond" \
    tests/cases/inline_cache.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'monomorphic dispatches: 2' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC=1 "$diamond" tests/cases/polymorphic_cache.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'inline caches: 2 hits, 2 misses' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC_PROBES=1 "$diamond" tests/cases/polymorphic_cache.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'method cache probes: 4' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC_REWRITES=1 "$diamond" tests/cases/polymorphic_cache.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'direct dispatch rewrites: 0' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC_SITES=1 "$diamond" tests/cases/inherited_cache.di 2>"$error_file")"
[[ "$actual" == "160" ]]
grep -Eq 'inline cache site\[[0-9]+\]: 2 hits, 2 misses, 2 classes' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC_PROBES=1 "$diamond" tests/cases/dispatch_benchmark.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'method cache probes: 1' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_IC_MONO_THRESHOLD=100 DIAMOND_TRACE_IC_PROBES=1 \
    "$diamond" tests/cases/dispatch_benchmark.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'method cache probes: 99' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_IC_MONO_THRESHOLD=100 DIAMOND_REPEAT=2 \
    DIAMOND_TRACE_IC_EACH_RUN=1 "$diamond" tests/cases/dispatch_benchmark.di \
    2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'run 1: inline caches: 99 hits, 1 misses, rewrites: 0' "$error_file"
grep -q 'run 2: inline caches: 99 hits, 1 misses, rewrites: 0' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC_FAST=1 "$diamond" tests/cases/mixed_dispatch_workload.di 2>"$error_file")"
[[ "$actual" == "1500" ]]
grep -q 'monomorphic dispatches: 498' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC_REWRITES=1 "$diamond" tests/cases/mixed_dispatch_workload.di 2>"$error_file")"
[[ "$actual" == "1500" ]]
grep -q 'direct dispatch rewrites: 1' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC_PROBES=1 "$diamond" tests/cases/mixed_dispatch_workload.di 2>"$error_file")"
[[ "$actual" == "1500" ]]
grep -q 'method cache probes: 1000' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_QUICKEN_THRESHOLD=3 DIAMOND_IC_MONO_THRESHOLD=4 \
    DIAMOND_TRACE_IC_POLICY=1 "$diamond" -e '1+2' 2>"$error_file")"
[[ "$actual" == "3" ]]
grep -q 'dispatch policy: quicken threshold 3, monomorphic threshold 4' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC_POLICY=1 "$diamond" -e '1+2' 2>"$error_file")"
[[ "$actual" == "3" ]]
grep -q 'dispatch policy: quicken threshold 1, monomorphic threshold 1' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC_REWRITES=1 "$diamond" tests/cases/mono_deopt.di 2>"$error_file")"
[[ "$actual" == "82" ]]
grep -q 'direct dispatch rewrites: 1' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC=1 "$diamond" tests/cases/mono_deopt.di 2>"$error_file")"
[[ "$actual" == "82" ]]
grep -q 'inline caches: 1 hits, 2 misses' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC=1 "$diamond" tests/cases/inherited_cache.di 2>"$error_file")"
[[ "$actual" == "160" ]]
grep -q 'inline caches: 2 hits, 2 misses' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_SHAPES=1 "$diamond" tests/cases/runtime_shapes.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'shape transitions: 1' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_FIELDS=1 "$diamond" tests/cases/runtime_shapes.di 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'field caches: 2 hits, 4 misses' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if DIAMOND_STRESS_GC=1 "$diamond" tests/cases/raise_stack.di >/dev/null 2>"$error_file"; then
    echo "raised value unexpectedly returned" >&2
    exit 1
fi
grep -q 'runtime error: uncaught exception: diamond cracked' "$error_file"
grep -q 'at fail:2:' "$error_file"
grep -q 'at call_fail:6:' "$error_file"
grep -q 'at tests/cases/raise_stack.di:9:' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'def depth(n)\n if n <= 0\n  0\n else\n  depth(n - 1) + 1\n end\nend\ndepth(5000)' \
    >/dev/null 2>"$error_file"; then
    echo "deep recursion unexpectedly completed" >&2
    exit 1
fi
grep -q 'runtime error: call stack overflow' "$error_file"
rm -f "$error_file"

actual="$($diamond --dump-bytecode -e 'yield' 2>/dev/null || true)"
grep -Eq 'YIELD +r[0-9]+, r[0-9]+' <<<"$actual"

actual="$($diamond --dump-bytecode -e $'def f()\n x = yield(1) + 1\n x\nend' 2>/dev/null || true)"
grep -Eq 'YIELD +r[0-9]+, r[0-9]+' <<<"$actual"

error_file="$(mktemp)"
if "$diamond" -e $'def f()\n x = yield(1) + 1\n x\nend\nf()' >/dev/null 2>"$error_file"; then
    echo "yield as a sub-expression outside a fiber unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: yield outside a fiber' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'class Foo\n def bar(a)\n  a\n end\n def self.make_patch()\n  def replacement(a, b)\n   a + b\n  end\n  replacement\n end\nend\nFoo.redefine_method("bar", Foo.make_patch())' \
    >/dev/null 2>"$error_file"; then
    echo "redefine_method with mismatched arity unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: wrong number of arguments' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'class Foo\n def bar(a)\n  a\n end\n def self.make_patch()\n  def replacement(a)\n   a\n  end\n  replacement\n end\nend\nFoo.redefine_method("nonexistent", Foo.make_patch())' \
    >/dev/null 2>"$error_file"; then
    echo "redefine_method of an unknown method name unexpectedly succeeded" >&2
    exit 1
fi
grep -q "class 'Foo' has no method 'nonexistent' to redefine" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'class Foo\n def bar(a)\n  a\n end\n def self.make_patch(extra)\n  def replacement(a)\n   a + extra\n  end\n  replacement\n end\nend\nFoo.redefine_method("bar", Foo.make_patch(1))' \
    >/dev/null 2>"$error_file"; then
    echo "redefine_method with a capturing closure unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'redefine_method callable must not capture any variables' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'class Foo\n def bar(a)\n  a\n end\nend\nclass Baz\n def bar(a)\n  a\n end\n def self.make_patch()\n  def replacement(a)\n   a\n  end\n  replacement\n end\nend\nFoo.redefine_method("bar", Baz.make_patch())' \
    >/dev/null 2>"$error_file"; then
    echo "redefine_method with a callable from a different class unexpectedly succeeded" >&2
    exit 1
fi
grep -q "redefine_method callable must be a method of 'Foo'" "$error_file"
rm -f "$error_file"

if "$diamond" -e $'begin\n raise 1\nrescue error: Int |\n 0\nend' \
    >/dev/null 2>&1; then
    echo "trailing rescue union unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'def wrong() -> String = 42\nwrong()' >/dev/null 2>&1; then
    echo "endless method bypassed its return contract" >&2
    exit 1
fi

if "$diamond" -e $'def missing() =' >/dev/null 2>&1; then
    echo "endless method accepted a missing expression" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode -e $'def answer() -> Int = 42\nanswer()')"
grep -A3 '== answer ==' <<<"$actual" | grep -q 'RETURN'

if "$diamond" -e $'def invalid(optional = 1, required) = required' \
    >/dev/null 2>&1; then
    echo "required parameter followed a default parameter" >&2
    exit 1
fi

if "$diamond" -e $'"missing #{42"' >/dev/null 2>&1; then
    echo "unterminated interpolation unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'"empty #{}"' >/dev/null 2>&1; then
    echo "empty interpolation unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond tests/multifile/main.di)"
[[ "$actual" == "[42, Hello, world]" ]]

actual="$($diamond tests/multifile/load_once_main.di)"
[[ "$actual" == "42" ]]

# Module and class reopening across files (see docs/syntax.md's
# "Classes" section) -- reopen_a.di/reopen_b.di each open `module
# Shared; class Widget; ... end; end` with a different method, and
# reopen_a.di's own `Consumer` forward-references `Extra`, declared only
# in reopen_b.di -- exercises reopening and the declaration-discovery
# pass together, cross-file, the actual motivating scenario for both.
actual="$($diamond tests/multifile/reopen_main.di)"
[[ "$actual" == "[a, b, extra-hi]" ]]

if "$diamond" tests/multifile/cycle_a.di >/dev/null 2>&1; then
    echo "circular require unexpectedly loaded" >&2
    exit 1
fi
cycle_error="$(mktemp)"
if "$diamond" tests/multifile/cycle_a.di >/dev/null 2>"$cycle_error"; then
    rm -f "$cycle_error"
    exit 1
fi
grep -q "circular require involving" "$cycle_error"
grep -q "required from .*cycle_b.di:1" "$cycle_error"
rm -f "$cycle_error"

actual="$($diamond tests/multifile/broken_main.di 2>&1 || true)"
grep -q 'tests/multifile/broken.di:3:16' <<<"$actual"
actual="$($diamond tests/multifile/nested_broken_main.di 2>&1 || true)"
grep -q 'tests/multifile/broken.di:3:16' <<<"$actual"
actual="$($diamond tests/multifile/nested_missing_main.di 2>&1 || true)"
grep -q 'nested_missing_mid.di:1: cannot require' <<<"$actual"
actual="$($diamond --dump-bytecode tests/multifile/broken_main.di 2>&1 || true)"
grep -q 'tests/multifile/broken.di:3:16' <<<"$actual"
actual="$($diamond tests/multifile/eof_broken_main.di 2>&1 || true)"
grep -q 'tests/multifile/eof_broken.di:' <<<"$actual"
! grep -q '^#line' <<<"$actual"
actual="$($diamond tests/multifile/eof_multiline_main.di 2>&1 || true)"
grep -q 'tests/multifile/eof_multiline_broken.di:2:1' <<<"$actual"
! grep -q '^#line' <<<"$actual"
crlf_dir="$(mktemp -d)"
printf 'if true\r\n  1\r\n' >"$crlf_dir/broken.di"
printf 'require "broken"\r\n' >"$crlf_dir/main.di"
actual="$($diamond "$crlf_dir/main.di" 2>&1 || true)"
grep -q 'broken.di:2:1' <<<"$actual"
printf 'def explode(value)\r\n  value[4]\r\nend\r\n' >"$crlf_dir/runtime.di"
printf 'require "runtime"\r\nexplode([1])\r\n' >"$crlf_dir/runtime_main.di"
actual="$($diamond "$crlf_dir/runtime_main.di" 2>&1 || true)"
grep -q 'at explode:2:10' <<<"$actual"
rm -rf "$crlf_dir"
crlf_error="$(mktemp)"
if "$diamond" -e $'1 + )\r\n' >/dev/null 2>"$crlf_error"; then
    rm -f "$crlf_error"
    exit 1
fi
grep -q -- '-e:1:' "$crlf_error"
rm -f "$crlf_error"
depth_dir="$(mktemp -d)"
for depth in $(seq 0 128); do
    if [[ "$depth" -eq 128 ]]; then
        printf '42\n' >"$depth_dir/f$depth.di"
    else
        next=$((depth+1))
        printf 'require "f%s"\n' "$next" >"$depth_dir/f$depth.di"
    fi
done
depth_error="$(mktemp)"
if "$diamond" "$depth_dir/f0.di" >/dev/null 2>"$depth_error"; then
    echo "require-depth limit unexpectedly succeeded" >&2
    rm -rf "$depth_dir" "$depth_error"
    exit 1
fi
grep -q "require nesting limit reached" "$depth_error"
rm -rf "$depth_dir" "$depth_error"
files_dir="$(mktemp -d)"
: >"$files_dir/main.di"
for file_index in $(seq 0 127); do
    printf '42\n' >"$files_dir/f$file_index.di"
    printf 'require "f%s"\n' "$file_index" >>"$files_dir/main.di"
done
files_error="$(mktemp)"
if "$diamond" "$files_dir/main.di" >/dev/null 2>"$files_error"; then
    echo "loaded-file limit unexpectedly succeeded" >&2
    rm -rf "$files_dir" "$files_error"
    exit 1
fi
grep -q "loaded-file limit reached" "$files_error"
rm -rf "$files_dir" "$files_error"

actual="$($diamond -e $'require "tests/multifile/math"\ndouble(21)')"
[[ "$actual" == "42" ]]
runtime_stack="$($diamond -e $'require "tests/multifile/math"\ndouble("bad")' 2>&1 || true)"
grep -q 'at double:' <<<"$runtime_stack"
runtime_stack="$($diamond tests/multifile/runtime_broken_main.di 2>&1 || true)"
grep -q 'at explode:' <<<"$runtime_stack"
grep -q 'at explode:2:10' <<<"$runtime_stack"
grep -q 'at tests/multifile/runtime_broken_main.di:' <<<"$runtime_stack"
runtime_stack="$(DIAMOND_STRESS_GC=1 $diamond tests/multifile/runtime_broken_main.di 2>&1 || true)"
grep -q 'at explode:' <<<"$runtime_stack"
actual="$($diamond -e $'require "tests/multifile/math"\r\ndouble(21)\r\n')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'require "./tests/multifile/math.di"\ndouble(21)')"
[[ "$actual" == "42" ]]

actual="$($diamond --dump-bytecode tests/multifile/main.di)"
grep -q '== double ==' <<<"$actual"
grep -q '== greet ==' <<<"$actual"

pkg_dir="$(mktemp -d)"
mkdir -p "$pkg_dir/cuts/greeter/lib"
printf 'def greet(name)\n  "hi, " + name\nend\n' >"$pkg_dir/cuts/greeter/lib/greeter.di"
printf 'require_cut "greeter"\ngreet("world")\n' >"$pkg_dir/main.di"
actual="$(cd "$pkg_dir" && "$diamond_abs" main.di)"
[[ "$actual" == "hi, world" ]]

# A relative file of the same name never competes with a cut -- require
# and require_cut are two entirely separate, unambiguous mechanisms, so a
# same-named cuts/greeter directory sitting alongside greeter.di changes
# nothing about what plain `require "greeter"` resolves to.
printf 'def greet(name)\n  "relative: " + name\nend\n' >"$pkg_dir/greeter.di"
printf 'require "greeter"\ngreet("world")\n' >"$pkg_dir/relative_main.di"
actual="$(cd "$pkg_dir" && "$diamond_abs" relative_main.di)"
[[ "$actual" == "relative: world" ]]
rm -f "$pkg_dir/greeter.di" "$pkg_dir/relative_main.di"

mkdir -p "$pkg_dir/cuts/pkg/lib"
printf 'def helper_fn()\n  "helped"\nend\n' >"$pkg_dir/cuts/pkg/lib/helper.di"
printf 'require "helper"\ndef pkg_fn()\n  helper_fn()\nend\n' >"$pkg_dir/cuts/pkg/lib/pkg.di"
printf 'require_cut "pkg"\npkg_fn()\n' >"$pkg_dir/pkg_main.di"
actual="$(cd "$pkg_dir" && "$diamond_abs" pkg_main.di)"
[[ "$actual" == "helped" ]]

pkg_missing_error="$(mktemp)"
printf 'require_cut "no_such_cut"\n' >"$pkg_dir/missing_main.di"
if (cd "$pkg_dir" && "$diamond_abs" missing_main.di) >/dev/null 2>"$pkg_missing_error"; then
    echo "nonexistent cut require_cut unexpectedly succeeded" >&2
    rm -rf "$pkg_dir" "$pkg_missing_error"
    exit 1
fi
grep -q "cannot require_cut 'no_such_cut'" "$pkg_missing_error"

slash_error="$(mktemp)"
printf 'require_cut "sub/greeter"\n' >"$pkg_dir/slash_main.di"
if (cd "$pkg_dir" && "$diamond_abs" slash_main.di) >/dev/null 2>"$slash_error"; then
    echo "require_cut with a slash unexpectedly succeeded" >&2
    rm -rf "$pkg_dir" "$pkg_missing_error" "$slash_error"
    exit 1
fi
grep -q "require_cut path must be a bare cut name" "$slash_error"
rm -rf "$pkg_dir" "$pkg_missing_error" "$slash_error"

manifest_dir="$(mktemp -d)"
mkdir -p "$manifest_dir/cuts/greeter/lib"
printf 'def greet(name)\n  "hi, " + name\nend\n' >"$manifest_dir/cuts/greeter/lib/greeter.di"
printf 'require_cut "greeter"\ngreet("world")\n' >"$manifest_dir/main.di"

printf '{"name": "greeter", "version": "0.1.0"}\n' >"$manifest_dir/cuts/greeter/diamond.cut"
actual="$(cd "$manifest_dir" && "$diamond_abs" main.di)"
[[ "$actual" == "hi, world" ]]
rm -f "$manifest_dir/cuts/greeter/diamond.cut"

actual="$(cd "$manifest_dir" && "$diamond_abs" main.di)"
[[ "$actual" == "hi, world" ]]

manifest_error="$(mktemp)"
printf '{"name": "wrong_name"}\n' >"$manifest_dir/cuts/greeter/diamond.cut"
if (cd "$manifest_dir" && "$diamond_abs" main.di) >/dev/null 2>"$manifest_error"; then
    echo "mismatched manifest name unexpectedly succeeded" >&2
    rm -rf "$manifest_dir" "$manifest_error"
    exit 1
fi
grep -q "declares name 'wrong_name', expected 'greeter'" "$manifest_error"

printf '42\n' >"$manifest_dir/cuts/greeter/diamond.cut"
if (cd "$manifest_dir" && "$diamond_abs" main.di) >/dev/null 2>"$manifest_error"; then
    echo "non-Hash manifest unexpectedly succeeded" >&2
    rm -rf "$manifest_dir" "$manifest_error"
    exit 1
fi
grep -q "must evaluate to a Hash" "$manifest_error"

printf '{"version": "0.1.0"}\n' >"$manifest_dir/cuts/greeter/diamond.cut"
if (cd "$manifest_dir" && "$diamond_abs" main.di) >/dev/null 2>"$manifest_error"; then
    echo "manifest missing name key unexpectedly succeeded" >&2
    rm -rf "$manifest_dir" "$manifest_error"
    exit 1
fi
grep -q "must have a String 'name' key" "$manifest_error"

printf '{"name": "greeter", "version": 1}\n' >"$manifest_dir/cuts/greeter/diamond.cut"
if (cd "$manifest_dir" && "$diamond_abs" main.di) >/dev/null 2>"$manifest_error"; then
    echo "non-String version unexpectedly succeeded" >&2
    rm -rf "$manifest_dir" "$manifest_error"
    exit 1
fi
grep -q "key 'version' must be a String" "$manifest_error"

printf 'this is not valid Diamond syntax )))\n' >"$manifest_dir/cuts/greeter/diamond.cut"
if (cd "$manifest_dir" && "$diamond_abs" main.di) >/dev/null 2>"$manifest_error"; then
    echo "manifest with a compile error unexpectedly succeeded" >&2
    rm -rf "$manifest_dir" "$manifest_error"
    exit 1
fi
grep -q "failed to compile at line" "$manifest_error"

printf 'raise "manifest boom"\n' >"$manifest_dir/cuts/greeter/diamond.cut"
if (cd "$manifest_dir" && "$diamond_abs" main.di) >/dev/null 2>"$manifest_error"; then
    echo "manifest that raises unexpectedly succeeded" >&2
    rm -rf "$manifest_dir" "$manifest_error"
    exit 1
fi
grep -q "cut manifest '.*' failed: uncaught exception: manifest boom" "$manifest_error"
rm -f "$manifest_error"

printf '{"name": "greeter", "version": "0.1.0"}\n' >"$manifest_dir/cuts/greeter/diamond.cut"
actual="$(cd "$manifest_dir" && DIAMOND_STRESS_GC=1 "$diamond_abs" main.di)"
[[ "$actual" == "hi, world" ]]
rm -rf "$manifest_dir"

if "$diamond" -e $'def invalid[T, T](value: T) = value' >/dev/null 2>&1; then
    echo "duplicate generic type variable unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'def invalid[T](value: Missing) = value' >/dev/null 2>&1; then
    echo "unknown generic annotation unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'def identity[T](value: T) = value\ndef invalid(value: T) = value' \
    >/dev/null 2>&1; then
    echo "generic type variable escaped its declaration scope" >&2
    exit 1
fi

if "$diamond" -e $'def empty[T]() -> Array[T] = []\nempty[Int, String]()' \
    >/dev/null 2>&1; then
    echo "wrong explicit generic argument count unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'class Box\n include Missing\nend' >/dev/null 2>&1; then
    echo "undefined included module unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'module Box\nend\nclass Box\nend' >/dev/null 2>&1; then
    echo "module and class name collision unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'module Recursive\n include Recursive\nend' >/dev/null 2>&1; then
    echo "self-including module unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e 'Missing::Thing.new()' >/dev/null 2>&1; then
    echo "undefined qualified name unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'module Config\n VALUE = 1\n VALUE = 2\nend' >/dev/null 2>&1; then
    echo "namespace constant reassignment unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'module Config\n value = 1\nend' >/dev/null 2>&1; then
    echo "lowercase module constant unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'module Tools\n def self.answer() = 1\n def self.answer() = 2\nend' >/dev/null 2>&1; then
    echo "duplicate module singleton function unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'class Factory\n def self.answer() = 1\n def self.answer() = 2\nend' >/dev/null 2>&1; then
    echo "duplicate class singleton method unexpectedly compiled" >&2
    exit 1
fi

if ! error="$($diamond -e $'class Box\n private\n def hidden() = 1\nend\nBox.new().hidden()' 2>&1 >/dev/null)"; then
    grep -q "private method 'hidden' called with an explicit receiver" <<<"$error"
else
    echo "private method call unexpectedly succeeded" >&2
    exit 1
fi

if "$diamond" -e $'class Broken\n attr_reader value,\nend' >/dev/null 2>&1; then
    echo "trailing attribute comma unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'class Box\n private missing\nend' >/dev/null 2>&1; then
    echo "undefined visibility target unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'class Box\n attr_reader value\n attr_reader value\nend' >/dev/null 2>&1; then
    echo "duplicate generated reader unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'module Named\n attr_writer name\n attr_writer name\nend' >/dev/null 2>&1; then
    echo "duplicate generated module writer unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'module Stateful\n def value() = @value\n module_function value\nend' >/dev/null 2>&1; then
    echo "stateful targeted module_function unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'module Stateful\n module_function\n attr_reader value\nend' >/dev/null 2>&1; then
    echo "stateful module_function mode unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'class Broken\n alias_method answer, missing\nend' >/dev/null 2>&1; then
    echo "undefined alias source unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'class Broken\n attr_writer value\n alias_method value=, value=\nend' >/dev/null 2>&1; then
    echo "duplicate writer alias unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'class Broken\n def value() = 42\n alias_method(result, value\nend' >/dev/null 2>&1; then
    echo "unterminated parenthesized alias unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'class Broken\n attr_predicate ready\n attr_predicate ready\nend' >/dev/null 2>&1; then
    echo "duplicate predicate attribute unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'raise' >/dev/null 2>&1; then
    echo "bare raise outside rescue unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'retry' >/dev/null 2>&1; then
    echo "retry outside rescue unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'redo' >/dev/null 2>&1; then
    echo "redo outside loop unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'begin\n raise "x"\nrescue error\n 1\nrescue : TypeError\n 2\nend' >/dev/null 2>&1; then
    echo "rescue after catch-all unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'begin\n 1 / 0\nrescue : TypeError | TypeError\n 42\nend' >/dev/null 2>&1; then
    echo "duplicate rescue type unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'begin\n 1 / 0\nrescue : TypeError\n 1\nrescue : TypeError\n 2\nend' >/dev/null 2>&1; then
    echo "repeated rescue clause type unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'begin\n 1 / 0\nrescue : StandardError\n 1\nrescue : ZeroDivisionError\n 2\nend' >/dev/null 2>&1; then
    echo "shadowed rescue subclass unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'loop\n break 42\nend')"
[[ "$actual" == "42" ]]
if "$diamond" -e $'loop 42 end' >/dev/null 2>&1; then exit 1; fi
actual="$($diamond -e $'error=RuntimeError.new()\n[error.message(), error.cause()]')"
[[ "$actual" == "[nil, nil]" ]]
if "$diamond" -e $'RuntimeError.new(1, 2, 3)' >/dev/null 2>&1; then exit 1; fi

actual="$($diamond -e '1+2 if true')"
[[ "$actual" == "3" ]]
if "$diamond" -e '42 if' >/dev/null 2>&1; then
    echo "postfix modifier without condition unexpectedly compiled" >&2
    exit 1
fi
if "$diamond" -e $'42 if\n true' >/dev/null 2>&1; then
    echo "multiline postfix condition unexpectedly compiled" >&2
    exit 1
fi
declaration_error="$(mktemp)"
if "$diamond" -e $'class Marker\nend if true' >/dev/null 2>"$declaration_error"; then
    echo "declaration-level postfix unexpectedly compiled" >&2
    rm -f "$declaration_error"
    exit 1
fi
grep -q "postfix modifiers cannot follow declarations" "$declaration_error"
rm -f "$declaration_error"
if "$diamond" -e $'module Marker\nend unless false' >/dev/null 2>"$declaration_error"; then
    echo "module postfix unexpectedly compiled" >&2
    rm -f "$declaration_error"
    exit 1
fi
grep -q "postfix modifiers cannot follow declarations" "$declaration_error"
if "$diamond" -e $'interface Marker\nend if true' >/dev/null 2>"$declaration_error"; then
    echo "interface postfix unexpectedly compiled" >&2
    rm -f "$declaration_error"
    exit 1
fi
grep -q "postfix modifiers cannot follow declarations" "$declaration_error"
rm -f "$declaration_error"
actual="$($diamond -e $'def typed_endless() -> Int = 42 if true\ntyped_endless()')"
[[ "$actual" == "42" ]]
if "$diamond" -e 'require "missing" if true' >/dev/null 2>&1; then
    echo "conditional require unexpectedly compiled" >&2
    exit 1
fi
require_error="$(mktemp)"
if "$diamond" -e 'require "missing" if true' >/dev/null 2>"$require_error"; then
    rm -f "$require_error"
    exit 1
fi
grep -q "require directive cannot have trailing syntax" "$require_error"
rm -f "$require_error"
require_error="$(mktemp)"
if "$diamond" -e 'require "missing_dependency"' >/dev/null 2>"$require_error"; then
    rm -f "$require_error"
    exit 1
fi
grep -q "cannot require 'missing_dependency.di'" "$require_error"
rm -f "$require_error"
long_path="$(printf 'x%.0s' $(seq 1 5000))"
if "$diamond" "$long_path" >/dev/null 2>&1; then
    echo "oversized root path unexpectedly opened" >&2
    exit 1
fi
root_error="$(mktemp)"
if "$diamond" tests/multifile/no_such_root.di >/dev/null 2>"$root_error"; then
    rm -f "$root_error"
    exit 1
fi
grep -q "cannot open 'tests/multifile/no_such_root.di'" "$root_error"
rm -f "$root_error"
root_error="$(mktemp)"
if "$diamond" tests >/dev/null 2>"$root_error"; then
    rm -f "$root_error"
    exit 1
fi
grep -q "cannot read 'tests': Is a directory" "$root_error"
rm -f "$root_error"
root_error="$(mktemp)"
if "$diamond" --dump-bytecode tests/multifile/no_such_root.di >/dev/null 2>"$root_error"; then
    rm -f "$root_error"
    exit 1
fi
grep -q "cannot open 'tests/multifile/no_such_root.di'" "$root_error"
rm -f "$root_error"
if "$diamond" -e $'module Constants\n VALUE = 42 if true\nend' >/dev/null 2>&1; then
    echo "conditional namespace constant unexpectedly compiled" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e 'Fiber.new(5)' >/dev/null 2>"$error_file"; then
    echo "Fiber.new with a non-Callable argument unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'Fiber.new argument must be a Callable value' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'def make()\n def once(x)\n  x\n end\n once\nend\nFiber.new(make())' \
    >/dev/null 2>"$error_file"; then
    echo "Fiber.new with a non-zero-arity callable unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'Fiber.new callable must take no arguments' "$error_file"
rm -f "$error_file"

actual="$($diamond --dump-bytecode -e $'Fiber = 5\nFiber.new(1)' 2>/dev/null || true)"
if grep -q 'FIBER_NEW' <<<"$actual"; then
    echo "Fiber.new on a shadowing local unexpectedly compiled to FIBER_NEW" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e $'def Fiber()\n 1\nend\nFiber.new(1)' >/dev/null 2>"$error_file"; then
    echo "Fiber.new on a shadowing top-level function unexpectedly compiled" >&2
    exit 1
fi
# A top-level function's bare name is now a real, first-class Callable
# value (see docs/roadmap.md), so this fails a step later than it used
# to: `Fiber` itself now resolves fine (it's the shadowing function,
# used as a value), and it's `.new(1)` -- invoking a method on a
# Closure -- that fails, at runtime rather than compile time.
grep -q "type error" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'def File()\n 1\nend\nFile.open("x", "r")' >/dev/null 2>"$error_file"; then
    echo "File.open on a shadowing top-level function unexpectedly compiled" >&2
    exit 1
fi
grep -q "type error" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'def make()\n def once()\n  1\n end\n once\nend\nf = Fiber.new(make())\nf.resume(0, 1)' \
    >/dev/null 2>"$error_file"; then
    echo "Fiber#resume with too many arguments unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: wrong number of arguments' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'def make()\n def once()\n  1\n end\n once\nend\nf = Fiber.new(make())\nf.nonexistent()' \
    >/dev/null 2>"$error_file"; then
    echo "an unknown method on a Fiber receiver unexpectedly succeeded" >&2
    exit 1
fi
grep -q "undefined method 'nonexistent' for Fiber" "$error_file"
rm -f "$error_file"

actual="$($diamond --dump-bytecode -e 'print("x")')"
grep -Eq 'PRINT +r[0-9]+, r[0-9]+, newline=0' <<<"$actual"

actual="$($diamond --dump-bytecode -e 'puts("x")')"
grep -Eq 'PRINT +r[0-9]+, r[0-9]+, newline=1' <<<"$actual"

if "$diamond" -e 'puts(1, 2)' >/dev/null 2>&1; then
    echo "puts with more than one argument unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode -e 'gets()' </dev/null)"
grep -Eq 'GETS +r[0-9]+' <<<"$actual"

actual="$(printf 'hello world\n' | $diamond -e $'name = gets()\n"hi #{name}"')"
[[ "$actual" == "hi hello world" ]]

actual="$(printf '' | $diamond -e $'x = gets()\nx == nil')"
[[ "$actual" == "true" ]]

actual="$(printf 'no trailing newline' | $diamond -e $'gets()')"
[[ "$actual" == "no trailing newline" ]]

actual="$(printf 'line1\r\nline2\n' | $diamond -e $'a = gets()\nb = gets()\n"#{a}|#{b}"')"
[[ "$actual" == "line1|line2" ]]

if "$diamond" -e 'gets(1)' >/dev/null 2>&1; then
    echo "gets with an argument unexpectedly compiled" >&2
    exit 1
fi

# debugger()/breakpoint(): real blocking-stdin behavior, so -- same reason
# gets() itself is never actually invoked from tests/cases/*.di (only
# shadowed there, see legacy_0333.di) -- these live here instead, with
# stdin explicitly controlled. </dev/null (EOF immediately) is the
# well-defined "continue right away" path this is designed around, not a
# workaround.
actual="$($diamond --dump-bytecode -e 'debugger()' </dev/null 2>&1)"
grep -Eq 'DEBUGGER +r[0-9]+, 0 locals' <<<"$actual"

actual="$($diamond -e $'x = 5\ny = x * 2\ndebugger()\ny + 1' </dev/null)"
grep -q -- '--- paused at -e:3:' <<<"$actual"
grep -q '^locals:$' <<<"$actual"
grep -q '^  x = 5$' <<<"$actual"
grep -q '^  y = 10$' <<<"$actual"
grep -q '(press Enter to continue)' <<<"$actual"
[[ "$(tail -1 <<<"$actual")" == "11" ]]

actual="$($diamond -e $'z = 1\nbreakpoint()\nz' </dev/null)"
grep -q -- '--- paused at -e:2:' <<<"$actual"
grep -q '^  z = 1$' <<<"$actual"
[[ "$(tail -1 <<<"$actual")" == "1" ]]

if "$diamond" -e 'debugger(1)' >/dev/null 2>&1; then
    echo "debugger with an argument unexpectedly compiled" >&2
    exit 1
fi

# A user-defined function of the same name shadows the built-in, exactly
# like puts/gets/Time/etc. do -- never even reaches the blocking path.
actual="$($diamond -e $'def debugger()\n  "shadowed"\nend\ndebugger()')"
[[ "$actual" == "shadowed" ]]

file_dir="$(mktemp -d)"
data_file="$file_dir/data.txt"
actual="$($diamond -e "$(printf 'f = File.open("%s", "w")\nf.write("hello, ")\nf.write("world")\nf.close()\ng = File.open("%s", "r")\ncontent = g.read()\ng.close()\ncontent' "$data_file" "$data_file")")"
[[ "$actual" == "hello, world" ]]

printf 'line1\nline2\n' >"$data_file"
actual="$($diamond -e "$(printf 'f = File.open("%s", "r")\na = f.gets()\nb = f.gets()\nc = f.gets()\nf.close()\n"#{a}|#{b}|#{c}"' "$data_file")")"
[[ "$actual" == "line1|line2|nil" ]]

actual="$($diamond -e "$(printf 'begin\n File.open("%s/nonexistent", "r")\nrescue error: IOError\n 42\nend' "$file_dir")")"
[[ "$actual" == "42" ]]

actual="$($diamond -e "$(printf 'f = File.open("%s", "r")\nf.close()\nbegin\n f.read()\nrescue error: IOError\n 42\nend' "$data_file")")"
[[ "$actual" == "42" ]]

actual="$($diamond -e "$(printf 'f = File.open("%s", "r")\nbegin\n f.write("x")\nrescue error: IOError\n 42\nend' "$data_file")")"
[[ "$actual" == "42" ]]

actual="$($diamond --dump-bytecode -e 'File = 5
File.open("x", "r")' 2>/dev/null || true)"
if grep -q 'FILE_OPEN' <<<"$actual"; then
    echo "File.open on a shadowing local unexpectedly compiled to FILE_OPEN" >&2
    rm -rf "$file_dir"
    exit 1
fi

if "$diamond" -e "$(printf 'f = File.open("%s", "r")\nf.write()' "$data_file")" >/dev/null 2>&1; then
    echo "File#write with no arguments unexpectedly succeeded" >&2
    rm -rf "$file_dir"
    exit 1
fi

rm -rf "$file_dir"

actual="$($diamond --dump-bytecode -e 'TCPSocket.connect("h", 1)' 2>/dev/null || true)"
grep -q 'TCP_CONNECT' <<<"$actual"

actual="$($diamond --dump-bytecode -e 'TCPServer.listen(0)' 2>/dev/null || true)"
grep -q 'TCP_LISTEN' <<<"$actual"

actual="$($diamond -e $'begin\n TCPSocket.connect("127.0.0.1", 1)\nrescue error: IOError\n 42\nend')"
[[ "$actual" == "42" ]]

socket_port=18734
server_out="$(mktemp)"
timeout 10 "$diamond" -e "$(printf 'server = TCPServer.listen(%d)
conn = server.accept()
msg = conn.gets()
conn.write("echo: #{msg}\\n")
conn.close()
server.close()
0' "$socket_port")" >"$server_out" 2>&1 &
socket_server_pid=$!
client_src="$(printf 'c = nil
attempts = 0
while c == nil
 c = begin
  TCPSocket.connect("127.0.0.1", %d)
 rescue error: IOError
  attempts = attempts + 1
  if attempts > 2000
   raise "giving up"
  end
  nil
 end
end
c.write("hello\\n")
response = c.gets()
c.close()
response' "$socket_port")"
client_out="$(mktemp)"
timeout 10 "$diamond" -e "$client_src" >"$client_out" 2>&1
wait "$socket_server_pid"
[[ "$(cat "$server_out")" == "0" ]]
[[ "$(cat "$client_out")" == "echo: hello" ]]
rm -f "$server_out" "$client_out"

actual="$($diamond --dump-bytecode -e 'TCPSocket = 5
TCPSocket.connect("h", 1)' 2>/dev/null || true)"
if grep -q 'TCP_CONNECT' <<<"$actual"; then
    echo "TCPSocket.connect on a shadowing local unexpectedly compiled to TCP_CONNECT" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode -e 'TCPServer = 5
TCPServer.listen(0)' 2>/dev/null || true)"
if grep -q 'TCP_LISTEN' <<<"$actual"; then
    echo "TCPServer.listen on a shadowing local unexpectedly compiled to TCP_LISTEN" >&2
    exit 1
fi

actual="$($diamond -e $'server = TCPServer.listen(0)\nserver.close()\nbegin\n server.accept()\nrescue error: IOError\n 42\nend')"
[[ "$actual" == "42" ]]

if "$diamond" -e $'def run()\n def bad(a, b)\n  a + b\n end\n [1].each(bad)\nend\nrun()' >/dev/null 2>&1; then
    echo "Array#each with a mismatched callback arity unexpectedly succeeded" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e '[].nope()' >/dev/null 2>"$error_file"; then
    echo "an unrecognized method on an Array receiver unexpectedly succeeded" >&2
    exit 1
fi
grep -q "undefined method 'nope' for Array" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '{}.nope()' >/dev/null 2>"$error_file"; then
    echo "an unrecognized method on a Hash receiver unexpectedly succeeded" >&2
    exit 1
fi
grep -q "undefined method 'nope' for Hash" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '"hi".slice(10, 1)' >/dev/null 2>"$error_file"; then
    echo "String#slice with an out-of-bounds start unexpectedly succeeded" >&2
    exit 1
fi
grep -q "index 10 out of bounds for String of length 2" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '"hi".slice("a", 1)' >/dev/null 2>"$error_file"; then
    echo "String#slice with a non-Int argument unexpectedly succeeded" >&2
    exit 1
fi
grep -q "String#slice arguments must be Int" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '"hi".index_of(5)' >/dev/null 2>"$error_file"; then
    echo "String#index_of with a non-String argument unexpectedly succeeded" >&2
    exit 1
fi
grep -q "String#index_of argument must be a String" "$error_file"
rm -f "$error_file"

file_dir="$(mktemp -d)"
data_file="$file_dir/data.txt"
printf 'hello world, this is a test file' >"$data_file"
actual="$($diamond -e "$(printf 'f = File.open("%s", "r")\nchunk = f.read(5)\nrest = f.read()\nf.close()\n"#{chunk}|#{rest}"' "$data_file")")"
[[ "$actual" == "hello| world, this is a test file" ]]

actual="$($diamond -e "$(printf 'f = File.open("%s", "r")\nf.read()' "$data_file")")"
[[ "$actual" == "hello world, this is a test file" ]]

error_file="$(mktemp)"
if "$diamond" -e "$(printf 'f = File.open("%s", "r")\nf.read(-1)' "$data_file")" \
    >/dev/null 2>"$error_file"; then
    echo "File#read with a negative length unexpectedly succeeded" >&2
    rm -rf "$file_dir" "$error_file"
    exit 1
fi
grep -q "File#read argument must be a non-negative Int" "$error_file"
rm -f "$error_file"

rm -rf "$file_dir"

error_file="$(mktemp)"
if "$diamond" -e 'File.open(1, "r")' >/dev/null 2>"$error_file"; then
    echo "File.open with a non-String path unexpectedly succeeded" >&2
    exit 1
fi
grep -q "File.open arguments must be String values" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'TCPSocket.connect(1, "80")' >/dev/null 2>"$error_file"; then
    echo "TCPSocket.connect with a non-String host unexpectedly succeeded" >&2
    exit 1
fi
grep -q "TCPSocket.connect arguments must be a String host and an Int port" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'TCPServer.listen("80")' >/dev/null 2>"$error_file"; then
    echo "TCPServer.listen with a non-Int port unexpectedly succeeded" >&2
    exit 1
fi
grep -q "TCPServer.listen argument must be an Int port" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'File.write("x")' >/dev/null 2>"$error_file"; then
    echo "malformed File.write unexpectedly compiled" >&2
    exit 1
fi
grep -q "expected 'open' after 'File'" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'File.open("x")' >/dev/null 2>"$error_file"; then
    echo "File.open with a missing mode argument unexpectedly compiled" >&2
    exit 1
fi
grep -q "expected ',' after File.open path" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'TCPSocket.dial("x", 80)' >/dev/null 2>"$error_file"; then
    echo "malformed TCPSocket.dial unexpectedly compiled" >&2
    exit 1
fi
grep -q "expected 'connect' after 'TCPSocket'" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'TCPServer.watch(80)' >/dev/null 2>"$error_file"; then
    echo "malformed TCPServer.watch unexpectedly compiled" >&2
    exit 1
fi
grep -q "expected 'listen' or 'listen_nonblocking' after 'TCPServer'" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '"a".split(5)' >/dev/null 2>"$error_file"; then
    echo "String#split with a non-String argument unexpectedly succeeded" >&2
    exit 1
fi
grep -q "String#split argument must be a String" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'chr("x")' >/dev/null 2>"$error_file"; then
    echo "chr with a non-Int argument unexpectedly succeeded" >&2
    exit 1
fi
grep -q "chr argument must be an Int" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '"hello"[10]' >/dev/null 2>"$error_file"; then
    echo "String index out of bounds unexpectedly succeeded" >&2
    exit 1
fi
grep -q "index 10 out of bounds for String of length 5" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '"hello"[-1]' >/dev/null 2>"$error_file"; then
    echo "String index -1 unexpectedly succeeded" >&2
    exit 1
fi
grep -q "index -1 out of bounds for String of length 5" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '""[0]' >/dev/null 2>"$error_file"; then
    echo "Indexing an empty String unexpectedly succeeded" >&2
    exit 1
fi
grep -q "index 0 out of bounds for String of length 0" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'s = "hello"\ns[0] = "x"' >/dev/null 2>"$error_file"; then
    echo "String element assignment unexpectedly succeeded" >&2
    exit 1
fi
grep -q "String does not support element assignment" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '"x".repeat(-1)' >/dev/null 2>"$error_file"; then
    echo "String#repeat with a negative argument unexpectedly succeeded" >&2
    exit 1
fi
grep -q "String#repeat argument must be a non-negative Int" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '"x".repeat("y")' >/dev/null 2>"$error_file"; then
    echo "String#repeat with a non-Int argument unexpectedly succeeded" >&2
    exit 1
fi
grep -q "String#repeat argument must be an Int" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '"hello".slice(0, -1)' >/dev/null 2>"$error_file"; then
    echo "String#slice with a negative length unexpectedly succeeded" >&2
    exit 1
fi
grep -q "index 0 out of bounds for String of length 5" "$error_file"
rm -f "$error_file"

stress_file_dir="$(mktemp -d)"
stress_data_file="$stress_file_dir/data.txt"
actual="$(DIAMOND_STRESS_GC=1 $diamond -e "$(printf 'f = File.open("%s", "w")\nf.write("hello, stress gc")\nf.close()\ng = File.open("%s", "r")\ncontent = g.read()\ng.close()\ncontent' "$stress_data_file" "$stress_data_file")")"
[[ "$actual" == "hello, stress gc" ]]
rm -rf "$stress_file_dir"

error_file="$(mktemp)"
if "$diamond" -e '"hi".nope()' >/dev/null 2>"$error_file"; then
    echo "an unrecognized method on a String receiver unexpectedly succeeded" >&2
    exit 1
fi
grep -q "undefined method 'nope' for String" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'def run()\n def once()\n  1\n end\n f = Fiber.new(once)\n f.nope()\nend\nrun()' \
    >/dev/null 2>"$error_file"; then
    echo "an unrecognized method on a Fiber receiver unexpectedly succeeded" >&2
    exit 1
fi
grep -q "undefined method 'nope' for Fiber" "$error_file"
rm -f "$error_file"

file_dir="$(mktemp -d)"
printf 'x' >"$file_dir/data.txt"
error_file="$(mktemp)"
if "$diamond" -e "$(printf 'f = File.open("%s", "r")\nf.nope()' "$file_dir/data.txt")" \
    >/dev/null 2>"$error_file"; then
    echo "an unrecognized method on a File receiver unexpectedly succeeded" >&2
    exit 1
fi
grep -q "undefined method 'nope' for File" "$error_file"
rm -f "$error_file" "$file_dir/data.txt"
rmdir "$file_dir"

error_file="$(mktemp)"
if "$diamond" -e 's = TCPServer.listen(0)
s.nope()' >/dev/null 2>"$error_file"; then
    echo "an unrecognized method on a Listener receiver unexpectedly succeeded" >&2
    exit 1
fi
grep -q "undefined method 'nope' for Listener" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'array_sort([1, "bad", 2])' >/dev/null 2>"$error_file"; then
    echo "array_sort with a non-Int element unexpectedly succeeded" >&2
    exit 1
fi
grep -q "expected Array\[Int\], got Array" "$error_file"
rm -f "$error_file"

stress_socket_port=18746
stress_server_out="$(mktemp)"
timeout 10 env DIAMOND_STRESS_GC=1 "$diamond" -e "$(printf 'server = TCPServer.listen(%d)
conn = server.accept()
msg = conn.gets()
conn.write("echo: #{msg}\\n")
conn.close()
server.close()
0' "$stress_socket_port")" >"$stress_server_out" 2>&1 &
stress_server_pid=$!
stress_client_src="$(printf 'c = nil
attempts = 0
while c == nil
 c = begin
  TCPSocket.connect("127.0.0.1", %d)
 rescue error: IOError
  attempts = attempts + 1
  if attempts > 2000
   raise "giving up"
  end
  nil
 end
end
c.write("hello\\n")
response = c.gets()
c.close()
response' "$stress_socket_port")"
stress_client_out="$(mktemp)"
timeout 10 env DIAMOND_STRESS_GC=1 "$diamond" -e "$stress_client_src" >"$stress_client_out" 2>&1
wait "$stress_server_pid"
[[ "$(cat "$stress_server_out")" == "0" ]]
[[ "$(cat "$stress_client_out")" == "echo: hello" ]]
rm -f "$stress_server_out" "$stress_client_out"

actual="$($diamond --dump-bytecode -e 'UDPSocket.bind(0)' 2>/dev/null || true)"
grep -q 'UDP_BIND' <<<"$actual"

actual="$($diamond --dump-bytecode -e 'UDPSocket.open()' 2>/dev/null || true)"
grep -q 'UDP_OPEN' <<<"$actual"

actual="$($diamond --dump-bytecode -e 'UDPSocket = 5
UDPSocket.bind(0)' 2>/dev/null || true)"
if grep -q 'UDP_BIND' <<<"$actual"; then
    echo "UDPSocket.bind on a shadowing local unexpectedly compiled to UDP_BIND" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode -e 'UDPSocket = 5
UDPSocket.open()' 2>/dev/null || true)"
if grep -q 'UDP_OPEN' <<<"$actual"; then
    echo "UDPSocket.open on a shadowing local unexpectedly compiled to UDP_OPEN" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e 'UDPSocket.bind("80")' >/dev/null 2>"$error_file"; then
    echo "UDPSocket.bind with a non-Int port unexpectedly succeeded" >&2
    exit 1
fi
grep -q "UDPSocket.bind argument must be an Int port" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'UDPSocket.dial(0)' >/dev/null 2>"$error_file"; then
    echo "malformed UDPSocket.dial unexpectedly compiled" >&2
    exit 1
fi
grep -q "expected 'bind' or 'open' after 'UDPSocket'" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 's = UDPSocket.open()
s.send("x", 5, 80)' >/dev/null 2>"$error_file"; then
    echo "UDPSocket#send with a non-String host unexpectedly succeeded" >&2
    exit 1
fi
grep -q "UDPSocket#send arguments must be (data, String host, Int port)" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 's = UDPSocket.open()
s.nope()' >/dev/null 2>"$error_file"; then
    echo "an unrecognized method on a UDPSocket receiver unexpectedly succeeded" >&2
    exit 1
fi
grep -q "undefined method 'nope' for UDPSocket" "$error_file"
rm -f "$error_file"

# A real client/server round trip. UDP has no TCPSocket.connect-style
# "keep retrying until the port's actually listening" signal (there's no
# handshake to fail cleanly), so the server prints "ready" right after
# UDPSocket.bind succeeds and the client side polls for that line in the
# captured output instead.
udp_port=18747
udp_server_out="$(mktemp)"
"$diamond" -e "$(printf 'socket = UDPSocket.bind(%d)
puts("ready")
result = socket.receive(1024)
socket.send("echo: #{result["data"]}", result["host"], result["port"])
socket.close()
0' "$udp_port")" >"$udp_server_out" 2>&1 &
udp_server_pid=$!
for _ in $(seq 1 200); do
    grep -q '^ready$' "$udp_server_out" && break
    sleep 0.05
done
udp_client_src="$(printf 'client = UDPSocket.open()
client.send("hello", "127.0.0.1", %d)
result = client.receive(1024)
client.close()
"#{result["data"]}|#{result["host"]}"' "$udp_port")"
udp_client_out="$(mktemp)"
timeout 10 "$diamond" -e "$udp_client_src" >"$udp_client_out" 2>&1
wait "$udp_server_pid"
[[ "$(tail -n1 "$udp_server_out")" == "0" ]]
[[ "$(cat "$udp_client_out")" == "echo: hello|127.0.0.1" ]]
rm -f "$udp_server_out" "$udp_client_out"

# Same round trip again under DIAMOND_STRESS_GC=1 -- exercises
# UDPSocket#receive's own multi-key Hash construction (data/host/port)
# under a collection on every single allocation, the same class of hazard
# IO.poll's own Hash result had (see docs/io.md).
udp_stress_port=18748
udp_stress_server_out="$(mktemp)"
env DIAMOND_STRESS_GC=1 "$diamond" -e "$(printf 'socket = UDPSocket.bind(%d)
puts("ready")
result = socket.receive(1024)
socket.send("echo: #{result["data"]}", result["host"], result["port"])
socket.close()
0' "$udp_stress_port")" >"$udp_stress_server_out" 2>&1 &
udp_stress_server_pid=$!
for _ in $(seq 1 200); do
    grep -q '^ready$' "$udp_stress_server_out" && break
    sleep 0.05
done
udp_stress_client_src="$(printf 'client = UDPSocket.open()
client.send("hello", "127.0.0.1", %d)
result = client.receive(1024)
client.close()
"#{result["data"]}|#{result["host"]}"' "$udp_stress_port")"
udp_stress_client_out="$(mktemp)"
timeout 10 env DIAMOND_STRESS_GC=1 "$diamond" -e "$udp_stress_client_src" >"$udp_stress_client_out" 2>&1
wait "$udp_stress_server_pid"
[[ "$(tail -n1 "$udp_stress_server_out")" == "0" ]]
[[ "$(cat "$udp_stress_client_out")" == "echo: hello|127.0.0.1" ]]
rm -f "$udp_stress_server_out" "$udp_stress_client_out"

actual="$($diamond --dump-bytecode -e 'def r()
 def h()
  1
 end
 Signal.trap("INT", h)
end
r()' 2>/dev/null || true)"
grep -q 'SIGNAL_TRAP' <<<"$actual"

actual="$($diamond --dump-bytecode -e 'Signal = 5
Signal.trap("INT", 5)' 2>/dev/null || true)"
if grep -q 'SIGNAL_TRAP' <<<"$actual"; then
    echo "Signal.trap on a shadowing local unexpectedly compiled to SIGNAL_TRAP" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e 'Signal.trap(5, 5)' >/dev/null 2>"$error_file"; then
    echo "Signal.trap with a non-String name unexpectedly succeeded" >&2
    exit 1
fi
grep -q "Signal.trap name must be a String" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'Signal.trap("INT", 5)' >/dev/null 2>"$error_file"; then
    echo "Signal.trap with a non-Callable handler unexpectedly succeeded" >&2
    exit 1
fi
grep -q "Signal.trap handler must be a Callable" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'def r()
 def h()
  1
 end
 Signal.trap("KILL", h)
end
r()' >/dev/null 2>"$error_file"; then
    echo "Signal.trap with an unrecognized signal name unexpectedly succeeded" >&2
    exit 1
fi
grep -q "unrecognized signal name 'KILL'" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'Signal.watch("INT", 5)' >/dev/null 2>"$error_file"; then
    echo "malformed Signal.watch unexpectedly compiled" >&2
    exit 1
fi
grep -q "expected 'trap' after 'Signal'" "$error_file"
rm -f "$error_file"

# The real point of this feature: a signal arriving while genuinely
# blocked in a native call (not just between bytecode instructions) has
# to actually interrupt it -- otherwise a trapped signal would never run
# its handler until whatever the process was blocked on happens to
# complete on its own, which for "graceful shutdown of an idle server"
# could be never. Exercises the accept()-specific EINTR-retry path
# (src/vm.c) directly, not just the once-per-instruction dispatch-loop
# check that CPU-bound code alone would already satisfy.
#
# This server does exactly one accept() call (not a loop), so unlike
# http/gremlin's own tests, even a bash /dev/tcp probe-and-close
# readiness check (this repo's usual wait_for_port trick) isn't safe
# here -- that probe connection would itself *be* the one accept() call
# consumes, exactly the stray-connection bug already found and fixed in
# packages/http's own test.sh. So: print "ready" right after
# TCPServer.listen succeeds and poll the captured output for that line
# instead, the same readiness signal the UDP tests above already use for
# the same underlying reason (no safe zero-side-effect probe available).
signal_port=18749
signal_out="$(mktemp)"
# A non-interactive shell (this script, `bash tests/run.sh`) sets SIGINT
# and SIGQUIT to be *ignored* for an asynchronous (backgrounded, `&`)
# command -- well-known bash/POSIX behavior, meant to keep a background
# job alive when the terminal sends SIGINT to the whole foreground
# process group. Since SIG_IGN survives exec(2), the diamond process
# below would inherit SIGINT already ignored at the OS level, and its own
# later Signal.trap (a plain sigaction() call) doesn't get a chance to
# run before that disposition is already in place for anything delivered
# in the gap -- confirmed the hard way: kill -INT reliably did nothing at
# all when this was a plain `... &` background, in this script, despite
# every one of the earlier manual `command &` tests (not run from inside
# a script file) working every time. The fix is the standard one: an
# explicit `trap - INT` (reset to default) inside a subshell, before
# exec-ing the real command, so the child never sees SIG_IGN in the
# first place.
( trap - INT
  exec timeout 10 "$diamond" -e "$(printf 'def run()
  def handler()
    puts("caught INT")
  end
  Signal.trap("INT", handler)
  server = TCPServer.listen(%d)
  puts("ready")
  conn = server.accept()
  puts("accepted")
  conn.close()
  server.close()
end
run()' "$signal_port")" >"$signal_out" 2>&1 ) &
signal_pid=$!
for _ in $(seq 1 200); do
    grep -q '^ready$' "$signal_out" && break
    sleep 0.05
done
kill -INT "$signal_pid"
sleep 0.3
exec 3<>"/dev/tcp/127.0.0.1/$signal_port"
{ exec 3<&- 3>&-; } 2>/dev/null || true
wait "$signal_pid"
actual="$(cat "$signal_out")"
[[ "$actual" == $'ready\ncaught INT\naccepted\nnil' ]]
rm -f "$signal_out"

# An untrapped signal still gets the OS default disposition (kills the
# process) -- Signal.trap is opt-in per signal name, not a blanket
# "Diamond now handles all signals" switch.
"$diamond" -e 'i = 0
while i < 2000000000
  i = i + 1
end' >/dev/null 2>&1 &
untrapped_pid=$!
sleep 0.3
kill -TERM "$untrapped_pid"
sleep 0.3
if kill -0 "$untrapped_pid" 2>/dev/null; then
    echo "process with no Signal.trap unexpectedly survived SIGTERM" >&2
    kill -9 "$untrapped_pid" 2>/dev/null || true
    exit 1
fi

actual="$($diamond --dump-bytecode -e 'TLSSocket.connect("localhost", 443)' 2>/dev/null || true)"
grep -q 'TLS_CONNECT' <<<"$actual"

actual="$($diamond --dump-bytecode -e 'TLSServer.listen(0, "cert.pem", "key.pem")' 2>/dev/null || true)"
grep -q 'TLS_LISTEN' <<<"$actual"

actual="$($diamond --dump-bytecode -e 'TLSSocket = 5
TLSSocket.connect("localhost", 443)' 2>/dev/null || true)"
if grep -q 'TLS_CONNECT' <<<"$actual"; then
    echo "TLSSocket.connect on a shadowing local unexpectedly compiled to TLS_CONNECT" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode -e 'TLSServer = 5
TLSServer.listen(0, "cert.pem", "key.pem")' 2>/dev/null || true)"
if grep -q 'TLS_LISTEN' <<<"$actual"; then
    echo "TLSServer.listen on a shadowing local unexpectedly compiled to TLS_LISTEN" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e 'TLSSocket.connect(5, 443)' >/dev/null 2>"$error_file"; then
    echo "TLSSocket.connect with a non-String host unexpectedly succeeded" >&2
    exit 1
fi
grep -q "TLSSocket.connect arguments must be a String host and an Int port" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'TLSSocket.dial("localhost", 443)' >/dev/null 2>"$error_file"; then
    echo "malformed TLSSocket.dial unexpectedly compiled" >&2
    exit 1
fi
grep -q "expected 'connect' after 'TLSSocket'" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'TLSServer.host(0, "cert.pem", "key.pem")' >/dev/null 2>"$error_file"; then
    echo "malformed TLSServer.host unexpectedly compiled" >&2
    exit 1
fi
grep -q "expected 'listen' after 'TLSServer'" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'TLSServer.listen("80", "cert.pem", "key.pem")' >/dev/null 2>"$error_file"; then
    echo "TLSServer.listen with a non-Int port unexpectedly succeeded" >&2
    exit 1
fi
grep -q "TLSServer.listen arguments must be an Int port and String certificate/key file paths" \
    "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'TLSServer.listen(0, "does-not-exist.pem", "does-not-exist.pem")' \
    >/dev/null 2>"$error_file"; then
    echo "TLSServer.listen with a missing certificate file unexpectedly succeeded" >&2
    exit 1
fi
grep -q "cannot load TLS certificate 'does-not-exist.pem'" "$error_file"
rm -f "$error_file"

# A real client/server TLS round trip that also exercises the "undefined
# method"/"closed socket" error paths against an actual TLSSocket
# instance, the same way the UDP/Socket coverage above does -- both need
# a live handshake to have happened first, so a fresh accepted connection
# is the simplest way to get one. The server accepts twice (once per
# client test below), so it can't be the single-accept()-call shape the
# Signal test above uses -- a small counted loop instead.
tls_errors_dir="$(mktemp -d)"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$tls_errors_dir/key.pem" \
    -out "$tls_errors_dir/cert.pem" -days 1 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1
tls_errors_port=18750
tls_errors_server_out="$(mktemp)"
"$diamond" -e "$(printf 'listener = TLSServer.listen(%d, "%s", "%s")
puts("ready")
i = 0
while i < 2
  conn = listener.accept()
  conn.close()
  i = i + 1
end
listener.close()
0' "$tls_errors_port" "$tls_errors_dir/cert.pem" "$tls_errors_dir/key.pem")" \
    >"$tls_errors_server_out" 2>&1 &
tls_errors_server_pid=$!
for _ in $(seq 1 200); do
    grep -q '^ready$' "$tls_errors_server_out" && break
    sleep 0.05
done
error_file="$(mktemp)"
if SSL_CERT_FILE="$tls_errors_dir/cert.pem" timeout 10 "$diamond" -e "$(printf 'c = TLSSocket.connect("localhost", %d)
c.nope()' "$tls_errors_port")" >/dev/null 2>"$error_file"; then
    echo "an unrecognized method on a TLSSocket receiver unexpectedly succeeded" >&2
    exit 1
fi
grep -q "undefined method 'nope' for TLSSocket" "$error_file"
rm -f "$error_file"
error_file="$(mktemp)"
if SSL_CERT_FILE="$tls_errors_dir/cert.pem" timeout 10 "$diamond" -e "$(printf 'c = TLSSocket.connect("localhost", %d)
c.close()
c.write("x")' "$tls_errors_port")" >/dev/null 2>"$error_file"; then
    echo "writing to a closed TLSSocket unexpectedly succeeded" >&2
    exit 1
fi
grep -q "TLS socket is closed" "$error_file"
rm -f "$error_file"
wait "$tls_errors_server_pid"
rm -f "$tls_errors_server_out"
rm -rf "$tls_errors_dir"

# A real client/server TLS round trip, plus the two failure modes that
# actually matter for a "secure by default" client: an untrusted
# certificate is rejected, and a certificate valid for the wrong hostname
# is rejected even when its issuer is trusted. Uses a fresh self-signed
# CA-of-one generated on the fly (openssl req) rather than a checked-in
# fixture, so nothing here depends on a certificate's expiry date years
# from now. $SSL_CERT_FILE overrides OpenSSL's own default trust store
# lookup (X509_get_default_cert_file, well-documented, widely used by
# other projects' own test suites the same way) -- pointing it at this
# self-signed cert makes the *client* process trust exactly this one
# certificate as if it were a real CA, without touching the system trust
# store or needing a --insecure-style escape hatch in TLSSocket.connect's
# own API (see docs/io.md: no such escape hatch is exposed, on purpose).
tls_dir="$(mktemp -d)"
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$tls_dir/key.pem" -out "$tls_dir/cert.pem" \
    -days 1 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost" >/dev/null 2>&1

tls_port=18751
tls_server_out="$(mktemp)"
"$diamond" -e "$(printf 'listener = TLSServer.listen(%d, "%s", "%s")
puts("ready")
conn = listener.accept()
msg = conn.gets()
conn.write("echo: #{msg}\\n")
conn.close()
listener.close()
0' "$tls_port" "$tls_dir/cert.pem" "$tls_dir/key.pem")" >"$tls_server_out" 2>&1 &
tls_server_pid=$!
for _ in $(seq 1 200); do
    grep -q '^ready$' "$tls_server_out" && break
    sleep 0.05
done
tls_client_out="$(mktemp)"
SSL_CERT_FILE="$tls_dir/cert.pem" timeout 10 "$diamond" -e "$(printf 'c = TLSSocket.connect("localhost", %d)
c.write("hello\\n")
r = c.gets()
c.close()
r' "$tls_port")" >"$tls_client_out" 2>&1
wait "$tls_server_pid"
[[ "$(tail -n1 "$tls_server_out")" == "0" ]]
[[ "$(cat "$tls_client_out")" == "echo: hello" ]]
rm -f "$tls_server_out" "$tls_client_out"

# Same round trip again under DIAMOND_STRESS_GC=1 -- exercises TLS session
# setup/handshake/read/write while every single allocation triggers a
# collection, the same treatment every other native I/O feature this round
# got.
tls_stress_port=18752
tls_stress_server_out="$(mktemp)"
env DIAMOND_STRESS_GC=1 "$diamond" -e "$(printf 'listener = TLSServer.listen(%d, "%s", "%s")
puts("ready")
conn = listener.accept()
msg = conn.gets()
conn.write("echo: #{msg}\\n")
conn.close()
listener.close()
0' "$tls_stress_port" "$tls_dir/cert.pem" "$tls_dir/key.pem")" >"$tls_stress_server_out" 2>&1 &
tls_stress_server_pid=$!
for _ in $(seq 1 200); do
    grep -q '^ready$' "$tls_stress_server_out" && break
    sleep 0.05
done
tls_stress_client_out="$(mktemp)"
SSL_CERT_FILE="$tls_dir/cert.pem" timeout 10 env DIAMOND_STRESS_GC=1 "$diamond" -e "$(printf 'c = TLSSocket.connect("localhost", %d)
c.write("hello\\n")
r = c.gets()
c.close()
r' "$tls_stress_port")" >"$tls_stress_client_out" 2>&1
wait "$tls_stress_server_pid"
[[ "$(tail -n1 "$tls_stress_server_out")" == "0" ]]
[[ "$(cat "$tls_stress_client_out")" == "echo: hello" ]]
rm -f "$tls_stress_server_out" "$tls_stress_client_out"

# Untrusted certificate: no $SSL_CERT_FILE override, so this self-signed
# cert isn't trusted by anything -- TLSSocket.connect must refuse the
# connection rather than silently accepting it (there is no verify=false
# escape hatch to accidentally reach for instead).
tls_untrusted_port=18753
tls_untrusted_server_out="$(mktemp)"
"$diamond" -e "$(printf 'listener = TLSServer.listen(%d, "%s", "%s")
puts("ready")
conn = listener.accept()
conn.close()
listener.close()
0' "$tls_untrusted_port" "$tls_dir/cert.pem" "$tls_dir/key.pem")" >"$tls_untrusted_server_out" 2>&1 &
tls_untrusted_server_pid=$!
for _ in $(seq 1 200); do
    grep -q '^ready$' "$tls_untrusted_server_out" && break
    sleep 0.05
done
error_file="$(mktemp)"
if timeout 10 "$diamond" -e "$(printf 'TLSSocket.connect("localhost", %d)' "$tls_untrusted_port")" \
    >/dev/null 2>"$error_file"; then
    echo "TLSSocket.connect to an untrusted self-signed certificate unexpectedly succeeded" >&2
    exit 1
fi
grep -q "certificate verify failed" "$error_file"
rm -f "$error_file"
# The server's own SSL_accept() legitimately fails too here (the client
# aborts the handshake with a fatal alert once its own verification
# fails, same as any TLS client would), so listener.accept() raises and
# the server script exits non-zero -- not a bug, just not this test's own
# assertion, so the exit status itself is intentionally not checked.
wait "$tls_untrusted_server_pid" || true
rm -f "$tls_untrusted_server_out"

# Trusted issuer, wrong hostname: the certificate is for "localhost" only
# (its one subjectAltName above) -- connecting to the same server via
# "127.0.0.1" instead must still fail, proving SSL_set1_host is actually
# enforcing the hostname match rather than verification stopping at "is
# the issuer trusted?".
tls_mismatch_port=18754
tls_mismatch_server_out="$(mktemp)"
"$diamond" -e "$(printf 'listener = TLSServer.listen(%d, "%s", "%s")
puts("ready")
conn = listener.accept()
conn.close()
listener.close()
0' "$tls_mismatch_port" "$tls_dir/cert.pem" "$tls_dir/key.pem")" >"$tls_mismatch_server_out" 2>&1 &
tls_mismatch_server_pid=$!
for _ in $(seq 1 200); do
    grep -q '^ready$' "$tls_mismatch_server_out" && break
    sleep 0.05
done
error_file="$(mktemp)"
if SSL_CERT_FILE="$tls_dir/cert.pem" timeout 10 "$diamond" -e \
    "$(printf 'TLSSocket.connect("127.0.0.1", %d)' "$tls_mismatch_port")" \
    >/dev/null 2>"$error_file"; then
    echo "TLSSocket.connect with a hostname the certificate doesn't cover unexpectedly succeeded" >&2
    exit 1
fi
grep -q "certificate verify failed" "$error_file"
rm -f "$error_file"
# Same reasoning as the untrusted-certificate test above: the server's own
# SSL_accept() fails too once the client aborts the handshake, so its
# exit status is intentionally not checked here.
wait "$tls_mismatch_server_pid" || true
rm -f "$tls_mismatch_server_out"
rm -rf "$tls_dir"

error_file="$(mktemp)"
if "$diamond" -e '5.abs()' >/dev/null 2>"$error_file"; then
    echo "Int literal .abs() unexpectedly succeeded (Int has no method dispatch)" >&2
    exit 1
fi
grep -q "runtime error" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'def f(x: Float) -> Float = x\nf(3)' >/dev/null 2>"$error_file"; then
    echo "Int argument for a Float-typed parameter unexpectedly succeeded" >&2
    exit 1
fi
grep -q "expected Float, got Int" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'raise 3.14' >/dev/null 2>"$error_file"; then
    echo "raise with a bare Float unexpectedly succeeded" >&2
    exit 1
fi
grep -q "uncaught exception: 3.14" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '5 / 0' >/dev/null 2>"$error_file"; then
    echo "Int division by zero unexpectedly succeeded" >&2
    exit 1
fi
grep -q "division by zero" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'to_f(3.5)' >/dev/null 2>"$error_file"; then
    echo "to_f on a Float argument unexpectedly succeeded" >&2
    exit 1
fi
grep -q "to_f argument must be an Int" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'to_i(3)' >/dev/null 2>"$error_file"; then
    echo "to_i on an Int argument unexpectedly succeeded" >&2
    exit 1
fi
grep -q "to_i argument must be a Float" "$error_file"
rm -f "$error_file"

for bad_arg in '0.0 / 0.0' '1.0 / 0.0' '(0.0 - 1.0) / 0.0'; do
    error_file="$(mktemp)"
    if "$diamond" -e "to_i($bad_arg)" >/dev/null 2>"$error_file"; then
        echo "to_i($bad_arg) unexpectedly succeeded" >&2
        exit 1
    fi
    grep -q "to_i argument must be a finite Float" "$error_file"
    rm -f "$error_file"
done

nines=""
for _ in $(seq 1 70); do nines+="9"; done
actual="$($diamond -e "${nines}.0")"
[[ "$actual" == "1e+70" ]]

actual="$($diamond --dump-bytecode -e 'yield' 2>/dev/null || true)"
yield_section="$(sed -n '/== -e ==/,/^== /p' <<<"$actual")"
grep -Eq 'YIELD +r[0-9]+, r[0-9]+' <<<"$yield_section"
[[ "$(grep -c NIL <<<"$yield_section")" == "0" ]]

actual="$($diamond --dump-bytecode -e 'nil')"
[[ "$(sed -n '/== -e ==/,/^== /p' <<<"$actual" | grep -c NIL)" == "0" ]]

error_file="$(mktemp)"
if "$diamond" -e 'mod(5, 0)' >/dev/null 2>"$error_file"; then
    echo "Int mod by zero unexpectedly succeeded" >&2
    exit 1
fi
grep -q "division by zero" "$error_file"
rm -f "$error_file"

nines=""
for _ in $(seq 1 200); do nines+="9"; done
actual="$($diamond -e "(\"$nines\" + \"$nines\").to_f()")"
[[ "$actual" == "Infinity" ]]

nines=""
for _ in $(seq 1 200); do nines+="9"; done
actual="$($diamond -e "pow((\"$nines\" + \"$nines\").to_f(), 2)")"
[[ "$actual" == "Infinity" ]]

error_file="$(mktemp)"
if "$diamond" -e 'sqrt("x")' >/dev/null 2>"$error_file"; then
    echo "sqrt with a non-numeric argument unexpectedly succeeded" >&2
    exit 1
fi
grep -q "math function argument must be an Int or Float" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e 'pow(2, "x")' >/dev/null 2>"$error_file"; then
    echo "pow with a non-numeric argument unexpectedly succeeded" >&2
    exit 1
fi
grep -q "math function argument must be an Int or Float" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '5e' >/dev/null 2>"$error_file"; then
    echo "bare 5e unexpectedly parsed as a valid program" >&2
    exit 1
fi
grep -q "expected newline after expression" "$error_file"
rm -f "$error_file"

nines=""
for _ in $(seq 1 300); do nines+="9"; done
error_file="$(mktemp)"
if "$diamond" -e "${nines}e300" >/dev/null 2>"$error_file"; then
    echo "an oversized exponent-notation literal unexpectedly parsed" >&2
    exit 1
fi
grep -q "float literal is too long" "$error_file"
rm -f "$error_file"

actual="$($diamond -e '1.5e-3')"
puts_actual="$(DIAMOND_STRESS_GC=1 $diamond -e 'puts(1.5e-3)')"
[[ "$actual" == "0.0015" && "$puts_actual" == $'0.0015\nnil' ]]

actual="$($diamond --dump-bytecode -e $'def test(x: Int | String, y: Int | String)\n if x is Int && y is Int\n  x + y\n else\n  0\n end\nend')"
test_dump="$(sed -n '/^== test ==$/,$p' <<<"$actual")"
grep -q 'ADD_INT' <<<"$test_dump"

actual="$($diamond --dump-bytecode -e $'def apply_nested[T](values: Array[Array[T]], &block: Callable[[T], T]) -> T\n yield(values[0][0])\nend\napply_nested([[20], [21]]) do |value|\n value + 1\nend')"
nested_block_dump="$(sed -n '/^== <block> ==$/,$p' <<<"$actual")"
grep -q 'ADD_INT' <<<"$nested_block_dump"

actual="$($diamond --dump-bytecode -e $'class GenericBox\n def initialize[T](value: T, &block: Callable[[T], T])\n  @value = yield(value)\n end\nend\nGenericBox.new(20) do |value|\n value + 1\nend')"
constructor_block_dump="$(sed -n '/^== <block> ==$/,$p' <<<"$actual")"
grep -q 'ADD_INT' <<<"$constructor_block_dump"

actual="$($diamond --dump-bytecode -e $'def apply_spread[T](value: T, &block: Callable[[T], T]) -> T\n yield(value)\nend\napply_spread(*[20]) do |value|\n value + 1\nend')"
spread_block_dump="$(sed -n '/^== <block> ==$/,$p' <<<"$actual")"
grep -q 'ADD_INT' <<<"$spread_block_dump"

actual="$($diamond --dump-bytecode -e $'class KeywordGeneric\n def initialize[T](value: T, &block: Callable[[T], T])\n  yield(value)\n end\n def self.apply[T](value: T, &block: Callable[[T], T]) -> T\n  yield(value)\n end\n def apply[T](value: T, &block: Callable[[T], T]) -> T\n  yield(value)\n end\nend\nKeywordGeneric.apply(value: 1) do |value|\n value + 1\nend\nobject = KeywordGeneric.new(value: 3) do |value|\n value + 1\nend\nobject.apply(value: 2) do |value|\n value + 1\nend')"
keyword_block_dump="$(sed -n '/^== <block> ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'ADD_INT' <<<"$keyword_block_dump")" == "3" ]]

actual="$($diamond --dump-bytecode tests/cases/generic_return_propagation.di)"
generic_return_dump="$(sed -n '/^== tests\/cases\/generic_return_propagation.di ==$/,/^== abs ==$/p' <<<"$actual")"
[[ "$(grep -c 'ADD_INT' <<<"$generic_return_dump")" == "10" ]]
[[ "$(grep -cE '  ADD +r' <<<"$generic_return_dump")" == "0" ]]

actual="$($diamond --dump-bytecode tests/cases/heterogeneous_spread_generic_inference.di)"
heterogeneous_spread_generic_dump="$(sed -n '/^== tests\/cases\/heterogeneous_spread_generic_inference.di ==$/,/^== min ==$/p' <<<"$actual")"
[[ "$(grep -c 'ADD_INT' <<<"$heterogeneous_spread_generic_dump")" == "9" ]]
[[ "$(grep -cE '  ADD +r' <<<"$heterogeneous_spread_generic_dump")" == "0" ]]

actual="$($diamond --dump-bytecode tests/cases/keyword_spread_context.di)"
keyword_spread_context_dump="$(sed -n '/^== tests\/cases\/keyword_spread_context.di ==$/,/^== min ==$/p' <<<"$actual")"
[[ "$(grep -c 'ADD_INT' <<<"$keyword_spread_context_dump")" == "3" ]]
[[ "$(grep -cE '  ADD +r' <<<"$keyword_spread_context_dump")" == "0" ]]

actual="$($diamond --dump-bytecode tests/cases/generic_union_return_propagation.di)"
generic_union_return_dump="$(sed -n '/^== tests\/cases\/generic_union_return_propagation.di ==$/,/^== abs ==$/p' <<<"$actual")"
[[ "$(grep -c 'ADD_INT' <<<"$generic_union_return_dump")" == "1" ]]
[[ "$(grep -cE '  ADD +r' <<<"$generic_union_return_dump")" == "1" ]]

actual="$($diamond --dump-bytecode tests/cases/bound_method_return_propagation.di)"
bound_method_return_dump="$(sed -n '/^== tests\/cases\/bound_method_return_propagation.di ==$/,/^== abs ==$/p' <<<"$actual")"
[[ "$(grep -c 'ADD_INT' <<<"$bound_method_return_dump")" == "7" ]]
[[ "$(grep -cE '  ADD +r' <<<"$bound_method_return_dump")" == "0" ]]

actual="$($diamond --dump-bytecode tests/cases/callable_union_return_propagation.di)"
callable_union_return_dump="$(sed -n '/^== tests\/cases\/callable_union_return_propagation.di ==$/,/^== abs ==$/p' <<<"$actual")"
[[ "$(grep -c 'ADD_INT' <<<"$callable_union_return_dump")" == "1" ]]
[[ "$(grep -cE '  ADD +r' <<<"$callable_union_return_dump")" == "1" ]]

actual="$($diamond --dump-bytecode tests/cases/callable_union_block_context.di)"
callable_union_block_dump="$(sed -n '/^== <block> ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'ADD_INT' <<<"$callable_union_block_dump")" == "4" ]]
[[ "$(grep -cE '  ADD +r' <<<"$callable_union_block_dump")" == "1" ]]

actual="$($diamond --dump-bytecode -e $'def union_value() -> Int | String\n [1, "one"][0]\nend\nunion_value()')"
union_function_dump="$(sed -n '/^== union_value ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$union_function_dump")" == "0" ]]

actual="$($diamond --dump-bytecode tests/cases/union_receiver_return_join.di)"
union_receiver_join_dump="$(sed -n '/^== tests\/cases\/union_receiver_return_join.di ==$/,/^== abs ==$/p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$union_receiver_join_dump")" == "0" ]]

actual="$($diamond --dump-bytecode tests/cases/divergent_union_block_context.di)"
union_block_dump="$(sed -n '/^== <block> ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'ADD_INT' <<<"$union_block_dump")" == "1" ]]

actual="$($diamond --dump-bytecode -e $'def nested_union() -> Array[Array[Int] | Array[String]]\n [[1], ["one"]]\nend\nnested_union()')"
nested_union_dump="$(sed -n '/^== nested_union ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$nested_union_dump")" == "0" ]]

error_file="$(mktemp)"
if "$diamond" -e 'def duplicate(value: Array[Int] | Array[Int]) = value' \
        >/dev/null 2>"$error_file"; then
    echo "duplicate parameterized union member unexpectedly compiled" >&2
    exit 1
fi
grep -q 'duplicate type in union' "$error_file"
rm -f "$error_file"

actual="$($diamond --dump-bytecode -e $'def test(a: Int | String, b: Int | String) -> String\n unless a is Int || b is Int\n  a\n else\n  "one-or-both"\n end\nend')"
test_dump="$(sed -n '/^== test ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$test_dump")" == "2" ]]

error_file="$(mktemp)"
if "$diamond" -e $'interface B < NoSuchInterface\n def bar()\nend' >/dev/null 2>"$error_file"; then
    echo "interface extension of an undefined base unexpectedly compiled" >&2
    exit 1
fi
grep -q "undefined base interface" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'interface A\n def foo()\nend\ninterface X\n def foo()\nend\ninterface B < A, X\nend' \
    >/dev/null 2>"$error_file"; then
    echo "interface extension with a name collision across bases unexpectedly compiled" >&2
    exit 1
fi
grep -q "duplicate interface method" "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'interface A\n def foo()\nend\ninterface B < A\n def foo()\nend' \
    >/dev/null 2>"$error_file"; then
    echo "interface extension redeclaring an inherited method unexpectedly compiled" >&2
    exit 1
fi
grep -q "duplicate interface method" "$error_file"
rm -f "$error_file"

methods1=""
for i in $(seq 1 130); do methods1+="def m$i()"$'\n'; done
methods2=""
for i in $(seq 1 130); do methods2+="def n$i()"$'\n'; done
overflow_program="interface Base1"$'\n'"$methods1""end"$'\n'"interface Base2"$'\n'"$methods2""end"$'\n'"interface Sub < Base1, Base2"$'\n'"end"
error_file="$(mktemp)"
if "$diamond" -e "$overflow_program" >/dev/null 2>"$error_file"; then
    echo "interface method-count overflow via composition unexpectedly compiled" >&2
    exit 1
fi
grep -q "interface has too many methods" "$error_file"
rm -f "$error_file"

actual="$($diamond -e $'x = 9223372036854775807 + 1\nx')"
puts_actual="$(DIAMOND_STRESS_GC=1 $diamond -e $'x = 9223372036854775807 + 1\nputs(x)')"
[[ "$actual" == "9223372036854775808" && "$puts_actual" == $'9223372036854775808\nnil' ]]

# nproc reports the *host's* CPU count, not what a cgroup-throttled CI
# container is actually allotted -- a k8s runner can report nproc=2 while
# its cpu.max quota caps it well under one real core. That alone turned
# out not to explain the flake seen on GitLab's shared small runner
# (serial 5.1s / parallel 9.17s, a ~1.8x ratio, reproduced twice
# identically): cpu.max there reports "max" (no hard quota), so cpu_budget
# fell back to nproc=2 and the strict check still ran and still failed.
# A consistent ~1.8x (not ~1.0x, not random) is the signature of two
# logical CPUs that are SMT/hyperthread siblings on a single physical
# core -- GitLab's small runners are commonly 2 vCPU = 1 physical core
# with hyperthreading, and a tight integer-increment loop like spin()
# saturates the shared execution ports, so the second logical CPU buys
# almost nothing. Count distinct physical cores directly from
# /proc/cpuinfo (physical id + core id pairs) and prefer that over raw
# nproc/cgroup quota when it's available, since it's the signal that
# actually determines whether two CPU-bound threads have independent
# execution resources to run concurrently on.
cpu_budget="$(nproc)"
if [[ -r /sys/fs/cgroup/cpu.max ]]; then
    read -r cfs_quota cfs_period < /sys/fs/cgroup/cpu.max
    if [[ "$cfs_quota" != "max" ]]; then
        cpu_budget="$(echo "$cfs_quota / $cfs_period" | bc -l)"
    fi
elif [[ -r /sys/fs/cgroup/cpu/cpu.cfs_quota_us && -r /sys/fs/cgroup/cpu/cpu.cfs_period_us ]]; then
    cfs_quota="$(cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us)"
    cfs_period="$(cat /sys/fs/cgroup/cpu/cpu.cfs_period_us)"
    if [[ "$cfs_quota" -gt 0 ]]; then
        cpu_budget="$(echo "$cfs_quota / $cfs_period" | bc -l)"
    fi
fi
physical_cores="$(awk -F: '/physical id/{p=$2} /^core id/{print p","$2}' /proc/cpuinfo 2>/dev/null | sort -u | wc -l)"
if [[ "$physical_cores" -gt 0 ]]; then
    cpu_budget="$physical_cores"
fi
echo "DIAG: nproc=$(nproc) physical_cores=$physical_cores cpu_budget=$cpu_budget" >&2

# Real-parallelism proof for Thread: two threads each doing genuine
# CPU-bound work (not sleep -- sleep would pass even under the old
# single-native-thread Fiber cooperative scheduler if it yielded during
# the sleep, so this has to be work an isolated pthread actually executes
# concurrently to prove anything) should finish in wall-clock time much
# closer to *one* of them than to their sum. Diamond has no Time/clock
# builtin, so this times the whole `diamond` subprocess from bash itself
# (via $EPOCHREALTIME) rather than measuring inside the language -- the
# same reason the Signal.trap tests above are subprocess/bash-timed
# instead of assertions inside the .di source. A generous tolerance band
# (< 1.6x one spin()'s own solo time, not a tight bound) keeps this from
# flaking under CI/sandbox scheduling noise while still failing hard if
# Thread.new secretly ran things serially (which would show up as
# parallel time roughly 2x the serial unit instead). 20,000,000
# iterations is deliberately picked to keep the *fastest* build variant
# (release, -O3) comfortably above a second of serial work --
# thread-spawn/join overhead and OS scheduling jitter are both roughly
# constant regardless of loop size, so a too-short serial run makes the
# ratio flaky (a 2,000,000 first draft measured serial ~0.14s/parallel
# ~0.23s on release here, a 1.7x ratio that tripped the 1.6x tolerance
# purely from thread overhead, not a real seriality regression).
# 20,000,000 keeps release at ~1s and the slowest build variant here
# (unoptimized debug plus ASan/UBSan or TSan instrumentation) at
# ~15-20s -- this used to be 200,000,000, which took minutes per build
# variant under a sanitizer and made test-all painfully slow for no
# extra coverage.
spin_program='def spin()
  i = 0
  while i < 20000000
    i = i + 1
  end
  i
end'
start="$EPOCHREALTIME"
serial_out="$("$diamond" -e "$spin_program
puts(spin())")"
end="$EPOCHREALTIME"
serial_time="$(echo "$end - $start" | bc)"

start="$EPOCHREALTIME"
parallel_out="$("$diamond" -e "$spin_program
t1 = Thread.new(spin)
t2 = Thread.new(spin)
puts(t1.join())
puts(t2.join())")"
end="$EPOCHREALTIME"
parallel_time="$(echo "$end - $start" | bc)"

if [[ "$serial_out" != $'20000000\nnil' || "$parallel_out" != $'20000000\n20000000\nnil' ]]; then
    echo "FAIL: Thread real-parallelism proof (unexpected output)" >&2
    echo "  serial:   $serial_out" >&2
    echo "  parallel: $parallel_out" >&2
    exit 1
fi
if (( $(echo "$cpu_budget >= 2" | bc -l) )); then
    if ! (( $(echo "$parallel_time < $serial_time * 1.6" | bc -l) )); then
        echo "FAIL: Thread real-parallelism proof (not actually parallel)" >&2
        echo "  serial time (1 spin):    ${serial_time}s" >&2
        echo "  parallel time (2 spins): ${parallel_time}s" >&2
        exit 1
    fi
else
    echo "NOTE: skipping Thread real-parallelism timing assertion -- cgroup CPU budget (${cpu_budget}) is under 2 cores, so two threads have no real room to run concurrently here" >&2
fi

# File-based test cases: tests/cases/<name>.di paired with:
#   <name>.expected          -- exact stdout match (trailing newline
#                                stripped the same way $() strips it on
#                                both sides)
#   <name>.expected_error    -- a substring expected in combined
#                                stdout+stderr, for compile/runtime-error
#                                regressions (can't share a program with a
#                                successful test, so each stays its own
#                                file)
#   <name>.expected_contains -- one or more grep -q patterns (BRE, one per
#                                line, matched with real grep so ^/$
#                                anchors and other regex metacharacters
#                                behave exactly like the inline grep -q
#                                checks this convention replaces), all of
#                                which must match somewhere against
#                                combined stdout+stderr; success still
#                                expected. For multi-pattern checks
#                                (bytecode disassembly, quickening traces)
#                                that don't reduce to one exact string.
#   <name>.expected_lastline  -- exact match against just the final line
#                                of output (e.g. --dump-bytecode's
#                                disassembly followed by the program's own
#                                result on the last line); may accompany
#                                .expected_contains
#   <name>.env                -- optional, KEY=VALUE per line, exported
#                                 for just this one case
#   <name>.flags               -- optional, extra CLI flags (one per line)
#                                 inserted before the file argument, e.g.
#                                 --dump-bytecode
# Minitest cases instead end with `suite.run!()` and need no sidecar: the
# uncaught failure makes their exit code nonzero, while output is informational.
# This is the newer convention going forward, growing this directory
# instead of this file; the inline -e assertions above are the older
# convention, most of them predating tests/cases/*.expected existing.
#
# All cases run up front in one batch via build/run_cases (tests/run_cases.c),
# which runs every case in a single process instead of this loop spawning a
# fresh `diamond` per file (see docs/roadmap.md) -- .env/.flags handling
# happens inside run_cases now, and this loop just reads back the .stdout/
# .combined/.exitcode files it wrote per case with bash's own $(<file), which
# strips a trailing newline the same way $(cat ...) used to.
case_output_dir="build/case_output"
"$run_cases_abs" tests/cases "$case_output_dir"

case_count=0
for case_file in tests/cases/*.di; do
    case_name="${case_file%.di}"
    case_base="$(basename "$case_name")"
    if [[ -f "$case_name.expected" ]]; then
        actual="$(<"$case_output_dir/$case_base.stdout")"
        exit_code="$(<"$case_output_dir/$case_base.exitcode")"
        expected="$(cat "$case_name.expected")"
        if [[ "$exit_code" != "0" ]]; then
            echo "FAIL: $case_file" >&2
            echo "  expected exit code 0, got: $exit_code" >&2
            exit 1
        fi
        if [[ "$actual" != "$expected" ]]; then
            echo "FAIL: $case_file" >&2
            echo "  expected: $expected" >&2
            echo "  actual:   $actual" >&2
            exit 1
        fi
        case_count=$((case_count + 1))
    elif [[ -f "$case_name.expected_error" ]]; then
        actual="$(<"$case_output_dir/$case_base.combined")"
        pattern="$(cat "$case_name.expected_error")"
        if [[ "$actual" != *"$pattern"* ]]; then
            echo "FAIL: $case_file" >&2
            echo "  expected error containing: $pattern" >&2
            echo "  actual: $actual" >&2
            exit 1
        fi
        case_count=$((case_count + 1))
    elif [[ -f "$case_name.expected_contains" || -f "$case_name.expected_lastline" ]]; then
        actual="$(<"$case_output_dir/$case_base.combined")"
        if [[ -f "$case_name.expected_contains" ]]; then
            while IFS= read -r pattern; do
                [[ -z "$pattern" ]] && continue
                if ! grep -q -- "$pattern" <<<"$actual"; then
                    echo "FAIL: $case_file" >&2
                    echo "  expected output to match: $pattern" >&2
                    echo "  actual: $actual" >&2
                    exit 1
                fi
            done < "$case_name.expected_contains"
        fi
        if [[ -f "$case_name.expected_lastline" ]]; then
            last_line="${actual##*$'\n'}"
            expected_last="$(cat "$case_name.expected_lastline")"
            if [[ "$last_line" != "$expected_last" ]]; then
                echo "FAIL: $case_file" >&2
                echo "  expected last line: $expected_last" >&2
                echo "  actual last line:   $last_line" >&2
                exit 1
            fi
        fi
        case_count=$((case_count + 1))
    elif grep -q 'suite\.run!()' "$case_file"; then
        exit_code="$(<"$case_output_dir/$case_base.exitcode")"
        if [[ "$exit_code" != "0" ]]; then
            actual="$(<"$case_output_dir/$case_base.combined")"
            echo "FAIL: $case_file" >&2
            echo "  expected exit code 0, got: $exit_code" >&2
            echo "  actual: $actual" >&2
            exit 1
        fi
        case_count=$((case_count + 1))
    fi
done

inline_count="$(grep -cE '^\[\[' "$0")"
echo "$((inline_count + case_count)) tests passed"
