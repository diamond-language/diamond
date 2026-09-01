class Database
  def self.path() = SkindicateEnvironment.database_path()
  def self.get(context)
    db = context["db"]
    if db == nil
      db = SQLite3.open(Database.path())
      db.execute("PRAGMA foreign_keys = ON")
      db.execute("PRAGMA journal_mode = WAL")
      db.execute("PRAGMA busy_timeout = 5000")
      context["db"] = db
      RequestLogging.get(context).info("database.connection.opened", {"adapter": "sqlite3", "database": Database.path(), "environment": SkindicateEnvironment.name()})
    end
    db
  end
end
