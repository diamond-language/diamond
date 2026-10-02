# Which environment the app runs in, and which database file that implies.
# Read at call time (not cached), so a test can set the variables first.
module AppEnvironment
  # DIAMOND_ENV, defaulting to development, with "dev"/"prod" accepted as
  # shorthands. Anything unrecognized raises instead of silently falling
  # back, so a typo like "porduction" cannot point the app at the wrong
  # database.
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

  # An explicit DIAMOND_DATABASE_PATH always wins (an empty string counts as
  # unset); otherwise each environment gets its own file so test runs can
  # never touch development or production data.
  def self.database_path()
    override = ENV["DIAMOND_DATABASE_PATH"]

    if override != nil && override != ""
      override
    elsif AppEnvironment.name() == "test"
      "library_test.db"
    elsif AppEnvironment.name() == "production"
      "library_production.db"
    else
      # Preserve the existing development database and its data.
      "library.db"
    end
  end
end
