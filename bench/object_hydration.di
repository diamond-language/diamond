# Models ActiveRecord::Repository-style row hydration -- skindicate's own
# User class (lib/models/user.di) is the real-world template: a class with
# typed attr_accessor fields, an initialize(attributes: Hash) that pulls
# values out of a Hash "row" with simple conditional defaulting, called once
# per fetched database row. This is the shape found to dominate skindicate's
# own front page: a direct SQLite timing of the same query took ~8ms against
# 8,000+ real rows, while hydrating the ~100 resulting objects into model
# instances took ~25ms -- the query was never the bottleneck, object
# construction was. No real database here (deliberately, for a fast,
# reproducible microbenchmark) -- just the Hash-in, typed-fields-out shape
# repeated at scale.
class HydratedUser
  attr_accessor email: String, username: String, role: String, is_seed

  def initialize(attributes: Hash = {})
    @email = attributes["email"]
    @username = attributes["username"]
    @role = if attributes["role"] == nil then "user" else attributes["role"] end
    @is_seed = attributes["is_seed"] == true
  end
end

def run()
  total = 0
  batch = 0
  while batch < 100
    index = 0
    while index < 100
      row = {"email": "user#{index}@example.com", "username": "user#{index}", "role": "user", "is_seed": false}
      user = HydratedUser.new(row)
      total = total + user.username().length()
      index = index + 1
    end
    batch = batch + 1
  end
  total
end
run()
