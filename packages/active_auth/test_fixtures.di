# Shared fixture for test.sh's own inline test cases: an in-memory
# SQLite schema plus every model configured, matching the shape
# README.md's own "Required tables" section documents (trimmed to just
# the tables this package's models actually use).
def setup_test_db()
  db = SQLite3.open(":memory:")
  db.execute("CREATE TABLE accounts (id INTEGER PRIMARY KEY, email TEXT NOT NULL UNIQUE, username TEXT NOT NULL UNIQUE, password_digest TEXT NOT NULL, email_verified_at INTEGER, role TEXT NOT NULL)")
  db.execute("CREATE TABLE sessions (id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL, token TEXT NOT NULL UNIQUE, csrf_token TEXT NOT NULL, user_agent TEXT, ip_address TEXT, expires_at INTEGER NOT NULL)")
  db.execute("CREATE TABLE backup_codes (id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL, code_digest TEXT NOT NULL, used_at INTEGER)")
  db.execute("CREATE TABLE email_verification_tokens (id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL, token TEXT NOT NULL UNIQUE, expires_at INTEGER NOT NULL)")
  db.execute("CREATE TABLE password_reset_tokens (id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL, token TEXT NOT NULL UNIQUE, expires_at INTEGER NOT NULL, used_at INTEGER)")

  ActiveAuth::Account.configure(ActiveRecord::Repository.new(Arel.table("accounts"), build_account, "id", nil, build_account_validator(db)))
  ActiveAuth::Session.configure(ActiveRecord::Repository.new(Arel.table("sessions"), build_session, "id"))
  ActiveAuth::BackupCode.configure(ActiveRecord::Repository.new(Arel.table("backup_codes"), build_backup_code, "id"))
  ActiveAuth::EmailVerificationToken.configure(ActiveRecord::Repository.new(Arel.table("email_verification_tokens"), build_email_verification_token, "id"))
  ActiveAuth::PasswordResetToken.configure(ActiveRecord::Repository.new(Arel.table("password_reset_tokens"), build_password_reset_token, "id"))
  db
end

def create_test_account(db, email = "alice@example.com", username = "alice")
  account = ActiveAuth::Account.new({"email": email, "username": username})
  account.secure_password = "correct horse battery staple"
  account.save(db)
  account
end
