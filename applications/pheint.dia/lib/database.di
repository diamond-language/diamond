def build_pheint_query_logger(logger, context)
  def log_query(event)
    phase = event["phase"]
    correlation = context["log_context"]
    correlation = {} if correlation == nil
    fields = event.merge(correlation).merge({
      "orm": "active_record", "builder": "arel"
    })
    if phase == "failed"
      logger.error("database.query.failed", fields)
    else
      logger.debug("database.query.#{phase}", fields)
    end
  end
  log_query
end

class PheintDatabase
  def self.path() = PheintEnvironment.database_path()

  def self.get(context)
    db = context["db"]
    if db == nil
      connection = SQLite3.open(PheintDatabase.path())
      connection.execute("PRAGMA foreign_keys = ON")
      connection.execute("PRAGMA journal_mode = WAL")
      connection.execute("PRAGMA busy_timeout = 5000")
      db = ActiveRecord::InstrumentedConnection.new(
        connection, build_pheint_query_logger(PheintLogger.get(context), context))
      context["db"] = db
      PheintLogger.get(context).info("database.connection.opened", {
        "adapter": "sqlite3", "database": PheintDatabase.path(),
        "environment": PheintEnvironment.name(), "foreign_keys": true,
        "journal_mode": "wal", "busy_timeout_ms": 5000
      })
    end
    db
  end
end

def build_account_validator(db)
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("email"),
    ActiveRecord::Validators.format("email", Regexp.new("^[^@ ]+@[^@ ]+\\.[^@ ]+$")),
    ActiveRecord::Validators.uniqueness(db, Arel.table("accounts"), "email")
  ])
end

def build_player_validator(db)
  ActiveRecord::Validators.combine([
    ActiveRecord::Validators.presence("handle"),
    ActiveRecord::Validators.length("handle", 3, 30),
    ActiveRecord::Validators.format("handle", Regexp.new("^[A-Za-z0-9_]+$")),
    ActiveRecord::Validators.uniqueness(db, Arel.table("players"), "handle")
  ])
end

def ensure_pheint_models_configured(context)
  if context["models_configured"] == nil
    db = PheintDatabase.get(context)
    Player.configure(ActiveRecord::Repository.new(
      Arel.table("players"), build_player, "id", nil, build_player_validator(db),
      nil, nil, nil, ["id", "account_id", "handle"]))
    Session.configure(ActiveRecord::Repository.new(
      Arel.table("sessions"), build_session, "id", nil, nil, nil, nil, nil,
      ["id", "account_id", "token", "expires_at"]))
    Account.configure(ActiveRecord::Repository.new(
      Arel.table("accounts"), build_account, "id", nil, build_account_validator(db),
      nil, nil, nil, ["id", "email", "password_digest"], nil, [
        ActiveRecord::AssociationReflection.new(
          "player", "has_one", Player.repository(), "account_id", "id"),
        ActiveRecord::AssociationReflection.new(
          "sessions", "has_many", Session.repository(), "account_id", "id")
      ]))
    Player.configure(ActiveRecord::Repository.new(
      Arel.table("players"), build_player, "id", nil, build_player_validator(db),
      nil, nil, nil, ["id", "account_id", "handle"], nil, [
        ActiveRecord::AssociationReflection.new(
          "account", "belongs_to", Account.repository(), "account_id", "id")
      ]))
    Session.configure(ActiveRecord::Repository.new(
      Arel.table("sessions"), build_session, "id", nil, nil, nil, nil, nil,
      ["id", "account_id", "token", "expires_at"], nil, [
        ActiveRecord::AssociationReflection.new(
          "account", "belongs_to", Account.repository(), "account_id", "id")
      ]))
    context["models_configured"] = true
    PheintLogger.get(context).info("models.configured", {"model_count": 3})
  end
end
