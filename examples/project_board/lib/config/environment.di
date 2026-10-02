# Which environment the app runs in, and the database and log level that
# implies. Read at call time, so tests can set the variables first.
module AppEnvironment
  # DIAMOND_ENV, defaulting to development ("dev"/"prod" are shorthands).
  # An unrecognized value raises rather than falling back, so a typo cannot
  # silently point the app at the wrong database.
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

  # DIAMOND_DATABASE_PATH wins if set and non-empty; otherwise each
  # environment has its own file so tests never touch real data.
  def self.database_path()
    override = ENV["DIAMOND_DATABASE_PATH"]

    if override != nil && override != ""
      override
    elsif AppEnvironment.name() == "test"
      "project_board_test.db"
    elsif AppEnvironment.name() == "production"
      "project_board_production.db"
    else
      # Preserve the existing development database and its data.
      "project_board.db"
    end
  end

  # LOG_LEVEL wins; otherwise production is quieter ("info") than the
  # other environments ("debug", which includes every SQL statement).
  def self.log_level()
    override = ENV["LOG_LEVEL"]

    if override != nil && override != ""
      override
    elsif AppEnvironment.name() == "production"
      "info"
    else
      "debug"
    end
  end
end
