# The package index: every published (name, version) and what each one needs.
# Loaded once from a text file, then frozen.
require "./constraint"
require "./set"

# One published version of a package and its dependencies, as
# [name, Constraint] pairs.
struct Release(name: String, version: Version, needs: Array)
end

# A line of the index file the loader could not make sense of.
class IndexError < StandardError
end

class Index
  def initialize()
    # name => Array of Release, kept sorted ascending by version.
    @releases = {}
  end

  # Adds a release, keeping that package's list sorted so `newest_matching`
  # can scan from the end. The sort uses Version's `<=>` via Comparable.
  def add(release: Release) -> Index
    list = @releases.fetch(release.name(), [])
    @releases[release.name()] = (list + [release]).sort_by() do |r| r.version() end
    self
  end

  # Freezing is shallow: `freeze()` on the Index would stop ivar writes but
  # leave the @releases Hash editable, so both are frozen explicitly.
  def seal() -> Index
    @releases.freeze()
    self.freeze()
  end

  def packages() -> Array = @releases.keys()

  def known?(name: String) -> Bool = @releases.key?(name)

  # Every dependency name any release mentions (with repeats).
  def dependency_names() -> Array
    names = []
    @releases.each() do |_, list|
      list.each() do |release|
        release.needs().each() do |dep| names.push(dep[0]) end
      end
    end
    names
  end

  # The highest release of `name` that satisfies `constraint`, or nil when
  # there is none. `Release | Nil` is the honest return type: callers must
  # handle the empty case before touching the result.
  def newest_matching(name: String, constraint: Constraint) -> Release | Nil
    candidates = @releases.fetch(name, []).select() do |r| satisfies?(constraint, r.version()) end
    candidates.empty?() ? nil : candidates.last()
  end
end

# Parses one index line, "name version [dep:constraint ...]", into a Release.
def parse_release(line: String) -> Release
  words = line.split(" ")
  raise IndexError.new("expected 'name version [dep:constraint ...]': #{line}") if words.length() < 2

  # Every word after the version is "dep:constraint".
  needs = words.slice(2, words.length() - 2).map() do |word|
    # Destructuring demands exactly two parts, so check the count first.
    parts = word.split(":")
    raise IndexError.new("bad dependency '#{word}' in: #{line}") unless parts.length() == 2
    [parts[0], parse_constraint(parts[1])]
  end

  Release.new(words[0], Version.parse(words[1]), needs)
end

# Reads the whole index file and returns it FROZEN (see `Index#seal`). After
# this nothing can add to it: any later `add` raises FrozenError rather than
# silently changing what an in-progress resolution sees.
def load_index(path: String) -> Index
  index = Index.new()
  File.read(path).split("\n").each_with_index() do |line, number|
    text = line.strip()
    next if text.empty?() || text.start_with?("#")
    begin
      index.add(parse_release(text))
    rescue error: VersionError
      raise IndexError.new("line #{number + 1}: #{error.message()}")
    end
  end
  index.seal()
end
