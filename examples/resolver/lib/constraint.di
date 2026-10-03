# What a dependency accepts. Constraint is SEALED: the four kinds below are
# the only ones, so a `case` over a Constraint must name all four (or have an
# `else`) and the compiler rejects a fifth kind that some `case` forgot.
require "./version"

sealed class Constraint
end

# Any version at all ("*").
class Anything < Constraint
end

# Exactly this version ("=1.2.0").
class Exactly < Constraint
  attr_reader version: Version
  def initialize(version: Version)
    @version = version
  end
end

# This version or newer (">=1.2.0").
class AtLeast < Constraint
  attr_reader version: Version
  def initialize(version: Version)
    @version = version
  end
end

# From `low` up to but not including `high` ("1.0.0..2.0.0").
class Between < Constraint
  attr_reader low: Version
  attr_reader high: Version
  def initialize(low: Version, high: Version)
    @low = low
    @high = high
  end
end

# Whether `candidate` satisfies `constraint`. The `case` is exhaustive over
# the sealed hierarchy, so there is no `else`: it is the compiler, not a
# default branch, that guarantees every kind is handled.
def satisfies?(constraint: Constraint, candidate: Version) -> Bool
  case constraint
  when Anything
    true
  when Exactly{version: wanted}
    candidate == wanted
  when AtLeast{version: floor}
    candidate >= floor
  when Between{low: low, high: high}
    candidate >= low && candidate < high
  end
end

# The text form, for reports; same exhaustiveness guarantee.
def describe(constraint: Constraint) -> String
  case constraint
  when Anything
    "any version"
  when Exactly{version: v}
    "exactly #{v}"
  when AtLeast{version: v}
    "#{v} or newer"
  when Between{low: low, high: high}
    "#{low} up to (not including) #{high}"
  end
end

# Parses the text forms above. A `Version | String` parameter would let
# callers pass either, but here every caller has text.
def parse_constraint(text: String) -> Constraint
  if text == "*"
    Anything.new()
  elsif text.start_with?(">=")
    AtLeast.new(Version.parse(text.slice(2, text.length() - 2)))
  elsif text.start_with?("=")
    Exactly.new(Version.parse(text.slice(1, text.length() - 1)))
  elsif text.include?("..")
    [low, high] = text.split("..")
    Between.new(Version.parse(low), Version.parse(high))
  else
    raise VersionError.new("not a constraint: #{text}")
  end
end
