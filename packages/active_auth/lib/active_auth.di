# ActiveAuth: accounts, sessions, email verification, password reset,
# and 2FA recovery codes -- ported from MaquinasStack's private Ruby
# ActiveAuth gem. See README.md for what's ported, what's deliberately
# not (multi-persona support, TOTP verification, HaveIBeenPwned
# checking, real SMTP delivery), and the Diamond-specific adaptations
# (module-namespaced classes instead of Ruby's plain top-level ones;
# an explicit `self.issue` factory instead of a before_create hook).
require "../../active_record/lib/active_record"
require "../../cookies/lib/cookies"
require "./active_auth/configuration"
require "./active_auth/account"
require "./active_auth/session"
require "./active_auth/backup_code"
require "./active_auth/email_verification_token"
require "./active_auth/password_reset_token"
require "./active_auth/mailer"
