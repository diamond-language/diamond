#!/usr/bin/env bash
set -euo pipefail

diamond=./build/diamond

actual="$($diamond -e '20 + 22')"
[[ "$actual" == "42" ]] || {
    echo "expected expression result 42, got: $actual" >&2
    exit 1
}

actual="$($diamond --version)"
[[ "$actual" == "diamond 0.1.0-dev" ]] || {
    echo "unexpected version output: $actual" >&2
    exit 1
}

actual="$($diamond tests/cases/arithmetic.di)"
[[ "$actual" == "42" ]] || {
    echo "expected file result 42, got: $actual" >&2
    exit 1
}

actual="$($diamond -e '2 + 3 * 4 - -1')"
[[ "$actual" == "15" ]] || {
    echo "precedence or unary arithmetic failed: $actual" >&2
    exit 1
}

quickening_trace="$(DIAMOND_QUICKEN=1 DIAMOND_TRACE_QUICKEN=1 \
    "$diamond" -e $'def add(a,b)=a+b\nadd(20,22)' 2>&1)"
grep -q 'quickened sites: 1' <<<"$quickening_trace"
grep -q '^42$' <<<"$quickening_trace"
quickening_trace="$(DIAMOND_QUICKEN=1 DIAMOND_TRACE_QUICKEN=1 \
    "$diamond" -e $'def add(a,b)=a+b\n[add(20,22), add("a","b")]' 2>&1)"
grep -q 'quickened sites: 1, deoptimized sites: 1' <<<"$quickening_trace"
grep -q '\[42, ab\]' <<<"$quickening_trace"
quickening_trace="$(DIAMOND_QUICKEN=1 DIAMOND_TRACE_QUICKEN=1 \
    "$diamond" -e $'def arithmetic(a,b) = a-b\narithmetic(20,2)' 2>&1)"
grep -q 'quickened sites: 1, deoptimized sites: 0' <<<"$quickening_trace"
grep -q '^18$' <<<"$quickening_trace"

quickening_trace="$(DIAMOND_QUICKEN=1 DIAMOND_TRACE_QUICKEN=1 \
    "$diamond" -e $'def arithmetic(a,b) = a*b\narithmetic(20,2)' 2>&1)"
grep -q 'quickened sites: 1, deoptimized sites: 0' <<<"$quickening_trace"
grep -q '^40$' <<<"$quickening_trace"

quickening_trace="$(DIAMOND_QUICKEN=1 DIAMOND_TRACE_QUICKEN=1 \
    "$diamond" -e $'def arithmetic(a,b) = a/b\narithmetic(20,2)' 2>&1)"
grep -q 'quickened sites: 1, deoptimized sites: 0' <<<"$quickening_trace"
grep -q '^10$' <<<"$quickening_trace"

quickening_trace="$(DIAMOND_QUICKEN=1 DIAMOND_TRACE_QUICKEN=1 \
    "$diamond" -e $'def comparison(a,b) = a < b\ncomparison(2,3)' 2>&1)"
grep -q 'quickened sites: 1, deoptimized sites: 0' <<<"$quickening_trace"
grep -q '^true$' <<<"$quickening_trace"

threshold_trace="$(DIAMOND_QUICKEN=1 DIAMOND_QUICKEN_THRESHOLD=2 \
    DIAMOND_TRACE_QUICKEN=1 "$diamond" -e $'def threshold(a,b) = a+b\nthreshold(1,2)\nthreshold(3,4)' 2>&1)"
grep -q 'quickened sites: 1, deoptimized sites: 0' <<<"$threshold_trace"
grep -q '^7$' <<<"$threshold_trace"

fallback_trace="$(DIAMOND_QUICKEN=1 DIAMOND_QUICKEN_THRESHOLD=invalid \
    DIAMOND_TRACE_QUICKEN=1 "$diamond" -e $'def fallback(a,b) = a+b\nfallback(1,2)' 2>&1)"
grep -q 'quickened sites: 1, deoptimized sites: 0' <<<"$fallback_trace"
grep -q '^3$' <<<"$fallback_trace"

equality_trace="$(DIAMOND_QUICKEN=1 DIAMOND_TRACE_QUICKEN=1 \
    "$diamond" -e $'def equality(a,b) = a==b\nequality(4,4)' 2>&1)"
grep -q 'quickened sites: 1, deoptimized sites: 0' <<<"$equality_trace"
grep -q '^true$' <<<"$equality_trace"

equality_trace="$(DIAMOND_QUICKEN=1 DIAMOND_TRACE_QUICKEN=1 \
    "$diamond" -e $'def equality(a,b) = a != b\nequality(4,5)' 2>&1)"
grep -q 'quickened sites: 1, deoptimized sites: 0' <<<"$equality_trace"
grep -q '^true$' <<<"$equality_trace"

equality_trace="$(DIAMOND_QUICKEN=1 DIAMOND_TRACE_QUICKEN=1 \
    "$diamond" -e $'def equality(a,b) = a==b\n[equality(4,4), equality("x","x")]' 2>&1)"
grep -q 'quickened sites: 1, deoptimized sites: 1' <<<"$equality_trace"
grep -q '^\[true, true\]$' <<<"$equality_trace"

quickening_trace="$(DIAMOND_QUICKEN=1 DIAMOND_TRACE_QUICKEN=1 \
    "$diamond" -e $'def comparison(a,b) = a >= b\ncomparison(3,3)' 2>&1)"
grep -q 'quickened sites: 1, deoptimized sites: 0' <<<"$quickening_trace"
grep -q '^true$' <<<"$quickening_trace"

[[ "$("$diamond" -e $'20-2')" == 18 ]]
[[ "$("$diamond" -e $'20*2')" == 40 ]]
[[ "$("$diamond" -e $'20/2')" == 10 ]]

error_file="$(mktemp)"
if "$diamond" -e '1 + )' 2>"$error_file"; then
    echo "invalid source unexpectedly succeeded" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q "error: expected expression" "$error_file"
rm -f "$error_file"

if "$diamond" -e '1 / 0' >/dev/null 2>&1; then
    echo "division by zero unexpectedly succeeded" >&2
    exit 1
fi

error_file="$(mktemp)"
if "$diamond" -e '9223372036854775807 + 1' >/dev/null 2>"$error_file"; then
    echo "integer add overflow unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: integer overflow' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'x = -9223372036854775807 - 1\nx + -1' >/dev/null 2>"$error_file"; then
    echo "integer add negative overflow unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: integer overflow' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'x = -9223372036854775807 - 1\nx - 1' >/dev/null 2>"$error_file"; then
    echo "integer subtract overflow unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: integer overflow' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e '9223372036854775807 * 2' >/dev/null 2>"$error_file"; then
    echo "integer multiply overflow unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: integer overflow' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'x = -9223372036854775807 - 1\nx / -1' >/dev/null 2>"$error_file"; then
    echo "integer divide overflow unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: integer overflow' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if "$diamond" -e $'x = -9223372036854775807 - 1\n-x' >/dev/null 2>"$error_file"; then
    echo "integer negate overflow unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: integer overflow' "$error_file"
rm -f "$error_file"

actual="$("$diamond" -e $'begin\n 9223372036854775807 + 1\nrescue error: RangeError\n error.message()\nend')"
[[ "$actual" == "integer overflow" ]]

error_file="$(mktemp)"
if DIAMOND_QUICKEN=1 "$diamond" -e \
    $'def add(a, b) = a + b\nadd(1, 2)\nadd(9223372036854775807, 1)' >/dev/null 2>"$error_file"; then
    echo "quickened integer overflow unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: integer overflow' "$error_file"
rm -f "$error_file"

actual="$($diamond tests/cases/control_flow.di)"
[[ "$actual" == "42" ]] || {
    echo "control flow program failed: $actual" >&2
    exit 1
}

actual="$($diamond -e $'if false\n  1\nelse\n  2\nend')"
[[ "$actual" == "2" ]] || {
    echo "false branch failed: $actual" >&2
    exit 1
}

if "$diamond" -e 'missing + 1' >/dev/null 2>&1; then
    echo "undefined local unexpectedly succeeded" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode -e '20 + 22')"
grep -q 'CONSTANT.*k0 (20)' <<<"$actual"
grep -q 'ADD' <<<"$actual"
grep -q 'RETURN' <<<"$actual"
[[ "${actual##*$'\n'}" == "42" ]] || {
    echo "disassembled program did not execute to 42" >&2
    exit 1
}

actual="$($diamond tests/cases/functions.di)"
[[ "$actual" == "42" ]] || {
    echo "function or recursion test failed: $actual" >&2
    exit 1
}

if "$diamond" -e $'def one(a)\n  a\nend\none()' >/dev/null 2>&1; then
    echo "wrong function arity unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode tests/cases/functions.di)"
grep -q '^== factorial ==$' <<<"$actual"
grep -q 'CALL' <<<"$actual"
[[ "${actual##*$'\n'}" == "42" ]] || {
    echo "disassembled function program did not execute to 42" >&2
    exit 1
}

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/strings.di)"
[[ "$actual" == "[hahahahahahahaha]" ]] || {
    echo "string or stress-GC test failed: $actual" >&2
    exit 1
}

actual="$("$diamond" -e '"same" == "same"')"
[[ "$actual" == "true" ]] || {
    echo "string value equality failed: $actual" >&2
    exit 1
}

actual="$("$diamond" --dump-bytecode -e '"dia" + "mond"')"
grep -q 'STRING.*s0 ("dia")' <<<"$actual"
[[ "${actual##*$'\n'}" == "diamond" ]] || {
    echo "disassembled string program failed" >&2
    exit 1
}

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/classes.di)"
[[ "$actual" == "hello, diamond" ]] || {
    echo "class, method, field, or stress-GC test failed: $actual" >&2
    exit 1
}

actual="$("$diamond" --dump-bytecode tests/cases/classes.di)"
grep -q 'NEW' <<<"$actual"
grep -q 'INVOKE' <<<"$actual"
grep -q 'GET_IVAR' <<<"$actual"
grep -q 'SET_IVAR' <<<"$actual"

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/inheritance.di)"
[[ "$actual" == "hello> diamond" ]] || {
    echo "inheritance, override, self, or inherited constructor failed: $actual" >&2
    exit 1
}

if "$diamond" -e $'class Child < Missing\nend' >/dev/null 2>&1; then
    echo "undefined superclass unexpectedly compiled" >&2
    exit 1
fi

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/super.di)"
[[ "$actual" == "hello> diamond!" ]] || {
    echo "super method or constructor chaining failed: $actual" >&2
    exit 1
}

actual="$("$diamond" --dump-bytecode tests/cases/super.di)"
grep -q 'SUPER' <<<"$actual"

if "$diamond" -e $'class Root\n  def value()\n    super()\n  end\nend' >/dev/null 2>&1; then
    echo "super without a superclass unexpectedly compiled" >&2
    exit 1
fi

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/types.di)"
[[ "$actual" == "Ruby: 42" ]] || {
    echo "typed primitive, class, or subtype boundary failed: $actual" >&2
    exit 1
}

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

actual="$("$diamond" --dump-bytecode -e $'def add(x: Int) -> Int\n  x + 1\nend\nadd(41)')"
grep -q 'CHECK_TYPE' <<<"$actual"

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/nilable_types.di)"
[[ "$actual" == "#<ChildRecord>" ]] || {
    echo "nilable nominal subtype check failed: $actual" >&2
    exit 1
}

actual="$("$diamond" -e $'def accept(x: Int | Nil) -> Int | Nil\n  x\nend\naccept(nil)')"
[[ "$actual" == "nil" ]] || {
    echo "nilable parameter rejected nil: $actual" >&2
    exit 1
}

if "$diamond" -e $'def accept(x: Int | Nil)\n x\nend\naccept("bad")' >/dev/null 2>&1; then
    echo "nilable parameter accepted wrong non-nil type" >&2
    exit 1
fi

actual="$("$diamond" -e $'def accept(x: Int | String) -> Int | String\n x\nend\naccept("diamond")')"
[[ "$actual" == "diamond" ]]

error_file="$(mktemp)"
if "$diamond" -e $'def accept(x: Int | String)\n x\nend\naccept(true)' \
    >/dev/null 2>"$error_file"; then
    echo "general union accepted an unrelated type" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'expected Int | String, got Bool' "$error_file"
rm -f "$error_file"

actual="$("$diamond" --dump-bytecode -e $'def accept(x: Int | String)\n x\nend\naccept(42)')"
grep -q 'CHECK_TYPE.*Int | String' <<<"$actual"

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/general_unions.di)"
[[ "$actual" == "diamond" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/array_generics.di)"
[[ "$actual" == "42" ]]

error_file="$(mktemp)"
if "$diamond" -e $'def ints(values: Array[Int])\n values\nend\nints([1, "bad"])' \
    >/dev/null 2>"$error_file"; then
    echo "Array[Int] accepted an existing String element" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'expected Array\[Int\], got Array' "$error_file"
rm -f "$error_file"

actual="$("$diamond" --dump-bytecode -e $'def nested(values: Array[Array[Int | Nil]])\n values\nend\nnested([[nil]])')"
grep -q 'Array\[Array\[Int | Nil\]\]' <<<"$actual"

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/hash_generics.di)"
[[ "$actual" == "42" ]]

error_file="$(mktemp)"
if "$diamond" -e $'def scores(values: Hash[String, Int])\n values\nend\nscores({"ok": "bad"})' \
    >/dev/null 2>"$error_file"; then
    echo "Hash[String, Int] accepted an existing String value" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'expected Hash\[String, Int\], got Hash' "$error_file"
rm -f "$error_file"

actual="$("$diamond" --dump-bytecode -e $'def nested(values: Hash[String, Array[Int | Nil]])\n values\nend\nnested({"items": [nil]})')"
grep -q 'Hash\[String, Array\[Int | Nil\]\]' <<<"$actual"

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e 'array_first([42])')"
[[ "$actual" == "42" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e 'array_swap_first_two([20, 22])')"
[[ "$actual" == "[22, 20]" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e 'hash_fetch({"answer": 42}, "answer", 0)')"
[[ "$actual" == "42" ]]

actual="$("$diamond" -e 'hash_fetch({}, "missing", 42)')"
[[ "$actual" == "42" ]]

actual="$("$diamond" --dump-bytecode -e 'array_first([42])')"
grep -q '^== array_first ==$' <<<"$actual"
grep -q 'INDEX_GET' <<<"$actual"

actual="$("$diamond" -e '[20, 22].length()')"
[[ "$actual" == "2" ]]

actual="$("$diamond" -e '{"answer": 42}.length()')"
[[ "$actual" == "1" ]]

actual="$("$diamond" -e '"diamond".length()')"
[[ "$actual" == "7" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e 'array_first_or([], 40) + array_last([1, 2])')"
[[ "$actual" == "42" ]]

actual="$("$diamond" -e 'array_last_or([], 42)')"
[[ "$actual" == "42" ]]

actual="$("$diamond" -e 'array_empty([]) && hash_empty({})')"
[[ "$actual" == "true" ]]

actual="$("$diamond" -e $'begin\n [1].length(2)\nrescue error: ArgumentError\n 42\nend')"
[[ "$actual" == "42" ]]

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

actual="$("$diamond" -e $'class Foo\n def bar(a, b)\n  a + b\n end\nend\nbegin\n Foo.new().bar(1)\nrescue error: ArgumentError\n error.message()\nend')"
[[ "$actual" == "wrong number of arguments" ]]

actual="$("$diamond" -e $'class Foo\n def initialize(a, b)\n  @a = a\n end\nend\nbegin\n Foo.new(1)\nrescue error: ArgumentError\n 42\nend')"
[[ "$actual" == "42" ]]

error_file="$(mktemp)"
if "$diamond" -e $'class Foo\n def bar(a, b)\n  a + b\n end\nend\nf = Foo.new()\nf.bar(1, 2)\nf.bar(1, 2)\nf.bar(1, 2)\nf.bar(1)' \
    >/dev/null 2>"$error_file"; then
    echo "monomorphic dispatch with wrong arity unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: wrong number of arguments' "$error_file"
rm -f "$error_file"

actual="$("$diamond" -e $'class Foo\n def bar(a, b)\n  a + b\n end\nend\nclass Baz\n def bar(a)\n  a\n end\nend\ndef call_it(x)\n x.bar(1)\nend\ncall_it(Baz.new())\nbegin\n call_it(Foo.new())\nrescue error: ArgumentError\n 42\nend')"
[[ "$actual" == "42" ]]

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

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e $'values = []\nindex = 0\nwhile index < 20\n values.push(index)\n index = index + 1\nend\nvalues.length()')"
[[ "$actual" == "20" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e $'values = [20, 22]\nlast = values.pop()\nlast + values.length() + 19')"
[[ "$actual" == "42" ]]

actual="$("$diamond" -e '[].pop()')"
[[ "$actual" == "nil" ]]

actual="$("$diamond" -e $'def checked(values: Array[Int])\n values\nend\nvalues = []\nchecked(values)\nbegin\n values.push("bad")\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$("$diamond" -e $'def checked(values: Array[Array[Int]])\n values\nend\nouter = []\ninner = []\nchecked(outer)\nouter.push(inner)\nbegin\n inner.push("bad")\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$("$diamond" -e 'array_include([20, 22], 22)')"
[[ "$actual" == "true" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e $'def mapped()\n def double(value)\n  value * 2\n end\n array_map([10, 11], double)\nend\nmapped()')"
[[ "$actual" == "[20, 22]" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e $'def total()\n sum = 0\n def add(value)\n  sum = sum + value\n end\n array_each([20, 22], add)\n sum\nend\ntotal()')"
[[ "$actual" == "42" ]]

actual="$("$diamond" --dump-bytecode -e $'def present(value: String | Nil) -> String\n if value != nil\n  value\n else\n  "fallback"\n end\nend\npresent(nil)')"
present_dump="$(sed -n '/^== present ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$present_dump")" == "1" ]]

actual="$("$diamond" --dump-bytecode -e $'def lookup(values: Hash[String, Int]) -> Int\n value = values["answer"]\n if value == nil\n  0\n else\n  value\n end\nend\nlookup({"answer": 42})')"
lookup_dump="$(sed -n '/^== lookup ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$lookup_dump")" == "1" ]]

actual="$("$diamond" -e $'def answer(value: Int | Nil) -> Int\n if value != nil\n  value\n else\n  42\n end\nend\nanswer(nil)')"
[[ "$actual" == "42" ]]

actual="$("$diamond" --dump-bytecode -e $'def unstable(value: String | Nil, flag: Bool) -> String\n if flag\n  value = "changed"\n end\n value\nend\nunstable("ok", false)')"
unstable_dump="$(sed -n '/^== unstable ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$unstable_dump")" == "3" ]]

actual="$("$diamond" -e '{"first": 20, "second": 22}.key_at(1)')"
[[ "$actual" == "second" ]]

actual="$("$diamond" -e '{"first": 20, "second": 22}.value_at(1)')"
[[ "$actual" == "22" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e 'hash_keys({"first": 20, "second": 22})')"
[[ "$actual" == "[first, second]" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e 'hash_values({"first": 20, "second": 22})')"
[[ "$actual" == "[20, 22]" ]]

actual="$("$diamond" -e 'hash_include_key({"answer": 42}, "answer")')"
[[ "$actual" == "true" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e $'def sum_values()\n total = 0\n def add(key, value)\n  total = total + value\n end\n hash_each({"a": 20, "b": 22}, add)\n total\nend\nsum_values()')"
[[ "$actual" == "42" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e $'def transform()\n def double(value)\n  value * 2\n end\n hash_map_values({"a": 20, "b": 1}, double)\nend\ntransform()')"
[[ "$actual" == "{a: 40, b: 2}" ]]

actual="$("$diamond" -e $'begin\n {}.key_at(0)\nrescue error: IndexError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$("$diamond" -e '42 is Int')"
[[ "$actual" == "true" ]]

actual="$("$diamond" -e '42 is String')"
[[ "$actual" == "false" ]]

actual="$("$diamond" --dump-bytecode -e $'def text(value: String | Int) -> String\n if value is String\n  value\n else\n  "number"\n end\nend\ntext(42)')"
text_dump="$(sed -n '/^== text ==$/,$p' <<<"$actual")"
grep -q 'IS_TYPE.*String' <<<"$text_dump"
[[ "$(grep -c 'CHECK_TYPE' <<<"$text_dump")" == "1" ]]

actual="$("$diamond" --dump-bytecode -e $'def number(value: String | Int) -> Int\n if value is String\n  0\n else\n  value\n end\nend\nnumber(42)')"
number_dump="$(sed -n '/^== number ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$number_dump")" == "1" ]]

actual="$("$diamond" -e $'class Animal\nend\nclass Dog < Animal\nend\nDog.new() is Animal')"
[[ "$actual" == "true" ]]

actual="$("$diamond" --dump-bytecode -e $'def compound(value: String | Int, flag: Bool) -> String\n if value is String && flag\n  value\n else\n  "fallback"\n end\nend\ncompound("ok", true)')"
compound_dump="$(sed -n '/^== compound ==$/,$p' <<<"$actual")"
[[ "$(grep -c 'CHECK_TYPE' <<<"$compound_dump")" == "3" ]]

if "$diamond" -e '42 is Missing' >/dev/null 2>&1; then
    echo "is accepted an unknown type" >&2
    exit 1
fi

actual="$("$diamond" -e $'def run()\n def identity(value)\n  value\n end\n array_each([], identity).length()\nend\nrun()')"
[[ "$actual" == "0" ]]

actual="$("$diamond" -e $'def run()\n def wrong()\n  42\n end\n begin\n  array_each([], wrong)\n rescue error: TypeError\n  42\n end\nend\nrun()')"
[[ "$actual" == "42" ]]

error_file="$(mktemp)"
if "$diamond" -e $'def run()\n def wrong()\n  42\n end\n array_map([], wrong)\nend\nrun()' \
    >/dev/null 2>"$error_file"; then
    echo "Callable[1] accepted a zero-arity closure" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'expected Callable\[1\], got Callable' "$error_file"
rm -f "$error_file"

actual="$("$diamond" -e $'def run()\n def pair(key, value)\n  key\n end\n hash_each({}, pair).length()\nend\nrun()')"
[[ "$actual" == "0" ]]

actual="$("$diamond" --dump-bytecode -e '42')"
array_each_dump="$(sed -n '/^== array_each ==$/,/^== /p' <<<"$actual")"
grep -q 'CHECK_TYPE.*Callable\[1\]' <<<"$array_each_dump"

if "$diamond" -e $'def invalid(callback: Callable[17])\n callback\nend' \
    >/dev/null 2>&1; then
    echo "oversized Callable arity unexpectedly compiled" >&2
    exit 1
fi

actual="$("$diamond" --dump-bytecode -e $'def maybe(x: String | Nil) -> String | Nil\n x\nend\nmaybe(nil)')"
grep -q 'String | Nil' <<<"$actual"

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

actual="$("$diamond" --dump-bytecode -e $'def dynamic(x) -> Int\n  x\nend\ndynamic(42)')"
grep -q 'CHECK_TYPE.*Int' <<<"$actual"

actual="$("$diamond" --dump-bytecode -e $'def absent() -> String | Nil\n  nil\nend\nabsent()')"
absent_dump="$(sed -n '/^== absent ==$/,$p' <<<"$actual")"
if grep -q 'CHECK_TYPE' <<<"$absent_dump"; then
    echo "provably nil return retained a nilable guard" >&2
    exit 1
fi

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/arrays.di)"
[[ "$actual" == "22" ]] || {
    echo "array indexing, typing, or recursive GC tracing failed: $actual" >&2
    exit 1
}

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e '[["diamond"], [42], []]')"
[[ "$actual" == "[[diamond], [42], []]" ]] || {
    echo "nested array printing failed: $actual" >&2
    exit 1
}

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

actual="$("$diamond" --dump-bytecode -e '[20, 22][1]')"
grep -q 'ARRAY' <<<"$actual"
grep -q 'INDEX_GET' <<<"$actual"

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/array_mutation.di)"
[[ "$actual" == "survived" ]] || {
    echo "array mutation or post-frame GC tracing failed: $actual" >&2
    exit 1
}

actual="$("$diamond" -e $'values = [20, 0]\nvalues[1] = 22\nvalues[0] + values[1]')"
[[ "$actual" == "42" ]] || {
    echo "indexed assignment did not update array: $actual" >&2
    exit 1
}

error_file="$(mktemp)"
if "$diamond" -e $'values = [1]\nvalues[-1] = 2' >/dev/null 2>"$error_file"; then
    echo "negative indexed assignment unexpectedly succeeded" >&2
    rm -f "$error_file"
    exit 1
fi
grep -q 'index -1 out of bounds for Array of length 1' "$error_file"
rm -f "$error_file"

actual="$("$diamond" --dump-bytecode -e $'values = [0]\nvalues[0] = 42\nvalues')"
grep -q 'INDEX_SET' <<<"$actual"

actual="$("$diamond" -e 'false && (1 / 0)')"
[[ "$actual" == "false" ]] || {
    echo "&& did not short-circuit or preserve false operand" >&2
    exit 1
}

actual="$("$diamond" -e 'true || (1 / 0)')"
[[ "$actual" == "true" ]] || {
    echo "|| did not short-circuit or preserve true operand" >&2
    exit 1
}

actual="$("$diamond" -e 'nil || "fallback"')"
[[ "$actual" == "fallback" ]] || {
    echo "|| did not return its evaluated right operand" >&2
    exit 1
}

actual="$("$diamond" -e 'false || true && 42')"
[[ "$actual" == "42" ]] || {
    echo "&& and || precedence is incorrect" >&2
    exit 1
}

actual="$("$diamond" -e '!nil')"
[[ "$actual" == "true" ]] || {
    echo "truthiness negation failed" >&2
    exit 1
}

actual="$("$diamond" --dump-bytecode -e 'nil || 42')"
grep -q 'JUMP_IF_TRUE' <<<"$actual"

actual="$("$diamond" tests/cases/returns.di)"
[[ "$actual" == "4" ]] || {
    echo "nested explicit return did not exit its function: $actual" >&2
    exit 1
}

actual="$("$diamond" -e $'def empty() -> Nil\n  return\nend\nempty()')"
[[ "$actual" == "nil" ]] || {
    echo "bare return did not produce nil: $actual" >&2
    exit 1
}

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

actual="$("$diamond" tests/cases/loop_control.di)"
[[ "$actual" == "12" ]] || {
    echo "break or next produced the wrong loop result: $actual" >&2
    exit 1
}

actual="$("$diamond" -e $'outer = 0\ntotal = 0\nwhile outer < 3\n  outer = outer + 1\n  inner = 0\n  while true\n    inner = inner + 1\n    if inner == 2\n      break\n    end\n    total = total + 1\n  end\nend\ntotal')"
[[ "$actual" == "3" ]] || {
    echo "nested break targeted the wrong loop: $actual" >&2
    exit 1
}

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

actual="$("$diamond" tests/cases/source_ergonomics.di)"
[[ "$actual" == "42" ]] || {
    echo "comments, separators, or trailing commas failed: $actual" >&2
    exit 1
}

actual="$("$diamond" -e '1_000_000 + 24')"
[[ "$actual" == "1000024" ]] || {
    echo "numeric digit separators decoded incorrectly: $actual" >&2
    exit 1
}

actual="$("$diamond" -e '"# not a comment"')"
[[ "$actual" == "# not a comment" ]] || {
    echo "comment marker inside string was misinterpreted: $actual" >&2
    exit 1
}

if "$diamond" -e '1__000' >/dev/null 2>&1; then
    echo "malformed numeric separators unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e '1_' >/dev/null 2>&1; then
    echo "trailing numeric separator unexpectedly compiled" >&2
    exit 1
fi

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/hashes.di)"
[[ "$actual" == "42" ]] || {
    echo "hash lookup, mutation, typing, or tracing failed: $actual" >&2
    exit 1
}

actual="$("$diamond" -e '{"name": "Diamond", "answer": 42,}')"
[[ "$actual" == "{name: Diamond, answer: 42}" ]] || {
    echo "hash literal or printing failed: $actual" >&2
    exit 1
}

actual="$("$diamond" -e $'data = {}\ndata["new"] = 42\ndata["new"]')"
[[ "$actual" == "42" ]] || {
    echo "hash insertion failed: $actual" >&2
    exit 1
}

actual="$("$diamond" -e '{}["missing"]')"
[[ "$actual" == "nil" ]] || {
    echo "missing hash key did not return nil: $actual" >&2
    exit 1
}

if "$diamond" -e $'def typed(value: Hash)\n value\nend\ntyped([])' \
    >/dev/null 2>&1; then
    echo "Hash annotation accepted an Array" >&2
    exit 1
fi

actual="$("$diamond" --dump-bytecode -e '{"answer": 42}')"
grep -q 'HASH' <<<"$actual"

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

actual="$("$diamond" --dump-bytecode -e '40 + 2')"
grep -Eq '^[0-9]{4} +1:[0-9]+ +ADD' <<<"$actual"

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/closure_capture.di)"
[[ "$actual" == "47" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/mutable_closure.di)"
[[ "$actual" == "2" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/deep_closure.di)"
[[ "$actual" == "42" ]]

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

actual="$("$diamond" tests/cases/inherited_cache.di)"
[[ "$actual" == "160" ]]

actual="$("$diamond" tests/cases/mono_deopt.di)"
[[ "$actual" == "82" ]]

actual="$("$diamond" tests/cases/dispatch_benchmark.di)"
[[ "$actual" == "42" ]]

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

actual="$("$diamond" tests/cases/mixed_dispatch_workload.di)"
[[ "$actual" == "1500" ]]

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

actual="$("$diamond" -e $'def depth(n)\n if n <= 0\n  0\n else\n  depth(n - 1) + 1\n end\nend\ndepth(90)')"
[[ "$actual" == "90" ]]

error_file="$(mktemp)"
if "$diamond" -e $'def depth(n)\n if n <= 0\n  0\n else\n  depth(n - 1) + 1\n end\nend\ndepth(5000)' \
    >/dev/null 2>"$error_file"; then
    echo "deep recursion unexpectedly completed" >&2
    exit 1
fi
grep -q 'runtime error: call stack overflow' "$error_file"
rm -f "$error_file"

actual="$("$diamond" -e $'def depth(n)\n if n <= 0\n  0\n else\n  depth(n - 1) + 1\n end\nend\nbegin\n depth(5000)\nrescue error: SystemStackError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$("$diamond" --dump-bytecode -e 'raise 42' 2>/dev/null || true)"
grep -q 'RAISE' <<<"$actual"

actual="$($diamond --dump-bytecode -e $'class Foo\n def bar(a)\n  a\n end\n def self.make_patch()\n  def replacement(a)\n   a\n  end\n  replacement\n end\nend\nFoo.redefine_method("bar", Foo.make_patch())')"
grep -q 'REDEFINE_METHOD' <<<"$actual"

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

actual="$("$diamond" -e $'class Shape\n def initialize(width, height)\n  @width = width\n  @height = height\n end\n def area()\n  @width * @height\n end\n def self.square_area_patch()\n  def square_area()\n   @width * @width\n  end\n  square_area\n end\nend\ns = Shape.new(3, 4)\nbefore = s.area()\nShape.redefine_method("area", Shape.square_area_patch())\nafter = s.area()\n"#{before}, #{after}"')"
[[ "$actual" == "12, 9" ]]

actual="$("$diamond" -e $'class Shape\n def initialize(width, height)\n  @width = width\n  @height = height\n end\n def area()\n  @width * @height\n end\n def self.square_area_patch()\n  def square_area()\n   @width * @width\n  end\n  square_area\n end\nend\ns = Shape.new(3, 4)\ns.area()\ns.area()\ns.area()\nShape.redefine_method("area", Shape.square_area_patch())\ns.area()')"
[[ "$actual" == "9" ]]

error_file="$(mktemp)"
if "$diamond" -e $'class Foo\n def bar(a)\n  a\n end\n def self.make_patch()\n  def replacement(a, b)\n   a + b\n  end\n  replacement\n end\nend\nFoo.redefine_method("bar", Foo.make_patch())' \
    >/dev/null 2>"$error_file"; then
    echo "redefine_method with mismatched arity unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: wrong number of arguments' "$error_file"
rm -f "$error_file"

actual="$("$diamond" -e $'class Foo\n def bar(a)\n  a\n end\n def self.make_patch()\n  def replacement(a, b)\n   a + b\n  end\n  replacement\n end\nend\nbegin\n Foo.redefine_method("bar", Foo.make_patch())\nrescue error: ArgumentError\n 42\nend')"
[[ "$actual" == "42" ]]

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

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/rescue.di)"
[[ "$actual" == "diamond rescued!" ]]

actual="$("$diamond" -e $'begin\n 40 + 2\nrescue error\n 0\nend')"
[[ "$actual" == "42" ]]

actual="$("$diamond" --dump-bytecode tests/cases/rescue.di)"
grep -q 'PUSH_RESCUE' <<<"$actual"
grep -q 'POP_RESCUE' <<<"$actual"

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/typed_rescue.di)"
[[ "$actual" == "42" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/standard_exceptions.di)"
[[ "$actual" == "42" ]]

actual="$("$diamond" -e $'begin\n true + 1\nrescue error: StandardError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/ensure.di)"
[[ "$actual" == "42" ]]

actual="$("$diamond" -e $'begin\n begin\n  1 / 0\n ensure\n  40 + 2\n end\nrescue error: ZeroDivisionError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$("$diamond" --dump-bytecode -e $'begin\n 42\nensure\n nil\nend')"
grep -q 'PUSH_ENSURE' <<<"$actual"
grep -q 'RUN_ENSURE' <<<"$actual"
grep -q 'END_ENSURE' <<<"$actual"

actual="$("$diamond" -e $'def answer()\n begin\n  return 1\n ensure\n  return 42\n end\nend\nanswer()')"
[[ "$actual" == "42" ]]

actual="$("$diamond" -e $'begin\n begin\n  raise "old"\n ensure\n  raise 42\n end\nrescue error: Int\n error\nend')"
[[ "$actual" == "42" ]]

if "$diamond" -e $'begin\n raise 1\nrescue error: Int |\n 0\nend' \
    >/dev/null 2>&1; then
    echo "trailing rescue union unexpectedly compiled" >&2
    exit 1
fi

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e $'def run()\n def stringify(value) -> String\n  "ok"\n end\n array_map_string([1, 2], stringify)\nend\nrun()')"
[[ "$actual" == "[ok, ok]" ]]

actual="$($diamond -e $'def run()\n def stringify(value) -> String\n  "ok"\n end\n result = array_map_string([1], stringify)\n begin\n  result.push(42)\n rescue error: TypeError\n  42\n end\nend\nrun()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def run()\n def wrong(value) -> Int\n  value\n end\n begin\n  array_map_string([], wrong)\n rescue error: TypeError\n  42\n end\nend\nrun()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def run()\n def unknown(value)\n  "ok"\n end\n begin\n  array_map_string([], unknown)\n rescue error: TypeError\n  42\n end\nend\nrun()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Animal\nend\nclass Dog < Animal\nend\ndef run()\n def make_dog() -> Dog\n  Dog.new()\n end\n def accept(callback: Callable[0, Animal]) -> Animal\n  callback()\n end\n accept(make_dog)\nend\nrun()')"
[[ "$actual" == "#<Dog>" ]]

actual="$($diamond --dump-bytecode -e $'def accept(callback: Callable[1, String])\n callback(1)\nend')"
grep -q 'Callable\[1, String\]' <<<"$actual"

actual="$($diamond -e $'def size(value: Sized) -> Int\n value.length()\nend\n[size("abc"), size([1, 2]), size({"a": 1})]')"
[[ "$actual" == "[3, 2, 1]" ]]

actual="$($diamond -e $'class Box\n def length() -> Int\n  42\n end\nend\ndef size(value: Sized) -> Int\n value.length()\nend\n[size(Box.new()), Box.new() is Sized, 42 is Sized]')"
[[ "$actual" == "[42, true, false]" ]]

actual="$($diamond -e $'class Parent\n def length() -> Int\n  42\n end\nend\nclass Child < Parent\nend\ndef size(value: Sized) -> Int\n value.length()\nend\nsize(Child.new())')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def size(value: Sized) -> Int\n value.length()\nend\ndef dynamic(values: Array)\n begin\n  size(values[0])\n rescue error: TypeError\n  42\n end\nend\ndynamic([1])')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def length_or_zero(value: Sized | Nil) -> Int\n if value is Sized\n  value.length()\n else\n  0\n end\nend\n[length_or_zero([1, 2]), length_or_zero(nil)]')"
[[ "$actual" == "[2, 0]" ]]

actual="$($diamond --dump-bytecode -e $'def size(value: Sized)\n value.length()\nend')"
grep -q 'CHECK_TYPE.*Sized' <<<"$actual"

actual="$($diamond -e $'interface Greetable\n def greet(name)\nend\nclass Person\n def greet(name) -> String\n  name\n end\nend\ndef greet(value: Greetable) -> String\n value.greet("hi")\nend\n[greet(Person.new()), Person.new() is Greetable]')"
[[ "$actual" == "[hi, true]" ]]

actual="$($diamond -e $'interface Greetable\n def greet(name)\nend\nclass Wrong\n def greet()\n  "no"\n end\nend\nWrong.new() is Greetable')"
[[ "$actual" == "false" ]]

actual="$($diamond -e $'interface LengthLike\n def length()\nend\ndef size(value: LengthLike) -> Int\n value.length()\nend\n[size("abc"), size([1, 2]), size({"a": 1})]')"
[[ "$actual" == "[3, 2, 1]" ]]

actual="$($diamond -e $'interface Named\n def name()\nend\nclass Parent\n def name() -> String\n  "diamond"\n end\nend\nclass Child < Parent\nend\nChild.new() is Named')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'interface Greetable\n def greet(name)\nend\ndef accept(value: Greetable)\n value\nend\ndef dynamic(values: Array)\n begin\n  accept(values[0])\n rescue error: TypeError\n  42\n end\nend\ndynamic([1])')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'interface Named\n def name()\nend\nclass Person\n def name() -> String\n  "diamond"\n end\nend\ndef name_or_nil(value: Named | Nil)\n if value is Named\n  value.name()\n else\n  nil\n end\nend\n[name_or_nil(Person.new()), name_or_nil(nil)]')"
[[ "$actual" == "[diamond, nil]" ]]

actual="$($diamond --dump-bytecode -e $'interface Named\n def name()\nend\ndef accept(value: Named)\n value\nend')"
grep -q 'CHECK_TYPE.*Named' <<<"$actual"

actual="$($diamond -e $'class Animal\nend\nclass Dog < Animal\nend\ninterface Maker\n def make(value: Dog) -> Animal\nend\nclass Good\n def make(value: Animal) -> Dog\n  Dog.new()\n end\nend\nclass BadParameter\n def make(value: String) -> Dog\n  Dog.new()\n end\nend\nclass BadReturn\n def make(value: Animal) -> String\n  "no"\n end\nend\n[Good.new() is Maker, BadParameter.new() is Maker, BadReturn.new() is Maker]')"
[[ "$actual" == "[true, false, false]" ]]

actual="$($diamond -e $'interface Consumer\n def accept(value)\nend\nclass Typed\n def accept(value: Int)\n  value\n end\nend\nclass Dynamic\n def accept(value)\n  value\n end\nend\n[Typed.new() is Consumer, Dynamic.new() is Consumer]')"
[[ "$actual" == "[false, true]" ]]

actual="$($diamond -e $'interface StringMaker\n def make() -> String\nend\nclass Untyped\n def make()\n  "diamond"\n end\nend\nUntyped.new() is StringMaker')"
[[ "$actual" == "false" ]]

actual="$($diamond -e $'interface IntegerLength\n def length() -> Int\nend\ninterface StringLength\n def length() -> String\nend\n[[] is IntegerLength, [] is StringLength]')"
[[ "$actual" == "[true, false]" ]]

actual="$($diamond -e $'interface StringMaker\n def make() -> String\nend\nclass Wrong\n def make() -> Int\n  1\n end\nend\ndef accept(value: StringMaker)\n value\nend\ndef dynamic(values: Array)\n begin\n  accept(values[0])\n rescue error: TypeError\n  42\n end\nend\ndynamic([Wrong.new()])')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def answer() = 42\nanswer()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def identity(value: String) -> String = value\nidentity("diamond")')"
[[ "$actual" == "diamond" ]]

actual="$($diamond -e $'class Greeter\n def greet(name: String) -> String = name\nend\nGreeter.new().greet("diamond")')"
[[ "$actual" == "diamond" ]]

actual="$($diamond -e $'def outer(value)\n def captured() = value\n captured()\nend\nouter(42)')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'interface Maker\n def make() -> String\nend\nclass DiamondMaker\n def make() -> String = "diamond"\nend\nDiamondMaker.new() is Maker')"
[[ "$actual" == "true" ]]

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

actual="$($diamond -e $'def greet(name = "world") = name\n[greet(), greet(nil), greet("diamond")]')"
[[ "$actual" == "[world, nil, diamond]" ]]

actual="$($diamond -e $'def values(a = 20, b = a + 2) = [a, b]\n[values(), values(40), values(40, 2)]')"
[[ "$actual" == "[[20, 22], [40, 42], [40, 2]]" ]]

actual="$($diamond -e $'class Greeter\n def greet(name = "world") = name\nend\n[Greeter.new().greet(), Greeter.new().greet(nil)]')"
[[ "$actual" == "[world, nil]" ]]

actual="$($diamond -e $'class Point\n def initialize(value = 42)\n  @value = value\n end\n def value() = @value\nend\n[Point.new().value(), Point.new(7).value()]')"
[[ "$actual" == "[42, 7]" ]]

actual="$($diamond -e $'def run()\n def greet(name = "world") = name\n [greet(), greet("diamond")]\nend\nrun()')"
[[ "$actual" == "[world, diamond]" ]]

actual="$($diamond -e $'interface Greeter\n def greet(name: String) -> String\nend\nclass Friendly\n def greet(name: String = "world") -> String = name\nend\nFriendly.new() is Greeter')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'def typed(value: Int = 42) -> Int = value\nbegin\n typed(nil)\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

if "$diamond" -e $'def invalid(optional = 1, required) = required' \
    >/dev/null 2>&1; then
    echo "required parameter followed a default parameter" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode -e $'def answer(value = 42) = value\nanswer()')"
grep -q 'ARGUMENT_PROVIDED' <<<"$actual"

actual="$($diamond -e $'def greet(name = "world") = "Hello, #{name}"\n[greet(), greet("Diamond")]')"
[[ "$actual" == "[Hello, world, Hello, Diamond]" ]]

actual="$($diamond -e $'value = 42\n"x=#{value}, bool=#{true}, nil=#{nil}, math=#{value + 1}"')"
[[ "$actual" == "x=42, bool=true, nil=nil, math=43" ]]

actual="$($diamond -e $'"nested #{"text"}"')"
[[ "$actual" == "nested text" ]]

actual="$($diamond -e $'class Box\nend\n"value=#{Box.new()}"')"
[[ "$actual" == "value=#<Box>" ]]

actual="$($diamond -e $'"escaped \\#{42}"')"
[[ "$actual" == 'escaped #{42}' ]]

if "$diamond" -e $'"missing #{42"' >/dev/null 2>&1; then
    echo "unterminated interpolation unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'"empty #{}"' >/dev/null 2>&1; then
    echo "empty interpolation unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode -e $'value = 42\n"value=#{value}"')"
grep -q 'TO_STRING' <<<"$actual"

actual="$($diamond tests/multifile/main.di)"
[[ "$actual" == "[42, Hello, world]" ]]

actual="$($diamond tests/multifile/load_once_main.di)"
[[ "$actual" == "42" ]]

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

actual="$($diamond -e $'class User\n def initialize(name)\n  @name = name\n end\n def to_s() -> String = "User(#{@name})"\nend\n"hello #{User.new("Ada")}"')"
[[ "$actual" == "hello User(Ada)" ]]

actual="$($diamond -e $'class Box\nend\n"#{Box.new()}"')"
[[ "$actual" == "#<Box>" ]]

actual="$($diamond -e $'values = [1, "two", [true, nil]]\nmap = {"values": values}\n"#{map}"')"
[[ "$actual" == "{values: [1, two, [true, nil]]}" ]]

actual="$($diamond -e $'values = []\nvalues.push(values)\n"#{values}"')"
[[ "$actual" == "[[...]]" ]]

actual="$($diamond -e $'class Bad\n def to_s() = 42\nend\nbegin\n "#{Bad.new()}"\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Parent\n def to_s() -> String = "parent"\nend\nclass Child < Parent\nend\n"#{Child.new()}"')"
[[ "$actual" == "parent" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e $'class Item\n def to_s() -> String = "item"\nend\n"#{Item.new()} #{[1, 2]}"')"
[[ "$actual" == "item [1, 2]" ]]

actual="$($diamond -e $'def identity[T](value: T) -> T = value\n[identity(42), identity("diamond")]')"
[[ "$actual" == "[42, diamond]" ]]

actual="$($diamond -e $'def pair[K, V](key: K, value: V) -> Hash[K, V] = {key: value}\npair("answer", 42)')"
[[ "$actual" == "{answer: 42}" ]]

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

actual="$($diamond --dump-bytecode -e $'def first[T](values: Array[T]) -> T = values[0]\nfirst([42])')"
grep -q 'Array\[T0\]' <<<"$actual"

actual="$($diamond -e $'def run()\n def stringify(value: Int) -> String = "#{value}"\n result = array_map_typed([1, 2], stringify)\n begin\n  result.push(3)\n rescue error: TypeError\n  result\n end\nend\nrun()')"
[[ "$actual" == "[1, 2]" ]]

actual="$($diamond -e $'def run()\n def stringify(value: Int) -> String = "#{value}"\n result = array_map_typed([], stringify)\n begin\n  result.push(42)\n rescue error: TypeError\n  42\n end\nend\nrun()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def pair[K, V](key: K, value: V) -> Hash[K, V] = {key: value}\nresult = pair("answer", 42)\nbegin\n result[1] = 2\nrescue error: TypeError\n result["answer"]\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def pair[K, V](key: K, value: V) -> Hash[K, V] = {key: value}\nresult = pair("answer", 42)\nbegin\n result["other"] = "wrong"\nrescue error: TypeError\n result["answer"]\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Box\n def wrap[T](value: T) -> Array[T] = [value]\nend\nresult = Box.new().wrap("diamond")\nbegin\n result.push(42)\nrescue error: TypeError\n result\nend')"
[[ "$actual" == "[diamond]" ]]

actual="$($diamond -e $'class Animal\nend\nclass Dog < Animal\nend\ndef singleton[T](value: T) -> Array[T] = [value]\nresult = singleton(Dog.new())\nbegin\n result.push(Animal.new())\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e $'def singleton[T](value: T) -> Array[T] = [value]\nresult = singleton("diamond")\nbegin\n result.push(42)\nrescue error: TypeError\n result\nend')"
[[ "$actual" == "[diamond]" ]]

actual="$($diamond -e $'def bad[T](value: T) -> T = "wrong"\nbegin\n bad(42)\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def singleton[T](value: T) -> Array[T] = [value]\nresult = singleton(["diamond"])\nbegin\n result.push([42])\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def run()\n def words(value: Int) -> Array[String] = ["#{value}"]\n result = array_map_typed([], words)\n begin\n  result.push([42])\n rescue error: TypeError\n 42\n end\nend\nrun()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def singleton[T](value: T) -> Array[T] = [value]\nresult = singleton({"items": [1]})\nbegin\n result.push({"items": ["wrong"]})\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e $'def singleton[T](value: T) -> Array[T] = [value]\nresult = singleton([["diamond"]])\nbegin\n result.push([[42]])\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def run()\n def accepts(callback: Callable[[Int], String]) = callback(42)\n def stringify(value: Int) -> String = "#{value}"\n accepts(stringify)\nend\nrun()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def run()\n def accepts(callback: Callable[[Int], String]) = 1\n def wrong(value: String) -> String = value\n begin\n  accepts(wrong)\n rescue error: TypeError\n  42\n end\nend\nrun()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def run()\n def accepts(callback: Callable[[Int], String]) = callback(42)\n def broad(value: Int | String) -> String = "#{value}"\n accepts(broad)\nend\nrun()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def run()\n def accepts(callback: Callable[[Int], String]) = callback(42)\n def dynamic(value) -> String = "#{value}"\n accepts(dynamic)\nend\nrun()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def run()\n def accepts(callback: Callable[[], String]) = callback()\n def greeting() -> String = "diamond"\n accepts(greeting)\nend\nrun()')"
[[ "$actual" == "diamond" ]]

actual="$($diamond -e $'def run()\n def accepts(callback: Callable[[Int], String]) = 1\n def wrong(value: Int) -> Int = value\n begin\n  accepts(wrong)\n rescue error: TypeError\n  42\n end\nend\nrun()')"
[[ "$actual" == "42" ]]

actual="$($diamond --dump-bytecode -e $'def accepts(callback: Callable[[Int, String], Bool]) = true\ntrue')"
grep -q 'Callable\[\[Int, String\], Bool\]' <<<"$actual"

actual="$($diamond -e $'def empty_ints() -> Array[Int] = []\ndef preserve[T](values: Array[T]) -> Array[T] = values\nresult = preserve(empty_ints())\nbegin\n result.push("wrong")\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def empty_scores() -> Hash[String, Int] = {}\ndef preserve[K, V](values: Hash[K, V]) -> Hash[K, V] = values\nresult = preserve(empty_scores())\nbegin\n result["wrong"] = "wrong"\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def empty_ints() -> Array[Int] = []\ndef run()\n def wrong(value: String) -> String = value\n begin\n  array_map_typed(empty_ints(), wrong)\n rescue error: TypeError\n  42\n end\nend\nrun()')"
[[ "$actual" == "42" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e $'def empty_like[T](sample: T) -> Array[T] = []\ndef preserve[T](values: Array[T]) -> Array[T] = values\nresult = preserve(empty_like(["diamond"]))\nbegin\n result.push([42])\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def empty[T]() -> Array[T] = []\nresult = empty[Int]()\nbegin\n result.push("wrong")\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def empty_pair[K, V]() -> Hash[K, V] = {}\nresult = empty_pair[String, Int]()\nresult["answer"] = 42\nresult["answer"]')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def identity[T](value: T) -> T = value\nbegin\n identity[Int]("wrong")\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def empty[T]() -> Array[T] = []\ndef outer[T](value: T) -> Array[T] = empty[T]()\nresult = outer("diamond")\nbegin\n result.push(42)\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Factory\n def empty[T]() -> Array[T] = []\nend\nresult = Factory.new().empty[String]()\nbegin\n result.push(42)\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

if "$diamond" -e $'def empty[T]() -> Array[T] = []\nempty[Int, String]()' \
    >/dev/null 2>&1; then
    echo "wrong explicit generic argument count unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode -e $'def empty[T]() -> Array[T] = []\nempty[Array[String]]()')"
grep -q 'CALL_TYPED.*\[Array\[String\]\]' <<<"$actual"

actual="$($diamond -e $'def values[T]() -> Array[T] = []\ndef run()\n values = [42]\n values[0]\nend\nrun()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Greetable\n def greet(name: String) -> String = "Hello, #{name}"\nend\nclass Person\n include Greetable\nend\nPerson.new().greet("sir")')"
[[ "$actual" == "Hello, sir" ]]

actual="$($diamond -e $'module Identity\n def itself() = self\nend\nclass Box\n include Identity\nend\nBox.new().itself()')"
[[ "$actual" == "#<Box>" ]]

actual="$($diamond -e $'module First\n def value() = 1\nend\nmodule Second\n def value() = 2\nend\nclass Box\n include First\n include Second\nend\nBox.new().value()')"
[[ "$actual" == "2" ]]

actual="$($diamond -e $'module Values\n def value() = 1\nend\nclass Box\n include Values\n def value() = 3\nend\nBox.new().value()')"
[[ "$actual" == "3" ]]

actual="$($diamond -e $'module Values\n def value() = 42\nend\nclass Parent\n include Values\nend\nclass Child < Parent\nend\nChild.new().value()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Collections\n def empty[T]() -> Array[T] = []\nend\nclass Factory\n include Collections\nend\nresult = Factory.new().empty[String]()\nbegin\n result.push(42)\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

if "$diamond" -e $'class Box\n include Missing\nend' >/dev/null 2>&1; then
    echo "undefined included module unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'module Box\nend\nclass Box\nend' >/dev/null 2>&1; then
    echo "module and class name collision unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'module Base\n def value() = 1\nend\nmodule Combined\n include Base\n def other() = 41\nend\nclass Box\n include Combined\nend\nBox.new().value() + Box.new().other()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Base\n def value() = 1\nend\nmodule Combined\n include Base\n def value() = 2\nend\nclass Box\n include Combined\nend\nBox.new().value()')"
[[ "$actual" == "2" ]]

actual="$($diamond -e $'module First\n def value() = 1\nend\nmodule Second\n def value() = 2\nend\nmodule Combined\n include First\n include Second\nend\nclass Box\n include Combined\nend\nBox.new().value()')"
[[ "$actual" == "2" ]]

actual="$($diamond -e $'module Base\n def empty[T]() -> Array[T] = []\nend\nmodule Combined\n include Base\nend\nclass Factory\n include Combined\nend\nresult = Factory.new().empty[Int]()\nbegin\n result.push("wrong")\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

if "$diamond" -e $'module Recursive\n include Recursive\nend' >/dev/null 2>&1; then
    echo "self-including module unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'module Models\n class User\n end\nend\nModels::User.new()')"
[[ "$actual" == "#<Models::User>" ]]

actual="$($diamond -e $'module Outer\n module Greetings\n  def greet() = "hello"\n end\n class Person\n  include Outer::Greetings\n end\nend\nOuter::Person.new().greet()')"
[[ "$actual" == "hello" ]]

actual="$($diamond -e $'module Outer\n module Greetings\n  def greet() = "hello"\n end\n class Person\n  include Greetings\n end\nend\nOuter::Person.new().greet()')"
[[ "$actual" == "hello" ]]

actual="$($diamond -e $'module Models\n class Box\n end\nend\ndef accept(value: Models::Box) -> Models::Box = value\naccept(Models::Box.new())')"
[[ "$actual" == "#<Models::Box>" ]]

actual="$($diamond -e $'module First\n class Box\n end\nend\nmodule Second\n class Box\n end\nend\n[First::Box.new(), Second::Box.new()]')"
[[ "$actual" == "[#<First::Box>, #<Second::Box>]" ]]

if "$diamond" -e 'Missing::Thing.new()' >/dev/null 2>&1; then
    echo "undefined qualified name unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'module Counter\n def increment()\n  if @count == nil\n   @count = 1\n  else\n   @count = @count + 1\n  end\n end\n def count() = @count\nend\nclass Box\n include Counter\nend\nbox = Box.new()\nbox.increment()\nbox.increment()\nbox.count()')"
[[ "$actual" == "2" ]]

actual="$($diamond -e $'module Named\n def set_name(value)\n  @name = value\n end\n def name() = @name\nend\nclass Person\n include Named\nend\nclass Product\n include Named\nend\nperson = Person.new()\nproduct = Product.new()\nperson.set_name("Ada")\nproduct.set_name("Diamond")\n[person.name(), product.name()]')"
[[ "$actual" == "[Ada, Diamond]" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e $'module State\n def set_value(value)\n  @value = value\n end\n def value() = @value\nend\nmodule Combined\n include State\nend\nclass Box\n include Combined\nend\nbox = Box.new()\nbox.set_value(["diamond"])\nbox.value()')"
[[ "$actual" == "[diamond]" ]]

actual="$($diamond -e $'module State\n def module_value() = @value\nend\nclass Box\n include State\n def set_value(value)\n  @value = value\n end\nend\nbox = Box.new()\nbox.set_value(42)\nbox.module_value()')"
[[ "$actual" == "42" ]]

actual="$($diamond --dump-bytecode -e $'module State\n def value() = @value\nend\nclass Box\n include State\nend\nBox.new().value()')"
grep -q 'GET_IVAR_NAME' <<<"$actual"

actual="$($diamond -e $'module Contracts\n interface Named\n  def name() -> String\n end\nend\nclass Person\n def name() -> String = "Ada"\nend\ndef read(value: Contracts::Named) -> String = value.name()\nread(Person.new())')"
[[ "$actual" == "Ada" ]]

actual="$($diamond -e $'module Config\n ANSWER = 42\n def answer() = ANSWER\nend\nclass Reader\n include Config\nend\n[Config::ANSWER, Reader.new().answer()]')"
[[ "$actual" == "[42, 42]" ]]

actual="$($diamond -e $'module Outer\n VALUE = 40\n module Inner\n  OFFSET = 2\n  def total() = VALUE + OFFSET\n end\n class Box\n  include Inner\n end\nend\nOuter::Box.new().total()')"
[[ "$actual" == "42" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" -e $'module Config\n NAMES = ["diamond"]\n def names() = NAMES\nend\nclass Reader\n include Config\nend\nReader.new().names()')"
[[ "$actual" == "[diamond]" ]]

actual="$($diamond -e $'module First\n VALUE = 1\nend\nmodule Second\n VALUE = 2\nend\n[First::VALUE, Second::VALUE]')"
[[ "$actual" == "[1, 2]" ]]

if "$diamond" -e $'module Config\n VALUE = 1\n VALUE = 2\nend' >/dev/null 2>&1; then
    echo "namespace constant reassignment unexpectedly compiled" >&2
    exit 1
fi

if "$diamond" -e $'module Config\n value = 1\nend' >/dev/null 2>&1; then
    echo "lowercase module constant unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'module Config\n VALUE = 40\n def self.load(offset: Int = 2) -> Int = VALUE + offset\nend\n[Config.load(), Config.load(1)]')"
[[ "$actual" == "[42, 41]" ]]

actual="$($diamond -e $'module Types\n def self.empty[T]() -> Array[T] = []\nend\nresult = Types.empty[String]()\nbegin\n result.push(42)\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Outer\n module Math\n  def self.answer() = 42\n end\nend\nOuter::Math.answer()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Tools\n def self.answer() = 42\n def included() = 1\nend\nclass Box\n include Tools\nend\nbegin\n Box.new().answer()\nrescue error: TypeError\n Tools.answer()\nend')"
[[ "$actual" == "42" ]]

if "$diamond" -e $'module Tools\n def self.answer() = 1\n def self.answer() = 2\nend' >/dev/null 2>&1; then
    echo "duplicate module singleton function unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode -e $'module Types\n def self.empty[T]() -> Array[T] = []\nend\nTypes.empty[String]()')"
grep -q 'CALL_TYPED.*\[String\]' <<<"$actual"

actual="$($diamond -e $'class Factory\n def self.answer() = 42\nend\nFactory.answer()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Parent\n def self.answer() = 42\nend\nclass Child < Parent\nend\nChild.answer()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Parent\n def self.answer() = 1\nend\nclass Child < Parent\n def self.answer(offset = 1) = 41 + offset\nend\n[Parent.answer(), Child.answer()]')"
[[ "$actual" == "[1, 42]" ]]

actual="$($diamond -e $'class Factory\n def self.empty[T]() -> Array[T] = []\nend\nresult = Factory.empty[Int]()\nbegin\n result.push("wrong")\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

if "$diamond" -e $'class Factory\n def self.answer() = 1\n def self.answer() = 2\nend' >/dev/null 2>&1; then
    echo "duplicate class singleton method unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'class Vault\n def reveal() = self.answer()\n private\n def answer() = 42\nend\nVault.new().reveal()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Vault\n private\n def answer() = 42\nend\nbegin\n Vault.new().answer()\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Parent\n private\n def answer() = 42\nend\nclass Child < Parent\n def reveal() = self.answer()\nend\nChild.new().reveal()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Hidden\n private\n def answer() = 42\nend\nclass Box\n def reveal() = self.answer()\n include Hidden\nend\nBox.new().reveal()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Hidden\n private\n def answer() = 42\nend\nclass Box\n include Hidden\nend\nbegin\n Box.new().answer()\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Box\n private\n def hidden() = 1\n public\n def visible() = 42\nend\nBox.new().visible()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Mixed\n private\n def hidden() = 1\n public\n def visible() = 42\nend\nclass Box\n include Mixed\nend\nBox.new().visible()')"
[[ "$actual" == "42" ]]

if ! error="$($diamond -e $'class Box\n private\n def hidden() = 1\nend\nBox.new().hidden()' 2>&1 >/dev/null)"; then
    grep -q "private method 'hidden' called with an explicit receiver" <<<"$error"
else
    echo "private method call unexpectedly succeeded" >&2
    exit 1
fi

actual="$($diamond -e $'class Person\n attr_reader name\n attr_writer name\nend\nperson = Person.new()\nperson.name=("Ada")\nperson.name()')"
[[ "$actual" == "Ada" ]]

actual="$($diamond -e $'module Named\n attr_reader name\n attr_writer name\nend\nclass Person\n include Named\nend\nperson = Person.new()\nperson.name=("Ada")\nperson.name()')"
[[ "$actual" == "Ada" ]]

actual="$($diamond -e $'class Parent\n attr_reader value\n attr_writer value\nend\nclass Child < Parent\nend\nchild=Child.new()\nchild.value=(42)\nchild.value()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Box\n private\n attr_reader value\n public\n def reveal() = self.value()\nend\nbegin\n Box.new().value()\nrescue error: TypeError\n Box.new().reveal()\nend')"
[[ "$actual" == "nil" ]]

actual="$($diamond -e $'class Box\n attr_reader value\n def value=(incoming: Int) -> Int\n  @value = incoming\n end\nend\nbox=Box.new()\nbox.value=(42)\nbox.value()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Named\n def name=(value)\n  @name = value\n end\n def name() = @name\nend\nclass Person\n include Named\nend\nperson=Person.new()\nperson.name=("Ada")\nperson.name()')"
[[ "$actual" == "Ada" ]]

actual="$($diamond -e $'class Box\n attr_reader value\n def value=(incoming = 42)\n  @value = incoming\n end\nend\nbox=Box.new()\nbox.value=()\nbox.value()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Box\n def assign() = self.value=(42)\n private\n def value=(incoming)\n  @value = incoming\n end\nend\nBox.new().assign()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Box\n attr_accessor value\nend\nbox=Box.new()\nbox.value=(42)\nbox.value()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Named\n attr_accessor name\nend\nclass Person\n include Named\nend\nperson=Person.new()\nperson.name=("Ada")\nperson.name()')"
[[ "$actual" == "Ada" ]]

actual="$($diamond -e $'class Point\n attr_accessor x, y\nend\npoint=Point.new()\npoint.x=(20)\npoint.y=(22)\npoint.x() + point.y()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Pair\n attr_reader left, right\nend\nclass Box\n include Pair\nend\n[Box.new().left(), Box.new().right()]')"
[[ "$actual" == "[nil, nil]" ]]

if "$diamond" -e $'class Broken\n attr_reader value,\nend' >/dev/null 2>&1; then
    echo "trailing attribute comma unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'class Box\n def hidden() = 42\n private hidden\n def reveal() = self.hidden()\nend\nBox.new().reveal()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Box\n private\n def visible() = 42\n public visible\nend\nBox.new().visible()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Mixed\n attr_reader first, second\n private first, second\nend\nclass Box\n include Mixed\n def reveal() = [self.first(), self.second()]\nend\nBox.new().reveal()')"
[[ "$actual" == "[nil, nil]" ]]

if "$diamond" -e $'class Box\n private missing\nend' >/dev/null 2>&1; then
    echo "undefined visibility target unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'module Math\n BASE = 40\n def add(value: Int = 2) -> Int = BASE + value\n module_function add\nend\n[Math.add(), Math.add(1)]')"
[[ "$actual" == "[42, 41]" ]]

actual="$($diamond -e $'module Types\n def empty[T]() -> Array[T] = []\n module_function empty\nend\nresult=Types.empty[String]()\nbegin\n result.push(42)\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Helpers\n def answer() = 42\n module_function answer\nend\nclass Box\n def reveal() = self.answer()\n include Helpers\nend\nBox.new().reveal()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Helpers\n def answer() = 42\n module_function answer\nend\nclass Box\n include Helpers\nend\nbegin\n Box.new().answer()\nrescue error: TypeError\n Helpers.answer()\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Math\n BASE = 40\n module_function\n def add(value = 2) = BASE + value\n def answer() = 42\nend\n[Math.add(), Math.answer()]')"
[[ "$actual" == "[42, 42]" ]]

actual="$($diamond -e $'module Types\n module_function\n def empty[T]() -> Array[T] = []\nend\nresult=Types.empty[Int]()\nbegin\n result.push("wrong")\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Helpers\n module_function\n def answer() = 42\nend\nclass Box\n def reveal() = self.answer()\n include Helpers\nend\nbegin\n Box.new().answer()\nrescue error: TypeError\n Box.new().reveal()\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Outer\n module_function\n def outer() = 40\n module Inner\n  def value() = 2\n end\nend\nclass Box\n include Outer::Inner\nend\nOuter.outer() + Box.new().value()')"
[[ "$actual" == "42" ]]

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

actual="$($diamond -e $'class Box\n def self.value=(incoming: Int) -> Int = incoming\nend\nBox.value=(42)')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Config\n def self.value=(incoming = 42) = incoming\nend\nConfig.value=()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Box\n attr_writer value\n private value=\n def assign() = self.value=(42)\nend\nbegin\n Box.new().value=(1)\nrescue error: TypeError\n Box.new().assign()\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Point\n attr_accessor(x, y)\nend\npoint=Point.new()\npoint.x=(20)\npoint.y=(22)\npoint.x() + point.y()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Box\n attr_reader value\n private(value)\n def reveal() = self.value()\nend\nBox.new().reveal()')"
[[ "$actual" == "nil" ]]

actual="$($diamond -e $'module Values\n def first() = 20\n def second() = 22\n module_function(first, second)\nend\nValues.first() + Values.second()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Values\n def value=(incoming) = incoming\n module_function value=\nend\nValues.value=(42)')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Pair\n attr(left, right)\nend\n[Pair.new().left(), Pair.new().right()]')"
[[ "$actual" == "[nil, nil]" ]]

actual="$($diamond -e $'class Answer\n def value() = 42\n alias_method result, value\nend\nAnswer.new().result()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Named\n attr_reader name\n alias_method label, name\nend\nclass Person\n include Named\nend\nPerson.new().label()')"
[[ "$actual" == "nil" ]]

if "$diamond" -e $'class Broken\n alias_method answer, missing\nend' >/dev/null 2>&1; then
    echo "undefined alias source unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'class Box\n attr_accessor value: Int\n def raw() = @value\nend\nbox=Box.new()\nbox.value=(42)\n[box.value(), box.raw()]')"
[[ "$actual" == "[42, 42]" ]]

actual="$($diamond -e $'class Box\n attr_accessor value: Int\n def raw() = @value\nend\nbox=Box.new()\nbegin\n box.value=("wrong")\nrescue error: TypeError\n box.raw()\nend')"
[[ "$actual" == "nil" ]]

actual="$($diamond -e $'class Box\n attr_reader value: Int\nend\nbegin\n Box.new().value()\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Pair\n attr_accessor left: Int, right: String\nend\npair=Pair.new()\npair.left=(42)\npair.right=("answer")\n[pair.left(), pair.right()]')"
[[ "$actual" == "[42, answer]" ]]

actual="$($diamond -e $'class Box\n def empty?() = true\nend\nBox.new().empty?()')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'def ready?(value: Int) = value == 42\nready?(42)')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'module Query\n def valid?() = true\nend\nclass Box\n include Query\nend\nBox.new().valid?()')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'class Counter\n attr_accessor value\n def reset!()\n  @value = 0\n end\nend\ncounter=Counter.new()\ncounter.value=(42)\ncounter.reset!()\ncounter.value()')"
[[ "$actual" == "0" ]]

actual="$($diamond -e $'def assert!(value: Bool) = value\nassert!(true)')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'module Mutation\n def clear!() = 42\nend\nclass Box\n include Mutation\nend\nBox.new().clear!()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Box\n attr_accessor value\n alias_method assign=, value=\nend\nbox=Box.new()\nbox.assign=(42)\nbox.value()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'module Named\n attr_accessor name\n alias_method label=, name=\nend\nclass Person\n include Named\nend\nperson=Person.new()\nperson.label=("Ada")\nperson.name()')"
[[ "$actual" == "Ada" ]]

if "$diamond" -e $'class Broken\n attr_writer value\n alias_method value=, value=\nend' >/dev/null 2>&1; then
    echo "duplicate writer alias unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'class Answer\n def value() = 42\n alias_method(result, value)\nend\nAnswer.new().result()')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class Box\n attr_accessor value\n alias_method(assign=, value=)\nend\nbox=Box.new()\nbox.assign=(42)\nbox.value()')"
[[ "$actual" == "42" ]]

if "$diamond" -e $'class Broken\n def value() = 42\n alias_method(result, value\nend' >/dev/null 2>&1; then
    echo "unterminated parenthesized alias unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'unless false\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'unless true\n 0\nelse\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def answer(value: Int | Nil) -> Int\n unless value == nil\n  value + 0\n else\n  0\n end\nend\nanswer(42)')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'value = 0\nuntil value == 3\n value = value + 1\nend\nvalue')"
[[ "$actual" == "3" ]]

actual="$($diamond -e $'value = 0\nuntil false\n value = value + 1\n break\nend\nvalue')"
[[ "$actual" == "1" ]]

actual="$($diamond -e $'value = 0\nuntil value == 3\n value = value + 1\n next\n value = 99\nend\nvalue')"
[[ "$actual" == "3" ]]

actual="$($diamond -e $'value = 2\nif value == 1\n "one"\nelsif value == 2\n "two"\nelse\n "other"\nend')"
[[ "$actual" == "two" ]]

actual="$($diamond -e $'value = 3\nif value == 1\n 1\nelsif value == 2\n 2\nelsif value == 3\n 42\nelse\n 0\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'if false\n 0\nelsif false\n 1\nend')"
[[ "$actual" == "nil" ]]

actual="$($diamond -e $'class Feature\n attr_writer enabled\n attr_predicate enabled: Bool\nend\nfeature=Feature.new()\nfeature.enabled=(true)\nfeature.enabled?()')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'module State\n attr_predicate ready\nend\nclass Job\n include State\nend\nJob.new().ready?()')"
[[ "$actual" == "nil" ]]

if "$diamond" -e $'class Broken\n attr_predicate ready\n attr_predicate ready\nend' >/dev/null 2>&1; then
    echo "duplicate predicate attribute unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'class Vault\n attr_writer ready\n attr_predicate ready\n private ready?\n def reveal() = self.ready?()\nend\nvault=Vault.new()\nvault.ready=(true)\nvault.reveal()')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'class State\n def valid?() = true\n def reset!() = 42\n alias_method acceptable?, valid?\n alias_method clear!, reset!\nend\nstate=State.new()\n[state.acceptable?(), state.clear!()]')"
[[ "$actual" == "[true, 42]" ]]

actual="$($diamond -e $'module Query\n def valid?(value) = value == 42\n module_function valid?\nend\nQuery.valid?(42)')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'begin\n 20\nrescue error\n 0\nelse\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'begin\n raise "failure"\nrescue error\n 42\nelse\n 0\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'value = 0\nbegin\n 1\nrescue error\n value = 1\nelse\n value = 40\nensure\n value = value + 2\nend\nvalue')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'begin\n begin\n  raise "failure"\n rescue error\n  raise\n end\nrescue outer\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'begin\n begin\n  1 / 0\n rescue error: ZeroDivisionError\n  raise\n end\nrescue outer: ZeroDivisionError\n 42\nend')"
[[ "$actual" == "42" ]]

if "$diamond" -e $'raise' >/dev/null 2>&1; then
    echo "bare raise outside rescue unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'attempts = 0\nbegin\n attempts = attempts + 1\n if attempts < 3\n  raise "again"\n end\n attempts\nrescue error\n retry\nend')"
[[ "$actual" == "3" ]]

actual="$($diamond -e $'attempts = 0\ncleanups = 0\nbegin\n attempts = attempts + 1\n if attempts < 2\n  raise "again"\n end\nrescue error\n retry\nensure\n cleanups = cleanups + 1\nend\n[attempts, cleanups]')"
[[ "$actual" == "[2, 1]" ]]

if "$diamond" -e $'retry' >/dev/null 2>&1; then
    echo "retry outside rescue unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'begin\n 1 / 0\nrescue : ZeroDivisionError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'begin\n [1][4]\nrescue : TypeError | IndexError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'begin\n 1 / 0\nrescue error: ZeroDivisionError\n error\nend')"
[[ "$actual" == "#<ZeroDivisionError>" ]]

actual="$($diamond -e $'condition_checks = 0\nbody_runs = 0\nwhile condition_checks < 1\n condition_checks = condition_checks + 1\n body_runs = body_runs + 1\n if body_runs < 3\n  redo\n end\nend\n[condition_checks, body_runs]')"
[[ "$actual" == "[3, 3]" ]]

actual="$($diamond -e $'runs = 0\nuntil true\n runs = 99\nend\nuntil runs == 2\n runs = runs + 1\n if runs == 1\n  redo\n end\nend\nruns')"
[[ "$actual" == "2" ]]

if "$diamond" -e $'redo' >/dev/null 2>&1; then
    echo "redo outside loop unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'result = while true\n break 42\nend\nresult')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'result = until false\n break "done"\nend\nresult')"
[[ "$actual" == "done" ]]

actual="$($diamond -e $'outer = while true\n inner = while true\n  break 20\n end\n break inner + 22\nend\nouter')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'if true then 42 else 0 end')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'unless true then 0 else 42 end')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'if false then\n 0\nelsif true then\n 42\nelse\n 1\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'value = 0\nwhile value < 3 do value = value + 1 end\nvalue')"
[[ "$actual" == "3" ]]

actual="$($diamond -e $'value = 0\nuntil value == 3 do\n value = value + 1\nend\nvalue')"
[[ "$actual" == "3" ]]

actual="$($diamond -e $'result = while true do break 42 end\nresult')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'not false')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'not nil')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'not (20 + 22 == 42)')"
[[ "$actual" == "false" ]]

actual="$($diamond -e $'true and 42')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'nil or "fallback"')"
[[ "$actual" == "fallback" ]]

actual="$($diamond -e $'[(false and (1 / 0)), (true or (1 / 0))]')"
[[ "$actual" == "[false, true]" ]]

actual="$($diamond -e $'begin\n [1][4]\nrescue : TypeError\n 0\nrescue : IndexError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'begin\n 1 / 0\nrescue type: TypeError\n 0\nrescue division: ZeroDivisionError\n 42\nrescue error\n 1\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'begin\n begin\n  raise "text"\n rescue : TypeError\n  0\n rescue : IndexError\n  1\n end\nrescue outer\n 42\nend')"
[[ "$actual" == "42" ]]

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

actual="$($diamond -e $'begin\n 40\nrescue : TypeError\n 0\nrescue : IndexError\n 1\nelse\n 42\nend')"
[[ "$actual" == "42" ]]

attempt="$($diamond -e $'attempts=0\nbegin\n attempts=attempts+1\n if attempts==1\n  [1][4]\n end\nrescue : TypeError\n 0\nrescue : IndexError\n retry\nelse\n 42\nend')"
[[ "$attempt" == "42" ]]

actual="$($diamond -e $'cleanup=0\nvalue=begin\n [1][4]\nrescue : TypeError\n 0\nrescue : IndexError\n 40\nensure\n cleanup=cleanup+1\nend\n[value+2, cleanup]')"
[[ "$actual" == "[42, 1]" ]]

actual="$($diamond -e $'begin\n begin\n  [1][4]\n rescue : TypeError\n  0\n rescue error: IndexError\n  raise\n end\nrescue outer: IndexError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'begin\n 1 / 0\nrescue : TypeError | IndexError\n 0\nrescue : ZeroDivisionError | RangeError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'class NetworkError < StandardError\nend\nclass TimeoutError < NetworkError\nend\nbegin\n raise TimeoutError.new()\nrescue : TypeError\n 0\nrescue error: NetworkError\n 42\nend')"
[[ "$actual" == "42" ]]

if "$diamond" -e $'begin\n 1 / 0\nrescue : StandardError\n 1\nrescue : ZeroDivisionError\n 2\nend' >/dev/null 2>&1; then
    echo "shadowed rescue subclass unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond -e $'result=loop do\n break 42\nend\nresult')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'count=0\nloop do\n count=count+1\n if count<3\n  redo\n end\n break count\nend')"
[[ "$actual" == "3" ]]

actual="$($diamond -e $'loop\n break 42\nend')"
[[ "$actual" == "42" ]]
if "$diamond" -e $'loop 42 end' >/dev/null 2>&1; then exit 1; fi
actual="$($diamond -e $'count=0\nresult=loop do\n count=count+1\n if count<3\n  next\n end\n break count+39\nend\nresult')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'outer=loop do\n inner=loop do\n  break 20\n end\n break inner+22\nend\nouter')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'class DetailedError < StandardError\n attr_reader message: String\n def initialize(message: String)\n  @message=message\n end\nend\nbegin\n raise DetailedError.new("failure")\nrescue error: DetailedError\n error.message()\nend')"
[[ "$actual" == "failure" ]]
actual="$($diamond -e $'class WrappedError < StandardError\n attr_reader cause: StandardError\n def initialize(cause: StandardError)\n  @cause=cause\n end\nend\nbegin\n raise WrappedError.new(TypeError.new())\nrescue error: WrappedError\n error.cause() is TypeError\nend')"
[[ "$actual" == "true" ]]
actual="$($diamond -e $'class DetailedError < StandardError\n attr_accessor message: String\nend\nbegin\n DetailedError.new().message=(42)\nrescue : TypeError\n 42\nend')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'class DetailedError < StandardError\n attr_accessor message: String\nend\nclass NetworkError < DetailedError\nend\nerror=NetworkError.new()\nerror.message=("offline")\nbegin\n raise error\nrescue caught: DetailedError\n caught.message()\nend')"
[[ "$actual" == "offline" ]]
actual="$($diamond -e $'class DetailedError < StandardError\n attr_accessor message: String\nend\nerror=DetailedError.new()\nerror.message=("kept")\nbegin\n begin\n  raise error\n rescue inner: DetailedError\n  raise\n end\nrescue outer: DetailedError\n outer.message()\nend')"
[[ "$actual" == "kept" ]]
actual="$($diamond -e $'class DetailedError < StandardError\n attr_accessor message: String\nend\nerror=DetailedError.new()\nerror.message=("stable")\ncleanup=nil\nbegin\n raise error\nrescue caught: DetailedError\n caught.message()\nensure\n cleanup=error.message()\nend\ncleanup')"
[[ "$actual" == "stable" ]]
actual="$($diamond -e $'begin\n 1 / 0\nrescue error: ZeroDivisionError\n error.message()\nend')"
[[ "$actual" == "division by zero" ]]
actual="$($diamond -e $'begin\n [1][4]\nrescue error: IndexError\n [error.message(), error.cause()]\nend')"
[[ "$actual" == "[index 4 out of bounds for Array of length 1, nil]" ]]
actual="$($diamond -e $'root=TypeError.new("root")\nerror=RuntimeError.new("wrapped", root)\n[error.message(), error.cause().message()]')"
[[ "$actual" == "[wrapped, root]" ]]
actual="$($diamond -e $'error=RuntimeError.new()\n[error.message(), error.cause()]')"
[[ "$actual" == "[nil, nil]" ]]
if "$diamond" -e $'RuntimeError.new(1, 2, 3)' >/dev/null 2>&1; then exit 1; fi
actual="$(DIAMOND_STRESS_GC=1 $diamond -e $'begin\n 1 / 0\nrescue error: ZeroDivisionError\n error.message()\nend')"
[[ "$actual" == "division by zero" ]]
actual="$($diamond -e $'error=RuntimeError.new("x")\nleft=begin\n error.message(1)\nrescue : ArgumentError\n 20\nend\nright=begin\n error.cause(1)\nrescue : ArgumentError\n 22\nend\nleft+right')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'class CustomError < StandardError\n def initialize(code: Int)\n  @message="code #{code}"\n end\nend\nCustomError.new(42).message()')"
[[ "$actual" == "code 42" ]]
actual="$($diamond -e $'root=IndexError.new("root")\nwrapped=RuntimeError.new("wrapped", root)\nbegin\n raise wrapped\nrescue error: RuntimeError\n [error.message(), error.cause().message()]
end')"
[[ "$actual" == "[wrapped, root]" ]]
actual="$($diamond -e $'type_message=begin\n 1+"x"\nrescue error: TypeError\n error.message()\nend\nargument_message=begin\n [1].push()\nrescue error: ArgumentError\n error.message()\nend\n[type_message is String, argument_message is String]')"
[[ "$actual" == "[true, true]" ]]
actual="$($diamond -e $'begin\n begin\n  1/0\n rescue error: ZeroDivisionError\n  raise\n end\nrescue outer: ZeroDivisionError\n outer.message()\nend')"
[[ "$actual" == "division by zero" ]]

actual="$($diamond -e '42 if true')"
[[ "$actual" == "42" ]]
actual="$($diamond -e '42 if false')"
[[ "$actual" == "nil" ]]
actual="$($diamond -e '42 unless false')"
[[ "$actual" == "42" ]]
actual="$($diamond -e '42 unless true')"
[[ "$actual" == "nil" ]]
actual="$($diamond -e $'value=0\nvalue=42 if true\nvalue')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'value=0\nvalue=42 if false\nvalue')"
[[ "$actual" == "0" ]]
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
actual="$(DIAMOND_STRESS_GC=1 $diamond -e '"stable" if true')"
[[ "$actual" == "stable" ]]
actual="$(DIAMOND_STRESS_GC=1 $diamond -e '"discarded" if false')"
[[ "$actual" == "nil" ]]
actual="$($diamond -e $'def ready()\n true\nend\n42 if ready()')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'def ready()\n false\nend\n42 unless ready()')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'loop do\n break 42 if true\nend')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'loop do\n break 42 unless false\nend')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'def answer()\n return 42 if true\n 0\nend\nanswer()')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'begin\n raise TypeError.new("bad") if true\nrescue : TypeError\n 42\nend')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'loop do\n next if false\n break 42\nend')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'loop do\n break if true\nend')"
[[ "$actual" == "nil" ]]
actual="$($diamond -e $'loop do\n redo if false\n break 42\nend')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'def conditional_return(flag)\n return if flag\n 42\nend\n[conditional_return(false), conditional_return(true)]')"
[[ "$actual" == "[42, nil]" ]]
actual="$($diamond -e $'attempts=0\nbegin\n attempts=attempts+1\n [1][4]\nrescue : IndexError\n attempts=attempts+1\n retry if attempts<3\nend\nattempts')"
[[ "$actual" == "4" ]]
actual="$($diamond -e $'outer=loop do\n inner=loop do\n  next if false\n  break 20\n end\n break inner+22\nend\nouter')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'def typed_return(flag: Bool) -> Int\n return 42 if flag\n 7\nend\n[typed_return(true), typed_return(false)]')"
[[ "$actual" == "[42, 7]" ]]
actual="$($diamond -e $'def endless_if() = 42 if true\nendless_if()')"
[[ "$actual" == "42" ]]
actual="$($diamond -e $'def endless_if() = 42 if false\nendless_if()')"
[[ "$actual" == "nil" ]]
actual="$($diamond -e $'def endless_unless() = 42 unless false\nendless_unless()')"
[[ "$actual" == "42" ]]
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
actual="$($diamond -e $'def ready() = true\ndef conditional() = 42 if ready()\nconditional()')"
[[ "$actual" == "42" ]]
actual="$(DIAMOND_STRESS_GC=1 $diamond -e $'def conditional() = "kept" if true\nconditional()')"
[[ "$actual" == "kept" ]]
actual="$($diamond -e $'class Box\n def value() = 42 if true\nend\nBox.new().value()')"
[[ "$actual" == "42" ]]
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

actual="$($diamond --dump-bytecode -e $'def make()\n def once()\n  1\n end\n once\nend\nFiber.new(make())')"
grep -q 'FIBER_NEW' <<<"$actual"

actual="$($diamond -e $'def make()\n def once()\n  1\n end\n once\nend\nf = Fiber.new(make())\n"ok"')"
[[ "$actual" == "ok" ]]

error_file="$(mktemp)"
if "$diamond" -e 'Fiber.new(5)' >/dev/null 2>"$error_file"; then
    echo "Fiber.new with a non-Callable argument unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'Fiber.new argument must be a Callable value' "$error_file"
rm -f "$error_file"

actual="$($diamond -e $'begin\n Fiber.new(5)\nrescue error: TypeError\n 42\nend')"
[[ "$actual" == "42" ]]

error_file="$(mktemp)"
if "$diamond" -e $'def make()\n def once(x)\n  x\n end\n once\nend\nFiber.new(make())' \
    >/dev/null 2>"$error_file"; then
    echo "Fiber.new with a non-zero-arity callable unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'Fiber.new callable must take no arguments' "$error_file"
rm -f "$error_file"

actual="$($diamond -e $'def make()\n def once(x)\n  x\n end\n once\nend\nbegin\n Fiber.new(make())\nrescue error: ArgumentError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond --dump-bytecode -e $'Fiber = 5\nFiber.new(1)' 2>/dev/null || true)"
if grep -q 'FIBER_NEW' <<<"$actual"; then
    echo "Fiber.new on a shadowing local unexpectedly compiled to FIBER_NEW" >&2
    exit 1
fi

actual="$($diamond -e $'def make_counter()\n def counter()\n  i = 0\n  loop do\n   got = yield(i)\n   i = i + got\n  end\n end\n counter\nend\nf = Fiber.new(make_counter())\nfirst = f.resume(0)\nsecond = f.resume(10)\nthird = f.resume(5)\n"#{first}, #{second}, #{third}"')"
[[ "$actual" == "0, 10, 15" ]]

actual="$($diamond -e $'def make()\n def once()\n  1\n end\n once\nend\nf = Fiber.new(make())\nbefore = f.status()\na = f.resume(0)\nafter = f.status()\nalive_before = f.alive?()\n"#{before}, #{a}, #{after}, #{alive_before}"')"
[[ "$actual" == "runnable, 1, completed, false" ]]

actual="$($diamond -e $'def make()\n def once()\n  yield(1)\n  99\n end\n once\nend\nf = Fiber.new(make())\na = f.resume(0)\nb = f.resume(0)\ns = f.status()\n"#{a}, #{b}, #{s}"')"
[[ "$actual" == "1, 99, completed" ]]

actual="$($diamond -e $'def make()\n def once()\n  yield(1)\n  99\n end\n once\nend\nf = Fiber.new(make())\nf.resume(0)\nf.resume(0)\nbegin\n f.resume(0)\nrescue error: FiberError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$($diamond -e $'def make()\n def bad()\n  yield(1)\n  raise RuntimeError.new("boom")\n end\n bad\nend\nf = Fiber.new(make())\nf.resume(0)\nbegin\n f.resume(0)\nrescue error: RuntimeError\n error.message()\nend')"
[[ "$actual" == "boom" ]]

actual="$(DIAMOND_STRESS_GC=1 $diamond -e $'def make()\n def once()\n  yield(7)\n end\n once\nend\nf = Fiber.new(make())\nf.resume(0)')"
[[ "$actual" == "7" ]]

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
grep -q 'runtime error: type error' "$error_file"
rm -f "$error_file"

actual="$($diamond --dump-bytecode -e 'print("x")')"
grep -Eq 'PRINT +r[0-9]+, r[0-9]+, newline=0' <<<"$actual"

actual="$($diamond --dump-bytecode -e 'puts("x")')"
grep -Eq 'PRINT +r[0-9]+, r[0-9]+, newline=1' <<<"$actual"

actual="$($diamond -e $'print("a")\nprint("b")\n"c"')"
[[ "$actual" == "abc" ]]

actual="$($diamond -e $'puts("a")\nputs("b")\n0')"
[[ "$actual" == $'a\nb\n0' ]]

actual="$($diamond -e $'class Foo\n def to_s()\n  "a Foo"\n end\nend\nputs(Foo.new())\n0')"
[[ "$actual" == $'a Foo\n0' ]]

actual="$($diamond -e $'def print(x)\n "shadowed"\nend\nprint("real")')"
[[ "$actual" == "shadowed" ]]

actual="$($diamond -e $'x = puts("hi")\nx == nil')"
[[ "$actual" == $'hi\ntrue' ]]

actual="$($diamond -e $'class Bad\n def to_s(x)\n  "no"\n end\nend\nbegin\n puts(Bad.new())\nrescue error: ArgumentError\n 42\nend')"
[[ "$actual" == "42" ]]

if "$diamond" -e 'puts(1, 2)' >/dev/null 2>&1; then
    echo "puts with more than one argument unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode -e 'gets()')"
grep -Eq 'GETS +r[0-9]+' <<<"$actual"

actual="$(printf 'hello world\n' | $diamond -e $'name = gets()\n"hi #{name}"')"
[[ "$actual" == "hi hello world" ]]

actual="$(printf '' | $diamond -e $'x = gets()\nx == nil')"
[[ "$actual" == "true" ]]

actual="$(printf 'no trailing newline' | $diamond -e $'gets()')"
[[ "$actual" == "no trailing newline" ]]

actual="$(printf 'line1\r\nline2\n' | $diamond -e $'a = gets()\nb = gets()\n"#{a}|#{b}"')"
[[ "$actual" == "line1|line2" ]]

actual="$($diamond -e $'def gets()\n "shadowed"\nend\ngets()')"
[[ "$actual" == "shadowed" ]]

if "$diamond" -e 'gets(1)' >/dev/null 2>&1; then
    echo "gets with an argument unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode -e 'File.open("x", "r")' 2>/dev/null || true)"
grep -q 'FILE_OPEN' <<<"$actual"

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

actual="$($diamond -e $'def run()\n result = []\n def collect(item)\n  result.push(item + 1)\n end\n [1,2,3].each(collect)\n result\nend\nrun()')"
[[ "$actual" == "[2, 3, 4]" ]]

actual="$($diamond -e $'def run()\n result = []\n def collect(item)\n  result.push(item + 1)\n end\n values = [1,2,3]\n array_each(values, collect)\n result\nend\nrun()')"
[[ "$actual" == "[2, 3, 4]" ]]

actual="$($diamond -e $'def run()\n result = []\n def collect(k, v)\n  result.push(v)\n end\n {"a":1,"b":2}.each(collect)\n result\nend\nrun()')"
[[ "$actual" == "[1, 2]" ]]

actual="$($diamond -e $'def run()\n result = []\n def collect(k, v)\n  result.push(v)\n end\n values = {"a":1,"b":2}\n hash_each(values, collect)\n result\nend\nrun()')"
[[ "$actual" == "[1, 2]" ]]

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

actual="$($diamond -e $'def run()\n def is_even(x)\n  x - (x / 2) * 2 == 0\n end\n enumerable_select([1,2,3,4,5,6], is_even)\nend\nrun()')"
[[ "$actual" == "[2, 4, 6]" ]]

actual="$($diamond -e $'def run()\n def is_positive(x)\n  x > 0\n end\n enumerable_select({"a":1,"b":-2,"c":3}, is_positive)\nend\nrun()')"
[[ "$actual" == "[1, 3]" ]]

actual="$($diamond -e $'def run()\n def is_even(x)\n  x - (x / 2) * 2 == 0\n end\n enumerable_count([1,2,3,4], is_even)\nend\nrun()')"
[[ "$actual" == "2" ]]

actual="$($diamond -e $'def run()\n def is_positive(x)\n  x > 0\n end\n enumerable_count({"a":1,"b":-2,"c":3}, is_positive)\nend\nrun()')"
[[ "$actual" == "2" ]]

actual="$($diamond -e $'def run()\n def is_even(x)\n  x - (x / 2) * 2 == 0\n end\n enumerable_any([1,3,5,6], is_even)\nend\nrun()')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'def run()\n def is_even(x)\n  x - (x / 2) * 2 == 0\n end\n enumerable_any([1,3,5], is_even)\nend\nrun()')"
[[ "$actual" == "false" ]]

actual="$($diamond -e $'def run()\n def is_positive(x)\n  x > 0\n end\n enumerable_all({"a":1,"b":2}, is_positive)\nend\nrun()')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'def run()\n def is_positive(x)\n  x > 0\n end\n enumerable_all({"a":1,"b":-2}, is_positive)\nend\nrun()')"
[[ "$actual" == "false" ]]

actual="$($diamond -e $'def run()\n def anything(x)\n  false\n end\n enumerable_all([], anything)\nend\nrun()')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'def run()\n def anything(x)\n  true\n end\n enumerable_any([], anything)\nend\nrun()')"
[[ "$actual" == "false" ]]

actual="$($diamond -e $'def run()\n def double(x)\n  x * 2\n end\n enumerable_map([1,2,3], double)\nend\nrun()')"
[[ "$actual" == "[2, 4, 6]" ]]

actual="$($diamond -e $'def run()\n def double(x)\n  x * 2\n end\n enumerable_map({"a":1,"b":2}, double)\nend\nrun()')"
[[ "$actual" == "[2, 4]" ]]

actual="$($diamond -e $'def run()\n def add(acc, x)\n  acc + x\n end\n enumerable_reduce([1,2,3], 0, add)\nend\nrun()')"
[[ "$actual" == "6" ]]

actual="$($diamond -e $'def run()\n def add(acc, x)\n  acc + x\n end\n enumerable_reduce({"a":1,"b":2,"c":3}, 0, add)\nend\nrun()')"
[[ "$actual" == "6" ]]

actual="$($diamond -e $'def run()\n def is_even(x)\n  x - (x / 2) * 2 == 0\n end\n [1,2,3,4,5,6].select(is_even)\nend\nrun()')"
[[ "$actual" == "[2, 4, 6]" ]]

actual="$($diamond -e $'def run()\n def is_positive(x)\n  x > 0\n end\n {"a":1,"b":-2,"c":3}.select(is_positive)\nend\nrun()')"
[[ "$actual" == "[1, 3]" ]]

actual="$($diamond -e $'def run()\n def is_even(x)\n  x - (x / 2) * 2 == 0\n end\n [1,2,3,4].count(is_even)\nend\nrun()')"
[[ "$actual" == "2" ]]

actual="$($diamond -e $'def run()\n def is_positive(x)\n  x > 0\n end\n {"a":1,"b":-2}.count(is_positive)\nend\nrun()')"
[[ "$actual" == "1" ]]

actual="$($diamond -e $'def run()\n def is_even(x)\n  x - (x / 2) * 2 == 0\n end\n [1,3,5,6].any?(is_even)\nend\nrun()')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'def run()\n def is_positive(x)\n  x > 0\n end\n [1,2,3].all?(is_positive)\nend\nrun()')"
[[ "$actual" == "true" ]]

actual="$($diamond -e $'def run()\n def double(x)\n  x * 2\n end\n [1,2,3].map(double)\nend\nrun()')"
[[ "$actual" == "[2, 4, 6]" ]]

actual="$($diamond -e $'def run()\n def double(x)\n  x * 2\n end\n {"a":1,"b":2}.map(double)\nend\nrun()')"
[[ "$actual" == "[2, 4]" ]]

actual="$($diamond -e $'def run()\n def add(acc, x)\n  acc + x\n end\n [1,2,3].reduce(0, add)\nend\nrun()')"
[[ "$actual" == "6" ]]

actual="$($diamond -e $'def run()\n def add(acc, x)\n  acc + x\n end\n {"a":1,"b":2,"c":3}.reduce(0, add)\nend\nrun()')"
[[ "$actual" == "6" ]]

echo "641 tests passed"
