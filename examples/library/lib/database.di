# Per-worker SQLite connection, lazily opened and cached on `context` --
# same one-instance-per-worker shape RackChain (packages/rack/lib/rack/rack_chain.di)
# uses for the middleware chain itself.
class Database
  def self.path() = "library.db"
  def self.get(context)
    db = context["db"]
    if db == nil
      db = SQLite3.open(Database.path())
      context["db"] = db
    end
    db
  end
end
