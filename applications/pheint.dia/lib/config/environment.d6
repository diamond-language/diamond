module PheintEnvironment
  def self.name()
    value = ENV["DIAMOND_ENV"]
    value = if value == nil then "development" else value end
    if value == "dev" then "development"
    elsif value == "prod" then "production"
    elsif value == "development" || value == "test" || value == "production" then value
    else
      raise ArgumentError.new(
        "unknown DIAMOND_ENV '#{value}' -- expected development, test, or production")
    end
  end

  def self.port() -> Int
    value = ENV["PORT"]
    if value == nil || value == "" then 18100 else value.to_i() end
  end

  def self.log_level()
    override = ENV["LOG_LEVEL"]
    if override != nil && override != ""
      override
    elsif PheintEnvironment.name() == "production"
      "info"
    else
      "debug"
    end
  end


  def self.database_path()
    override = ENV["DIAMOND_DATABASE_PATH"]
    if override != nil && override != ""
      override
    elsif PheintEnvironment.name() == "test"
      "pheint_test.db"
    elsif PheintEnvironment.name() == "production"
      "pheint_production.db"
    else
      "pheint_development.db"
    end
  end
end
