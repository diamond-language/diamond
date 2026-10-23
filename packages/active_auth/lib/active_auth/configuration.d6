module ActiveAuth

  # App-wide settings, ported from ActiveAuth's own Configuration. Diamond
  # has no `ActiveAuth.configure do |c| ... end` yield-a-mutable-object
  # block DSL equivalent worth inventing here -- `.configure(options)`
  # setting class variables is this codebase's own established pattern
  # (packages/cookies' CookieSession, packages/rack's RateLimit/
  # SecurityHeaders all do this), reused rather than a one-off DSL. Must
  # be called once per worker before use, same "each Thread.new-spawned
  # gremlin_serve worker gets its own independent DiamondVm" reasoning
  # those other .configure-based classes already document.
  class Configuration
    def self.configure(options: Hash = {})
      @@app_name = if options["app_name"] == nil then "App" else options["app_name"] end
      @@base_url = if options["base_url"] == nil then "http://localhost:3000" else options["base_url"] end
      @@smtp_from = if options["smtp_from"] == nil then "noreply@localhost" else options["smtp_from"] end
      @@totp_issuer = if options["totp_issuer"] == nil then @@app_name else options["totp_issuer"] end
    end

    def self.app_name()
      if @@app_name == nil then Configuration.configure() end
      @@app_name
    end
    def self.base_url()
      if @@app_name == nil then Configuration.configure() end
      @@base_url
    end
    def self.smtp_from()
      if @@app_name == nil then Configuration.configure() end
      @@smtp_from
    end
    def self.totp_issuer()
      if @@app_name == nil then Configuration.configure() end
      @@totp_issuer
    end
  end

end
