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

actual="$($diamond tests/cases/arithmetic.dia)"
[[ "$actual" == "42" ]] || {
    echo "expected file result 42, got: $actual" >&2
    exit 1
}

actual="$($diamond -e '2 + 3 * 4 - -1')"
[[ "$actual" == "15" ]] || {
    echo "precedence or unary arithmetic failed: $actual" >&2
    exit 1
}

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

actual="$($diamond tests/cases/control_flow.dia)"
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

actual="$($diamond tests/cases/functions.dia)"
[[ "$actual" == "42" ]] || {
    echo "function or recursion test failed: $actual" >&2
    exit 1
}

if "$diamond" -e $'def one(a)\n  a\nend\none()' >/dev/null 2>&1; then
    echo "wrong function arity unexpectedly compiled" >&2
    exit 1
fi

actual="$($diamond --dump-bytecode tests/cases/functions.dia)"
grep -q '^== factorial ==$' <<<"$actual"
grep -q 'CALL' <<<"$actual"
[[ "${actual##*$'\n'}" == "42" ]] || {
    echo "disassembled function program did not execute to 42" >&2
    exit 1
}

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/strings.dia)"
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

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/classes.dia)"
[[ "$actual" == "hello, diamond" ]] || {
    echo "class, method, field, or stress-GC test failed: $actual" >&2
    exit 1
}

actual="$("$diamond" --dump-bytecode tests/cases/classes.dia)"
grep -q 'NEW' <<<"$actual"
grep -q 'INVOKE' <<<"$actual"
grep -q 'GET_IVAR' <<<"$actual"
grep -q 'SET_IVAR' <<<"$actual"

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/inheritance.dia)"
[[ "$actual" == "hello> diamond" ]] || {
    echo "inheritance, override, self, or inherited constructor failed: $actual" >&2
    exit 1
}

if "$diamond" -e $'class Child < Missing\nend' >/dev/null 2>&1; then
    echo "undefined superclass unexpectedly compiled" >&2
    exit 1
fi

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/super.dia)"
[[ "$actual" == "hello> diamond!" ]] || {
    echo "super method or constructor chaining failed: $actual" >&2
    exit 1
}

actual="$("$diamond" --dump-bytecode tests/cases/super.dia)"
grep -q 'SUPER' <<<"$actual"

if "$diamond" -e $'class Root\n  def value()\n    super()\n  end\nend' >/dev/null 2>&1; then
    echo "super without a superclass unexpectedly compiled" >&2
    exit 1
fi

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/types.dia)"
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

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/nilable_types.dia)"
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

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/general_unions.dia)"
[[ "$actual" == "diamond" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/array_generics.dia)"
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

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/hash_generics.dia)"
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

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/arrays.dia)"
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

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/array_mutation.dia)"
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

actual="$("$diamond" tests/cases/returns.dia)"
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

actual="$("$diamond" tests/cases/loop_control.dia)"
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

if "$diamond" -e $'while true\n  break 42\nend' >/dev/null 2>&1; then
    echo "valued break unexpectedly compiled" >&2
    exit 1
fi

actual="$("$diamond" tests/cases/source_ergonomics.dia)"
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

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/hashes.dia)"
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
if "$diamond" tests/cases/stack_trace.dia >/dev/null 2>"$error_file"; then
    echo "runtime stack trace unexpectedly succeeded" >&2
    exit 1
fi
grep -q 'runtime error: division by zero' "$error_file"
grep -q 'at divide:3:' "$error_file"
grep -q 'at invoke:8:' "$error_file"
grep -q 'at tests/cases/stack_trace.dia:11:' "$error_file"
rm -f "$error_file"

actual="$("$diamond" --dump-bytecode -e '40 + 2')"
grep -Eq '^[0-9]{4} +1:[0-9]+ +ADD' <<<"$actual"

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/closure_capture.dia)"
[[ "$actual" == "47" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/mutable_closure.dia)"
[[ "$actual" == "2" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/deep_closure.dia)"
[[ "$actual" == "42" ]]

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC=1 "$diamond" tests/cases/inline_cache.dia 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'inline caches: 4 hits, 1 misses' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_IC=1 "$diamond" tests/cases/polymorphic_cache.dia 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'inline caches: 2 hits, 2 misses' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_SHAPES=1 "$diamond" tests/cases/runtime_shapes.dia 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'shape transitions: 1' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
actual="$(DIAMOND_TRACE_FIELDS=1 "$diamond" tests/cases/runtime_shapes.dia 2>"$error_file")"
[[ "$actual" == "42" ]]
grep -q 'field caches: 2 hits, 4 misses' "$error_file"
rm -f "$error_file"

error_file="$(mktemp)"
if DIAMOND_STRESS_GC=1 "$diamond" tests/cases/raise_stack.dia >/dev/null 2>"$error_file"; then
    echo "raised value unexpectedly returned" >&2
    exit 1
fi
grep -q 'runtime error: uncaught exception: diamond cracked' "$error_file"
grep -q 'at fail:2:' "$error_file"
grep -q 'at call_fail:6:' "$error_file"
grep -q 'at tests/cases/raise_stack.dia:9:' "$error_file"
rm -f "$error_file"

actual="$("$diamond" --dump-bytecode -e 'raise 42' 2>/dev/null || true)"
grep -q 'RAISE' <<<"$actual"

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/rescue.dia)"
[[ "$actual" == "diamond rescued!" ]]

actual="$("$diamond" -e $'begin\n 40 + 2\nrescue error\n 0\nend')"
[[ "$actual" == "42" ]]

actual="$("$diamond" --dump-bytecode tests/cases/rescue.dia)"
grep -q 'PUSH_RESCUE' <<<"$actual"
grep -q 'POP_RESCUE' <<<"$actual"

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/typed_rescue.dia)"
[[ "$actual" == "42" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/standard_exceptions.dia)"
[[ "$actual" == "42" ]]

actual="$("$diamond" -e $'begin\n true + 1\nrescue error: StandardError\n 42\nend')"
[[ "$actual" == "42" ]]

actual="$(DIAMOND_STRESS_GC=1 "$diamond" tests/cases/ensure.dia)"
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

actual="$($diamond tests/multifile/main.dia)"
[[ "$actual" == "[42, Hello, world]" ]]

actual="$($diamond tests/multifile/load_once_main.dia)"
[[ "$actual" == "42" ]]

if "$diamond" tests/multifile/cycle_a.dia >/dev/null 2>&1; then
    echo "circular require unexpectedly loaded" >&2
    exit 1
fi

actual="$($diamond tests/multifile/broken_main.dia 2>&1 || true)"
grep -q 'tests/multifile/broken.dia:3:16' <<<"$actual"

actual="$($diamond -e $'require "tests/multifile/math"\ndouble(21)')"
[[ "$actual" == "42" ]]

actual="$($diamond --dump-bytecode tests/multifile/main.dia)"
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

echo "303 tests passed"
