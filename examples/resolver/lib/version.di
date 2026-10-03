# A semantic version ("1.12.0"). It is Comparable, so `<`, `>=`, `between?`,
# `sort` and `max` all work from the single `<=>` below, and it is frozen as
# soon as it is built: a Version that has been compared, stored in an index
# and used as a Hash key must never change under its holders' feet.

# Raised for text that is not three dot-separated non-negative integers.
class VersionError < StandardError
end

class Version
  include Comparable

  attr_reader major: Int
  attr_reader minor: Int
  attr_reader patch: Int

  # Parses "MAJOR.MINOR.PATCH". The three-part check comes first so "1.2" and
  # "1.2.3.4" fail with the same message as "a.b.c".
  def self.parse(text: String) -> Version
    parts = text.split(".")
    raise VersionError.new("not a version: #{text}") unless parts.length() == 3
    numbers = parts.map() do |part|
      raise VersionError.new("not a version: #{text}") unless part.match?("^[0-9]+$")
      part.to_i()
    end
    Version.new(numbers[0], numbers[1], numbers[2])
  end

  # `freeze()` returns the receiver, so `initialize`'s last expression is the
  # frozen instance itself and nothing can reassign an ivar afterwards.
  def initialize(major: Int, minor: Int, patch: Int)
    @major = major
    @minor = minor
    @patch = patch
    self.freeze()
  end

  # Compares major, then minor, then patch. Returning -1/0/1 (not a
  # difference) is the contract Comparable derives everything else from.
  def <=>(other: Version) -> Int
    return major() <=> other.major() unless major() == other.major()
    return minor() <=> other.minor() unless minor() == other.minor()
    patch() <=> other.patch()
  end

  # Hash keys use the string form, so two equal versions are one key.
  def to_s() -> String = "#{@major}.#{@minor}.#{@patch}"
end
