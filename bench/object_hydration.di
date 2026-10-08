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
#
# `attributes` is a required parameter, not `= {}` (skindicate's real
# User#initialize does default it, but Repository-driven construction --
# the only real call site -- always supplies a real Hash) -- a default
# value's own construction (DIAMOND_OP_HASH, an allocation) is out of
# scope for the Phase 2b JIT slice this benchmark now also exercises
# (docs/internal/jit-design.md/bench/RESULTS.md), which only compiles
# allocation-free functions; a defaulted-but-always-provided parameter
# would otherwise make this whole class permanently JIT-ineligible over
# bytecode that never actually runs for any real call this benchmark makes.
# Same reasoning for @role below: no `"user"` string-literal fallback --
# constructing a literal String is itself an allocation (DIAMOND_OP_
# STRING), and this benchmark's own `row` always supplies "role" anyway,
# so the fallback branch never actually ran even before this change.
#
# Each `attributes["email"]`-shaped read compiles to INDEX_GET_STRING, which looks the
# Hash up by the literal's bytes and allocates no key String (it used to build one with
# DIAMOND_OP_STRING on every call -- see --dump-bytecode). tests/cases/
# jit_hash_ivar_construct.di is the same INDEX_GET/SET_IVAR/CHECK_TYPE/self/argument
# machinery validated with the Hash key passed as a parameter.
class HydratedUser
  attr_accessor email: String, username: String, role: String, is_seed

  def initialize(attributes: Hash)
    @email = attributes["email"]
    @username = attributes["username"]
    @role = attributes["role"]
    @is_seed = attributes["is_seed"] == true
  end
end

# `ActiveRecord::Model#initialize`'s own *current* shape
# (packages/active_record/lib/active_record/model.di) -- rewritten
# 2026-09-15 (docs/internal/jit-design.md's "skindicate ORM hydration
# hotspot" note) from a `keys()`-loop to the native `#dup`, ~20x faster on
# its own. Unlike HydratedUser above, this one IS fully JIT-eligible as of
# Phase 4 (docs/internal/jit-design.md): `attributes.dup()` compiles via
# the new diamond_jit_dup trampoline, and nothing before it in this body
# sets jc->has_called (no comparison, no other call), so the whole
# `initialize` compiles end to end -- verified via `DIAMOND_TRACE_JIT`.
class HydratedModel
  def initialize(attributes: Hash = {})
    @attributes = attributes.dup()
    @association_cache = {}
  end
  def attribute(name) = @attributes[name]
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

def run_dup()
  total = 0
  batch = 0
  while batch < 100
    index = 0
    while index < 100
      row = {"email": "user#{index}@example.com", "username": "user#{index}", "role": "user", "is_seed": false}
      user = HydratedModel.new(row)
      total = total + user.attribute("username").length()
      index = index + 1
    end
    batch = batch + 1
  end
  total
end

run()
run_dup()
