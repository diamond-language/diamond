# Indexed assignment works on any receiver expression, not only a plain
# local, @ivar, or @@cvar: here, the Hash a method returns.
class Registry
  def initialize()
    @table = {"hits": 1}
  end
  def table() -> Hash = @table
end
registry = Registry.new()
registry.table()["hits"] += 10
registry.table()["misses"] ||= 5
registry.table()["misses"] ||= 99
registry.table()["names"] = ["a"]
registry.table()["names"][0] = "b"
registry.table()
