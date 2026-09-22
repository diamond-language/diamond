# active_auth

Account, session, password reset, email verification, and recovery-code models backed by `active_record`.

## Installation

Install the cut at `cuts/active_auth/` and load it with `require_cut "active_auth"`. See the [Diamond package guide](https://github.com/diamond-language/diamond/blob/main/docs/packages.md).

Requires `active_record`, `cookies`.

## Usage

```ruby
require_cut "active_auth"

ActiveAuth::Account.configure(ActiveRecord::Repository.new(
  Arel.table("accounts"), build_account, "id", nil, build_account_validator(db)))
ActiveAuth::Session.configure(ActiveRecord::Repository.new(
  Arel.table("sessions"), build_session, "id"))

account = ActiveAuth::Account.new({"email": email, "username": username})
account.secure_password = password
account.save(db)  # raises ActiveRecord::ValidationError on failure

session, raw_token = ActiveAuth::Session.issue(db, account.id(), user_agent, ip_address)
# Send raw_token in a secure cookie.

# On a later request:
session = ActiveAuth::Session.from_token(db, cookie_value)
current_account = if session == nil then nil else session.account(db) end
```

## Notes

Configure each model with an `ActiveRecord::Repository` before use. `Session.issue` stores a token fingerprint and returns the raw token once; send it to the client. `Session.issue_cookie` and `from_cookie` use signed cookies. The mailer logs messages; wire up delivery in your application.
