# resolver: pick compatible versions of packages from an index.
#
# resolver.di INDEX REQUIREMENT...
# resolver.di --check INDEX
#
# INDEX is a text file, one release per line: "name version [dep:constraint ...]".
# A REQUIREMENT is "name:constraint". Constraints are "*", "=1.2.0", ">=1.2.0"
# and "1.0.0..2.0.0" (up to, not including, the second version). --check lists
# dependencies on packages the index never publishes.
require "./lib/resolver"

def usage() -> Int
  warn("usage: resolver.di INDEX NAME:CONSTRAINT...")
  64
end

# Turns "name:constraint" into the [name, Constraint] pairs `resolve` takes.
def parse_requirement(text: String) -> Array
  parts = text.split(":")
  raise IndexError.new("expected name:constraint, got '#{text}'") unless parts.length() == 2
  [parts[0], parse_constraint(parts[1])]
end

# Prints the outcome. The `case` over the union is exhaustive (no `else`), so
# adding a fourth outcome class to the union breaks this file until handled.
# Returns the exit status: 0 resolved, 1 conflict or missing.
def report(result: Resolved | Conflict | Missing) -> Int
  case result
  when Resolved
    result.picks().each() do |name, version| puts("#{name} #{version}") end
    0
  when Conflict
    puts("conflict: #{result.name()} must be #{describe(result.constraint())} (wanted by #{result.wanted_by()})")
    puts(result.chosen().nil?() ? "  no version of #{result.name()} matches" : "  but #{result.chosen()} is already chosen")
    1
  when Missing
    puts("missing: #{result.name()} (wanted by #{result.wanted_by()}) is not in the index")
    1
  end
end

# `--check`: dependencies naming a package the index does not publish. Every
# release mentions its dependencies independently, so the same missing name
# turns up many times; `unique` (generic: Array[T] -> Array[T]) reports each
# once, in first-seen order.
def check(path: String) -> Int
  index = load_index(path)
  dangling = unique(index.dependency_names()).reject() do |name| index.known?(name) end
  dangling.each() do |name| puts("dangling: #{name}") end
  puts("index ok: #{index.packages().length()} packages") if dangling.empty?()
  dangling.empty?() ? 0 : 1
end

def run(argv: Array) -> Int
  return check(argv[1]) if argv[0] == "--check" && argv.length() == 2
  index = load_index(argv[0])
  roots = argv.slice(1, argv.length() - 1).map() do |text| parse_requirement(text) end
  report(resolve(index, roots))
end

def main(argv) -> Int
  return usage() if argv.length() < 2
  begin
    run(argv)
  rescue error: IOError
    warn(error.message())
    66
  rescue error: IndexError
    warn(error.message())
    65
  rescue error: VersionError
    warn(error.message())
    65
  end
end

exit(main(ARGV))
