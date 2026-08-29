#!/usr/bin/env bash
set -euo pipefail

# Requires the diamond binary on PATH, or DIAMOND_BIN pointing at one
# (e.g. DIAMOND_BIN=../../build/diamond bash test.sh, or `make
# test-active-auth-package` from the repo root, which sets this up
# already).
diamond="${DIAMOND_BIN:-diamond}"
cd "$(dirname "$0")"

count=0

run_case() {
    local script="$1"
    "$diamond" -e "require \"$(pwd)/lib/active_auth\"
require \"$(pwd)/test_fixtures\"
$script"
}

assert_eq() {
    local actual="$1" expected="$2"
    if [[ "$actual" != "$expected" ]]; then
        echo "expected '$expected', got '$actual'" >&2
        exit 1
    fi
    count=$((count + 1))
}

# --- account creation normalizes email/username and hashes the password ---
actual="$(run_case '
db = setup_test_db()
account = create_test_account(db, "  Alice@Example.com ", "  Alice Smith  ")
"#{account.email()} #{account.username()} #{account.role()} #{account.authenticate("correct horse battery staple")} #{account.authenticate("wrong")}"
')"
assert_eq "$actual" "alice@example.com alice-smith member true false"

# --- duplicate email is rejected ---
actual="$(run_case '
db = setup_test_db()
create_test_account(db, "alice@example.com", "alice")
begin
  create_test_account(db, "alice@example.com", "someoneelse")
  "not rejected"
rescue error: ActiveRecord::ValidationError
  error.errors()[0]
end
')"
assert_eq "$actual" "email has already been taken"

# --- email verification round-trip ---
actual="$(run_case '
db = setup_test_db()
account = create_test_account(db)
before = account.email_verified?()
account.verify_email!(db)
"#{before} #{account.email_verified?()}"
')"
assert_eq "$actual" "false true"

# --- verifying email does not conflict with the account's own unique
# fields (the exclude_id fix on ActiveRecord::Repository#update) ---
actual="$(run_case '
db = setup_test_db()
account = create_test_account(db)
account.verify_email!(db)
"ok"
')"
assert_eq "$actual" "ok"

# --- elevated?/roles ---
actual="$(run_case '
db = setup_test_db()
member = ActiveAuth::Account.new({"email": "m@example.com", "username": "member"})
member.secure_password = "correct horse battery staple"
member.save(db)
admin = ActiveAuth::Account.new({"email": "a@example.com", "username": "admin", "role": "admin"})
admin.secure_password = "correct horse battery staple"
admin.save(db)
"#{member.elevated?()} #{admin.elevated?()}"
')"
assert_eq "$actual" "false true"

# --- session issue/lookup, and a forged/tampered token is rejected
# cleanly (never raises, just returns nil) ---
actual="$(run_case '
db = setup_test_db()
account = create_test_account(db)
session, raw = ActiveAuth::Session.issue(db, account.id(), "test-agent", "127.0.0.1")
found = ActiveAuth::Session.from_token(db, raw)
forged = ActiveAuth::Session.from_token(db, "not-a-real-token")
stored_fingerprint_rejected = ActiveAuth::Session.from_token(db, session.token())
"#{session.expired?()} #{found.id() == session.id()} #{forged} #{stored_fingerprint_rejected} #{ActiveAuth::Session.from_token(db, nil)} #{ActiveAuth::Session.from_token(db, "")}"
')"
assert_eq "$actual" "false true nil nil nil nil"

# --- backup codes: generation, matching (with separator variants),
# and single use ---
actual="$(run_case '
db = setup_test_db()
account = create_test_account(db)
codes = ActiveAuth::BackupCode.generate_for!(db, account.id())
first = account.backup_codes(db)[0]
plain = first.match?(codes[0])
no_dashes = first.match?(codes[0].gsub(Regexp.new("-"), "").downcase())
spaced = first.match?(codes[0].gsub(Regexp.new("-"), " "))
wrong = first.match?("totally-wrong-code")
first.consume!(db)
"#{codes.length()} #{plain} #{no_dashes} #{spaced} #{wrong} #{first.used?()} #{first.match?(codes[0])}"
')"
assert_eq "$actual" "8 true true true false true false"

# --- regenerating backup codes replaces the old batch entirely ---
actual="$(run_case '
db = setup_test_db()
account = create_test_account(db)
first_batch = ActiveAuth::BackupCode.generate_for!(db, account.id())
ActiveAuth::BackupCode.generate_for!(db, account.id())
still_valid = account.backup_codes(db)[0].match?(first_batch[0])
account.backup_codes(db).length()
')"
assert_eq "$actual" "8"

# --- email verification token issue/lookup ---
actual="$(run_case '
db = setup_test_db()
account = create_test_account(db)
token, raw = ActiveAuth::EmailVerificationToken.issue(db, account.id())
found = ActiveAuth::EmailVerificationToken.from_token(db, raw)
"#{found.id() == token.id()} #{ActiveAuth::EmailVerificationToken.from_token(db, "wrong")}"
')"
assert_eq "$actual" "true nil"

# --- password reset token is single-use: consuming twice raises ---
actual="$(run_case '
db = setup_test_db()
account = create_test_account(db)
token, raw = ActiveAuth::PasswordResetToken.issue(db, account.id())
found = ActiveAuth::PasswordResetToken.from_token(db, raw)
found.consume!(db)
gone = ActiveAuth::PasswordResetToken.from_token(db, raw)
begin
  found.consume!(db)
  "double consume succeeded"
rescue error: RuntimeError
  "#{gone} #{error.message()}"
end
')"
assert_eq "$actual" "nil token already used"

# --- Mailer no-ops under DIAMOND_ENV=test rather than printing ---
actual="$(DIAMOND_ENV=test run_case '
db = setup_test_db()
account = create_test_account(db)
token, raw = ActiveAuth::EmailVerificationToken.issue(db, account.id())
ActiveAuth::Mailer.send_verification(account, raw)
"no crash"
')"
assert_eq "$actual" "no crash"

echo "$count active_auth tests passed"
