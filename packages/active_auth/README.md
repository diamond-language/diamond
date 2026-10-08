# active_auth

Account, session, password reset, email verification, and recovery-code models backed by `active_record`.

## Installation

`active_auth` is not published to the registry yet. Until it is, copy `packages/active_auth` from a checkout of the [Diamond repository](https://github.com/diamond-language/diamond) into your project as `cuts/active_auth/`, then load it with `require_cut "active_auth"`. `facet update` leaves hand-copied cuts in place.

It depends on `active_record` and `cookies`, which are published. Add them with `facet`:

```sh
facet add active_record --registry https://cuts.dilang.tech --version "^0.19.0"
facet add cookies --registry https://cuts.dilang.tech --version "^0.1.0"
facet update
```

## Usage

Accounts and sessions are `active_record` models. Create the two tables, configure each
model with a repository, then sign people up and look sessions up from a signed cookie.
This complete program does that over HTTP with `gremlin`:

```ruby
# app.di
require_cut "gremlin"
require_cut "active_auth"

SECRET = ENV["SESSION_SECRET"] || "dev-only-secret-change-me"

def open_database()
  db = SQLite3.open("app.db")
  db.execute("CREATE TABLE IF NOT EXISTS accounts (
    id INTEGER PRIMARY KEY, email TEXT NOT NULL UNIQUE, username TEXT NOT NULL UNIQUE,
    password_digest TEXT, email_verified_at INTEGER, role TEXT NOT NULL DEFAULT 'member')")
  db.execute("CREATE TABLE IF NOT EXISTS sessions (
    id INTEGER PRIMARY KEY, account_id INTEGER NOT NULL, token TEXT NOT NULL UNIQUE,
    csrf_token TEXT NOT NULL, user_agent TEXT, ip_address TEXT, expires_at INTEGER NOT NULL)")
  ActiveAuth::Account.configure(ActiveRecord::Repository.new(
    Arel.table("accounts"), build_account, "id", nil, build_account_validator(db)))
  ActiveAuth::Session.configure(ActiveRecord::Repository.new(
    Arel.table("sessions"), build_session, "id"))
  db
end

class Pages
  def self.sign_up(db, params)
    account = ActiveAuth::Account.new({"email": params["email"], "username": params["username"]})
    account.secure_password = params["password"]
    begin
      account.save(db)
    rescue error: ActiveRecord::ValidationError
      return [422, {"Content-Type": "text/plain"}, error.errors().join("\n")]
    end
    session, set_cookie = ActiveAuth::Session.issue_cookie(db, account.id(), secret: SECRET)
    [302, {"Location": "/", "Set-Cookie": set_cookie}, "welcome"]
  end

  def self.home(db, request)
    session = ActiveAuth::Session.from_cookie(request, db, secret: SECRET)
    if session == nil
      [200, {"Content-Type": "text/plain"}, "signed out\n"]
    else
      [200, {"Content-Type": "text/plain"}, "signed in as #{session.account(db).username()}\n"]
    end
  end
end

def app(request, context)
  db = context["db"]
  if db == nil
    db = open_database()
    context["db"] = db
  end
  if request["method"] == "POST" && request["path"] == "/sign_up"
    params = {}   # naive form parsing; use a real decoder for percent-encoded input
    request["body"].split("&").each() do |pair|
      k, v = pair.split("=")
      params[k] = v
    end
    Pages.sign_up(db, params)
  else
    Pages.home(db, request)
  end
end

gremlin_serve(8080, app)
```

```sh
SESSION_SECRET=$(openssl rand -hex 32) diamond app.di
curl -c jar -b jar -d "email=ada@example.com&username=ada&password=correcthorse" localhost:8080/sign_up
curl -c jar -b jar localhost:8080/        # signed in as ada
```

The database connection is opened lazily inside `app` and kept in the worker's `context`
because each `gremlin_serve` worker thread has its own VM. A persistent `app.db` is used
here; in a real project, open the connection through your `database_config` settings.

## Notes

- `account.secure_password = password` stores a bcrypt digest; `account.authenticate(password)`
  checks one. `save` raises `ActiveRecord::ValidationError` (see `error.errors()`) for an invalid
  email, a taken or malformed username, or an unknown role.
- `Session.issue(db, account_id, user_agent, ip_address)` stores only a fingerprint of the token
  and returns `[session, raw_token]`; the raw token is shown once, so send it to the client.
  `Session.from_token(db, raw)` finds a non-expired session. `Session.issue_cookie` and
  `Session.from_cookie` wrap these with a signed `Set-Cookie` value, as above; sessions last 30 days.
- The package also provides password reset tokens, email verification tokens, and backup
  codes (`ActiveAuth::PasswordResetToken`, `EmailVerificationToken`, `BackupCode`). Each is a model
  you configure with a repository and its own table, like `Account` and `Session`.
- The mailer logs messages; wire up delivery in your application.
