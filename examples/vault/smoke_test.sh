#!/usr/bin/env bash
# Exercises the vault end to end under the interpreter and as a
# `diamond build` binary: storing and reading secrets, wrong passwords,
# and two kinds of tampering with the file. Set DIAMOND_BIN to use a
# diamond other than ../../build/diamond.
set -euo pipefail
cd "$(dirname "$0")"
diamond="${DIAMOND_BIN:-../../build/diamond}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# Cheap settings so the test is fast; real use keeps the defaults.
export VAULT_PASSWORD=hunter2 VAULT_BCRYPT_COST=4 VAULT_KDF_ROUNDS=1000

"$diamond" build vault.di -o "$work/vault" > "$work/build.log"
# run_vault NAME ARGS...: runs the interpreted or built vault.
run_vault() {
  local how="$1"; shift
  if [[ "$how" == interpreted ]]; then "$diamond" vault.di "$@"; else "$work/vault" "$@"; fi
}
check() {
  local how="$1" file="$work/$1.vault"
  run_vault "$how" "$file" init | grep -q "created"
  echo "correct horse battery staple" | run_vault "$how" "$file" add wifi > /dev/null
  printf 'AKIA123\n' | run_vault "$how" "$file" add aws > /dev/null
  [[ "$(run_vault "$how" "$file" list | tr '\n' ' ')" == "aws wifi " ]]
  [[ "$(run_vault "$how" "$file" get wifi)" == "correct horse battery staple" ]]
  [[ "$(run_vault "$how" "$file" verify)" == "ok" ]]
  # Nothing secret is stored in the clear.
  ! grep -q -e horse -e AKIA -e hunter2 "$file"

  local status=0
  VAULT_PASSWORD=wrong run_vault "$how" "$file" get wifi 2> "$work/err" || status=$?
  [[ "$status" == 1 ]] && grep -q "wrong password" "$work/err"

  run_vault "$how" "$file" rm aws | grep -q "removed aws"
  status=0; run_vault "$how" "$file" get aws 2> "$work/err" || status=$?
  [[ "$status" == 1 ]] && grep -q "no entry named aws" "$work/err"
}
check interpreted
check binary

# Tampering 1: flip a character inside an entry's ciphertext. AES-GCM's
# tag check rejects it, so the entry can't be read at all.
file="$work/binary.vault"
cp "$file" "$work/tampered.vault"
sed -i 's/"wifi":"\(.\)\(.\)/"wifi":"\2\1/' "$work/tampered.vault"
status=0; "$work/vault" "$work/tampered.vault" get wifi 2> "$work/err" || status=$?
[[ "$status" == 1 ]] && grep -q "failed to decrypt" "$work/err"

# Tampering 2: an entry copied under another name decrypts fine (same key),
# so only the HMAC over all entries notices.
printf 'second\n' | "$work/vault" "$file" add other > /dev/null
wifi_blob="$(sed 's/.*"wifi":"\([^"]*\)".*/\1/' "$file")"
sed "s|\"other\":\"[^\"]*\"|\"other\":\"$wifi_blob\"|" "$file" > "$work/swapped.vault"
[[ "$("$work/vault" "$work/swapped.vault" get other)" == "correct horse battery staple" ]]
status=0; "$work/vault" "$work/swapped.vault" verify 2> "$work/err" || status=$?
[[ "$status" == 1 ]] && grep -q "modified outside vault" "$work/err"

# Usage and I/O errors.
status=0; "$work/vault" 2> /dev/null || status=$?; [[ "$status" == 64 ]]
status=0; "$work/vault" "$work/missing.vault" list 2> /dev/null || status=$?; [[ "$status" == 66 ]]
status=0; VAULT_PASSWORD= "$work/vault" "$file" list 2> /dev/null || status=$?; [[ "$status" == 64 ]]
echo "vault smoke test passed"
