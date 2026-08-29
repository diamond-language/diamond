# active_auth

Accounts, sessions, email verification, password reset, and 2FA
recovery codes -- ported from MaquinasStack's private Ruby `ActiveAuth`
gem (`active_auth/`). Everything here is `ActiveRecord::Model`-based,
following `packages/active_record`'s own model/repository/validator
conventions exactly (see e.g. `applications/skindicate.dia/lib/models/
user.di` for the closest existing Diamond precedent this mirrors).

## What's ported, what isn't (yet)

**Ported**, matching the Ruby original's own behavior:
- `ActiveAuth::Account` -- email/username/password (bcrypt via
  `ActiveRecord::Model#secure_password=`/`#authenticate`, inherited for
  free), a site-wide `role` (`member`/`moderator`/`admin`/`dev`),
  email verification state. Email and username are normalized
  (lowercase/trimmed, username whitespace collapsed to hyphens) at
  construction time -- see the Diamond-specific notes below for why
  that's different from the Ruby original's `before_validation` hooks.
- `ActiveAuth::Session` -- token-based, 30-day expiry. The stored
  `token` column is always a SHA-256 fingerprint, never the raw
  presentable value; `self.issue` returns `[session, raw_token]` once,
  for the caller to put in a cookie.
- `ActiveAuth::BackupCode` -- one-time 2FA recovery codes, bcrypt-
  hashed, accepting common separator variants (`A1B2-C3D4-E5F6`,
  `a1b2 c3d4 e5f6`, `A1B2C3D4E5F6` all match the same code).
- `ActiveAuth::EmailVerificationToken` / `ActiveAuth::PasswordResetToken`
  -- same fingerprint-only-storage shape as `Session`; the latter is
  single-use (`#consume!` raises if already used).
- `ActiveAuth::Mailer` -- logs the message rather than sending real
  email (see below); no-ops entirely under `DIAMOND_ENV=test`.
- `ActiveAuth::Configuration` -- `app_name`/`base_url`/`smtp_from`/
  `totp_issuer`, `.configure(options)`-style (see Diamond-specific
  notes).

**Not ported**:
- **Multi-persona support** (`Persona`/`PersonaMembership`/`PersonaEvent`,
  plus `ArtistPersona`/`BotPersona`/`GroupPersona`/`UnclaimedPersona`
  subtypes) -- one account owning/co-managing several public-facing
  identities is a real, separate feature, not just a Diamond-language
  adaptation. Deferred to whichever phase actually wires this package
  into an app and needs to decide whether that app wants multiple
  identities per account at all.
- **`TotpCredential`** (TOTP/RFC 6238 2FA codes) -- the Ruby original
  uses `rotp`, which needs HMAC-SHA1. Diamond's native `HMAC` only
  exposes `.sha256` today; TOTP's standard algorithm needs a new native
  primitive (`HMAC.sha1`, mechanically the same OpenSSL `EVP_sha1()`
  swap the existing `HMAC.sha256`/`Cipher` primitives already
  established) before this can be ported faithfully -- pure-Diamond
  SHA-1 isn't practical without bitwise operators, which Diamond
  doesn't have. `BackupCode` (bcrypt-based, no TOTP needed) is fully
  ported and usable on its own as a 2FA recovery mechanism even without
  the primary TOTP code path.
- **HaveIBeenPwned password checking** -- needs an outbound HTTP client
  to a third-party API; Diamond's `packages/http` is a server, not a
  client. Not attempted.
- **Real SMTP delivery** -- `ActiveAuth::Mailer.deliver` only logs;
  reopen it (Diamond class reopening) with a real transport if a
  consuming app needs actual email delivery.

## Diamond-specific notes

- **Namespaced, not plain top-level classes.** The Ruby original
  deliberately defines top-level `Account`/`Session`/etc. (not
  `ActiveAuth::Account`) so they behave like ordinary host-app models
  in a single-app Ruby process. Diamond has one flat global namespace
  across every `require`d package in the same program, so a consuming
  app defining its *own* `Session`/`Account` class (as
  `examples/project_board` and `applications/skindicate.dia` both
  already do) would silently collide/reopen this package's classes
  instead of staying separate. Every model here lives under
  `module ActiveAuth` instead.
- **Normalization happens at construction, not via a save hook.**
  Ruby's `before_validation` runs before validation on every save;
  Diamond's closest equivalent, `Repository`'s `before_save` callback,
  runs *after* validation (confirmed directly -- `Repository#create`/
  `#update` call `self.validate!` first), so a before_save-based
  normalizer would validate the raw, unnormalized input and never see
  its own output. `Account#initialize` normalizes email/username
  directly instead. The one gap this doesn't cover: reassigning
  `account.email = "Mixed@Case.com"` after construction bypasses
  normalization (`attr_accessor`'s plain setter has no hook of its
  own) -- construct a new `Account` with updated attributes instead.
- **`self.issue` instead of a `before_create` hook.** `Session`/
  `EmailVerificationToken`/`PasswordResetToken` all generate their own
  raw token server-side. Ruby stashes the raw value in a non-persisted
  `@presented_token` ivar inside a `before_create` hook, readable back
  off the same in-memory object after `.create!` returns. Diamond's
  `Repository` `before_save` callback only receives a plain attributes
  `Hash`, not the calling `Model` instance, so there's nothing to stash
  an ivar onto -- each `self.issue(db, ...)` factory does the generate-
  then-save-then-return-both explicitly instead, returning
  `[record, raw_token]`.
- **A real `packages/active_record` bug found and fixed while porting
  this**: `Repository#update` validated attributes *before* excluding
  the record's own row from a `uniqueness` check, so saving *any*
  already-persisted record with a uniqueness-validated field (e.g.
  `Account#verify_email!`, which just flips one unrelated column)
  always failed, flagging the record as conflicting with itself. Fixed
  by threading the record's own id through `Repository#update` ->
  `#validate!` -> every validator as a new `exclude_id` parameter
  (`nil` on `#create`); `ActiveRecord::Validators.uniqueness` now
  excludes it from the query. This is a real, general fix to
  `packages/active_record` itself, not an active_auth-specific
  workaround -- see that package's own README and
  `tests/cases/active_record_validators.di`'s new regression test.
- Two new, real Diamond language constraints found while porting (see
  `packages/active_karma/README.md` for the first one, found there):
  a top-level function referencing a namespaced class (`ActiveAuth::
  Account`) needs that class already declared *earlier in the same
  file* -- unlike a plain top-level class, `Module::Class` doesn't get
  the declaration-discovery pass's forward-reference treatment. Every
  `build_x`-style mapper function here is placed *after* its module
  block for exactly this reason (the reverse direction -- a method
  calling a not-yet-declared top-level function -- works fine). And: a
  `def self.x` singleton method must be defined before any sibling
  method *of any kind* (instance or singleton) in the same class that
  calls it by qualified name -- previously confirmed only for
  singleton-calling-singleton; this session reconfirmed it also
  applies to an instance method (`initialize`) calling a sibling
  `self.` method.

## Usage

```ruby
require "../../active_auth/lib/active_auth"

# once per worker, matching every other .configure-based package here
ActiveAuth::Account.configure(ActiveRecord::Repository.new(
  Arel.table("accounts"), build_account, "id", nil, build_account_validator(db)))
ActiveAuth::Session.configure(ActiveRecord::Repository.new(
  Arel.table("sessions"), build_session, "id"))
# ... BackupCode, EmailVerificationToken, PasswordResetToken the same way

account = ActiveAuth::Account.new({"email": email, "username": username})
account.secure_password = password
account.save(db)  # raises ActiveRecord::ValidationError on failure

session, raw_token = ActiveAuth::Session.issue(db, account.id(), user_agent, ip_address)
# ... set raw_token in a cookie ...

# on a later request:
session = ActiveAuth::Session.from_token(db, cookie_value)
current_account = if session == nil then nil else session.account(db) end
```

## Test

```sh
make test-active-auth-package
# or: DIAMOND_BIN=../../build/diamond bash test.sh
```
