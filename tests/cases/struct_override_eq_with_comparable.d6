# `include Comparable` in a struct body appends its own derived == after the
# struct's generated one; a hand-written == defined after the include must
# still win, so `x == nil` answers false instead of reaching <=> (whose
# parameter is typed).
struct Version(major: Int, minor: Int)
  include Comparable

  def <=>(other: Version)
    return @major <=> other.major() unless @major == other.major()
    @minor <=> other.minor()
  end

  def ==(other)
    other is Version && @major == other.major() && @minor == other.minor()
  end
end

a = Version.new(1, 2)
puts(a == Version.new(1, 2))
puts(a == nil)
puts(a < Version.new(1, 3))
puts(a.between?(Version.new(1, 0), Version.new(2, 0)))
nil
