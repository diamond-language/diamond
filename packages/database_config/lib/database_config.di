# JSON-backed database connection configuration.
#
# A file may contain one connection object directly, or a Hash of named
# environments. DatabaseConfig.load(path, "production") selects one named
# object; DatabaseConfig.open(config) opens the corresponding native handle.
class DatabaseConfig
  def self.read_file(path: String) -> String
    file = File.open(path, "r")
    begin
      file.read()
    ensure
      file.close()
    end
  end

  def self.adapter(config: Hash) -> String
    adapter = config["adapter"]
    if !(adapter is String)
      raise ArgumentError.new("database config requires a String 'adapter'")
    end
    adapter.downcase()
  end

  def self.string(config: Hash, key: String, fallback = nil)
    value = config[key]
    value = fallback if value == nil
    if value == nil || !(value is String)
      raise ArgumentError.new("database config requires String '#{key}'")
    end
    value
  end

  def self.integer(config: Hash, key: String, fallback = nil)
    value = config[key]
    value = fallback if value == nil
    if value == nil || !(value is Int)
      raise ArgumentError.new("database config requires Int '#{key}'")
    end
    value
  end

  def self.password(config: Hash) -> String
    env_name = config["password_env"]
    if env_name != nil
      if !(env_name is String)
        raise ArgumentError.new("database config 'password_env' must be a String")
      end
      value = ENV[env_name]
      if value == nil
        raise ArgumentError.new("database password environment variable '#{env_name}' is not set")
      end
      value
    else
      DatabaseConfig.string(config, "password", "")
    end
  end

  def self.validate(config: Hash) -> Bool
    adapter = DatabaseConfig.adapter(config)
    if adapter == "sqlite" || adapter == "sqlite3"
      DatabaseConfig.string(config, "database")
    elsif adapter == "postgres" || adapter == "postgresql"
      if config["connection"] == nil
        DatabaseConfig.string(config, "host", "127.0.0.1")
        DatabaseConfig.integer(config, "port", 5432)
        DatabaseConfig.string(config, "database")
        DatabaseConfig.string(config, "user")
        DatabaseConfig.password(config)
      else
        DatabaseConfig.string(config, "connection")
      end
    elsif adapter == "mysql" || adapter == "mariadb"
      DatabaseConfig.string(config, "host", "127.0.0.1")
      DatabaseConfig.integer(config, "port", 3306)
      DatabaseConfig.string(config, "database")
      DatabaseConfig.string(config, "user")
      DatabaseConfig.password(config)
    else
      raise ArgumentError.new("unsupported database adapter '#{adapter}'")
    end
    true
  end

  def self.load(path: String, name = nil) -> Hash
    document = JSON.parse(DatabaseConfig.read_file(path))
    if !(document is Hash)
      raise ArgumentError.new("database config root must be a JSON object")
    end
    config = if name == nil then document else document[name] end
    if config == nil
      raise ArgumentError.new("database config has no entry '#{name}'")
    end
    if !(config is Hash)
      raise ArgumentError.new("database config entry must be a JSON object")
    end
    DatabaseConfig.validate(config)
    config
  end

  def self.postgresql_value(config: Hash, key: String, fallback = nil) -> String
    value = DatabaseConfig.string(config, key, fallback)
    if value.include?("'") || value.include?("\\")
      raise ArgumentError.new(
        "database config '#{key}' cannot contain a quote or backslash; use a complete 'connection' string")
    end
    value
  end

  def self.postgresql_connection(config: Hash) -> String
    connection = config["connection"]
    if connection != nil
      DatabaseConfig.string(config, "connection")
    else
      # Quoting permits whitespace. Ambiguous quote/backslash values are
      # rejected with guidance to provide libpq's complete conninfo directly.
      host = DatabaseConfig.postgresql_value(config, "host", "127.0.0.1")
      database = DatabaseConfig.postgresql_value(config, "database")
      user = DatabaseConfig.postgresql_value(config, "user")
      password = DatabaseConfig.password(config)
      if password.include?("'") || password.include?("\\")
        raise ArgumentError.new(
          "database config password cannot contain a quote or backslash; use a complete 'connection' string")
      end
      port = DatabaseConfig.integer(config, "port", 5432)
      "host='#{host}' port=#{port} dbname='#{database}' user='#{user}' password='#{password}'"
    end
  end

  def self.open(config: Hash)
    DatabaseConfig.validate(config)
    adapter = DatabaseConfig.adapter(config)
    if adapter == "sqlite" || adapter == "sqlite3"
      SQLite3.open(DatabaseConfig.string(config, "database"),
        DatabaseConfig.string(config, "mode", "rwc"))
    elsif adapter == "postgres" || adapter == "postgresql"
      PostgreSQL.open(DatabaseConfig.postgresql_connection(config))
    else
      MySQL.open(DatabaseConfig.string(config, "host", "127.0.0.1"),
        DatabaseConfig.string(config, "user"), DatabaseConfig.password(config),
        DatabaseConfig.string(config, "database"),
        DatabaseConfig.integer(config, "port", 3306))
    end
  end
end
