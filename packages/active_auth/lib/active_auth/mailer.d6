module ActiveAuth

  # Ported from ActiveAuth's own Mailer -- default implementation only:
  # logs the message rather than sending real email. The Ruby original's
  # real-SMTP path (require "mail"; Mail.deliver do ... end, gated on
  # ENV["SMTP_HOST"]) isn't ported -- Diamond has no outbound-SMTP client
  # in this codebase, and adding one is real new scope beyond porting
  # this gem's own logic. A consuming app wanting real delivery should
  # reopen `ActiveAuth::Mailer.deliver` (Diamond class reopening,
  # docs/syntax.md's "Reopening" section) with its own transport, the
  # same override point the Ruby original documents.
  #
  # No-ops under DIAMOND_ENV=test, matching the Ruby original's own
  # RACK_ENV/RAILS_ENV == "test" guard, so a real app's test suite
  # doesn't get log noise for every verification/reset email a test
  # sends.
  class Mailer
    def self.test_env?() -> Bool = ENV["DIAMOND_ENV"] == "test"

    def self.deliver(to, subject, body)
      if Mailer.test_env?()
        return
      end
      puts("[MAILER] To: #{to}\nSubject: #{subject}\n\n#{body}\n------------------------------------------------------------")
    end

    def self.send_verification(account, raw_token)
      url = "#{Configuration.base_url()}/verify-email/#{raw_token}"
      body = "Welcome to #{Configuration.app_name()}, #{account.username()}.\n\n" +
        "Verify your email address by visiting:\n#{url}\n\n" +
        "This link expires in 24 hours.\nIf you didn't create an account, ignore this email."
      Mailer.deliver(account.email(), "Verify your #{Configuration.app_name()} account", body)
    end

    def self.send_password_reset(account, raw_token)
      url = "#{Configuration.base_url()}/reset-password/#{raw_token}"
      body = "Password reset requested for @#{account.username()}.\n\n" +
        "Visit this link to choose a new password:\n#{url}\n\n" +
        "This link expires in 1 hour.\nIf you didn't request a reset, you can ignore this email."
      Mailer.deliver(account.email(), "Reset your #{Configuration.app_name()} password", body)
    end
  end

end
