module AppEnvironment
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
    elsif AppEnvironment.name() == "test"
      "project_board_test.db"
    elsif AppEnvironment.name() == "production"
      "project_board_production.db"
    else
      # Preserve the existing development database and its data.
      "project_board.db"
    end
  end

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
