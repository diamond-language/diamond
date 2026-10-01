# Per-worker SQLite connection, lazily opened and cached on `context` --
# same one-instance-per-worker shape RackChain (packages/rack/lib/rack/rack_chain.di)
# uses for the middleware chain itself.
class Database
  def self.path() = AppEnvironment.database_path()

  # `context` is a Hash the server creates fresh for each worker thread and
  # passes to every request that worker handles. Opening the connection on
  # first use and stashing it there gives exactly one connection per worker,
  # with no sharing between threads (each worker has its own heap).
  def self.get(context)
    db = context["db"]

    if db == nil
      db = SQLite3.open(Database.path())
      context["db"] = db
    end
    db
  end
end
