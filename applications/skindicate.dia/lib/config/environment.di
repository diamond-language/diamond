module SkindicateEnvironment
  def self.name()
    value = ENV["DIAMOND_ENV"]
    value = if value == nil then "development" else value end
    if value == "dev" then "development"
    elsif value == "prod" then "production"
    elsif value == "development" || value == "test" || value == "production" then value
    else
      raise ArgumentError.new("unknown DIAMOND_ENV '#{value}' -- expected development, test, or production")
    end
  end

  def self.database_path()
    override = ENV["DIAMOND_DATABASE_PATH"]
    if override != nil && override != ""
      override
    elsif SkindicateEnvironment.name() == "test"
      "skindicate_test.db"
    elsif SkindicateEnvironment.name() == "production"
      "skindicate_production.db"
    else
      # Preserve the existing development database and its data.
      "skindicate.db"
    end
  end

  def self.log_level()
    override = ENV["LOG_LEVEL"]
    if override != nil && override != ""
      override
    elsif SkindicateEnvironment.name() == "production"
      "info"
    else
      "debug"
    end
  end

  # Where uploaded skin/preview files are read from and written to --
  # always relative to the app's own directory, not the environment, so
  # public/uploads/ stays the single StaticFiles root regardless of
  # DIAMOND_ENV (unlike the database path, there's no dev/test/prod
  # split here -- the smoke test's own fixtures are small enough that
  # sharing the directory with development data is fine, and cleaning
  # it up is the same "rerun setup_db.di deliberately" story the
  # database already has).
  def self.uploads_directory()
    "public/uploads"
  end

  # The secret EncryptedCookies-style signing key for the session
  # cookie (packages/cookies' SignedCookies). A real deployment sets
  # SKINDICATE_SESSION_SECRET; the fallback exists purely so this app
  # runs with zero setup in development -- see README.md.
  def self.session_secret()
    secret = ENV["SKINDICATE_SESSION_SECRET"]
    if secret == nil then "dev-only-insecure-secret-change-me" else secret end
  end
end
