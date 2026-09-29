#!/usr/bin/env bash
# Runs exact through the interpreter and as a `diamond build` binary:
# solves a system with a fractional answer, reports a contradictory and an
# underdetermined system, computes a determinant, inverts the order-5
# Hilbert matrix (whose exact inverse is all integers, and which the tool
# checks by multiplying back to the identity), and checks every error path.
# Set DIAMOND_BIN to use a diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

"$diamond" build exact.di -o "$work/exact" > "$work/build.log"

status_of() { local s=0; "$@" > /dev/null 2>&1 || s=$?; echo "$s"; }

for exact in "$diamond exact.di" "$work/exact"; do
  $exact solve testdata/system.txt | cmp testdata/system.expected -
  $exact inverse testdata/hilbert.txt | cmp testdata/hilbert_inverse.expected -
  [[ "$($exact det testdata/hilbert.txt)" == "1/266716800000" ]]
  [[ "$($exact det testdata/singular.txt)" == "0" ]]
  [[ "$($exact det testdata/system.txt 2>&1 || true)" == *"needs a square matrix"* ]]

  out="$($exact solve testdata/contradiction.txt || true)"
  [[ "$out" == "no solution: the equations contradict each other" ]]
  out="$($exact solve testdata/underdetermined.txt || true)"
  [[ "$out" == "infinitely many solutions: x3 can be chosen freely" ]]
  [[ "$($exact inverse testdata/singular.txt 2>&1 || true)" == "matrix is singular" ]]
  [[ "$($exact solve testdata/ragged.txt 2>&1 || true)" == *"ragged matrix"* ]]

  [[ "$(status_of $exact solve testdata/system.txt)" == 0 ]]
  [[ "$(status_of $exact solve testdata/contradiction.txt)" == 1 ]]
  [[ "$(status_of $exact solve testdata/ragged.txt)" == 65 ]]
  [[ "$(status_of $exact solve testdata/missing.txt)" == 66 ]]
  [[ "$(status_of $exact)" == 64 ]]
  [[ "$(status_of $exact bogus testdata/system.txt)" == 64 ]]
done

echo "exact smoke test passed"
