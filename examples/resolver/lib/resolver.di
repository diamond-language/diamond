# Dependency resolution: given root requirements, pick one version of every
# package they transitively need, or say precisely why that is impossible.
#
# The three outcomes are separate classes joined by a UNION annotation
# (`Resolved | Conflict | Missing`) rather than a sealed parent, so callers
# get the same exhaustive-`case` check from a plain `A | B | C` type.
require "./index"

# Success: package name => chosen Version, in the order they were chosen.
class Resolved
  attr_reader picks: Hash
  def initialize(picks: Hash)
    @picks = picks
  end
end

# `name` cannot satisfy `constraint` (wanted by `wanted_by`). `chosen` is the
# version already locked in for it, or nil if no version of it matches at all.
class Conflict
  attr_reader name: String
  attr_reader constraint: Constraint
  attr_reader wanted_by: String
  attr_reader chosen: Version | Nil
  def initialize(name: String, constraint: Constraint, wanted_by: String, chosen: Version | Nil)
    @name = name
    @constraint = constraint
    @wanted_by = wanted_by
    @chosen = chosen
  end
end

# The index has no package called `name` (wanted by `wanted_by`).
class Missing
  attr_reader name: String
  attr_reader wanted_by: String
  def initialize(name: String, wanted_by: String)
    @name = name
    @wanted_by = wanted_by
  end
end

# The mutable state of one resolution run: what has been locked in so far.
# A package reached by two paths is locked once; the second visit only checks
# the new constraint against the version already chosen.
class Plan
  attr_reader picks: Hash
  def initialize()
    @picks = {}
  end
end

# Locks in `name` under `constraint`, then recurses into its dependencies.
# Returns nil on success or the first Conflict/Missing found. This is a plain
# depth-first walk with NO backtracking: the first choice for a package is
# final, so a later requirement it cannot meet is reported as a conflict even
# if some other earlier choice would have avoided it.
def pin(index: Index, plan: Plan, name: String, constraint: Constraint, wanted_by: String) -> Conflict | Missing | Nil
  return Missing.new(name, wanted_by) unless index.known?(name)

  # Already locked: the earlier choice must also satisfy this new constraint.
  picks = plan.picks()
  locked = picks[name]
  unless locked.nil?()
    return nil if satisfies?(constraint, locked)
    return Conflict.new(name, constraint, wanted_by, locked)
  end

  # First visit: take the newest matching release, or conflict if none match.
  release = index.newest_matching(name, constraint)
  return Conflict.new(name, constraint, wanted_by, nil) if release.nil?()
  picks[name] = release.version()

  # Depend on every dependency in turn, stopping at the first failure.
  release.needs().each() do |dep|
    [dep_name, dep_constraint] = dep
    problem = pin(index, plan, dep_name, dep_constraint, "#{name} #{release.version()}")
    if problem != nil
      return problem
    end
  end
  nil
end

# Resolves a list of [name, Constraint] root requirements. The `case` on the
# union below has no `else`: it must name Conflict and Missing, and the
# compiler checks that it does.
def resolve(index: Index, roots: Array) -> Resolved | Conflict | Missing
  plan = Plan.new()
  roots.each() do |root|
    [name, constraint] = root
    problem = pin(index, plan, name, constraint, "(root)")
    if problem != nil
      return problem
    end
  end
  Resolved.new(plan.picks())
end
