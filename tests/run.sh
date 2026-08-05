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
if grep -q 'CHECK_TYPE' <<<"$actual"; then
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
if grep -q 'CHECK_TYPE' <<<"$actual"; then
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
grep -Eq '^000[0-9]+ +1:[0-9]+ +ADD' <<<"$actual"

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

echo "101 tests passed"
