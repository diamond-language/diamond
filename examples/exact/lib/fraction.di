# An exact rational number: a numerator and a positive denominator with no
# common factor. Every instance is frozen and already in lowest terms, so
# two equal fractions always have equal fields and `==` is a field compare.
#
# There is no coercion protocol: `fraction + 1` works only because the
# operators below accept an Int on the right; `1 + fraction` would raise
# TypeError, so code here always puts the Fraction on the left.

class Fraction
  include Comparable

  # Normalizes on construction, which is what makes every Fraction canonical.
  def initialize(numerator: Int, denominator: Int = 1)
    raise ZeroDivisionError.new("fraction with denominator 0") if denominator == 0

    # Keep the sign on the numerator (the denominator must end up positive),
    # then divide both by their greatest common divisor to reach lowest
    # terms: 6/-8 becomes -3/4. The gcd is taken of magnitudes because the
    # sign is already handled.
    sign = if denominator < 0 then -1 else 1 end
    divisor = gcd(magnitude(numerator), magnitude(denominator))
    @numerator = sign * numerator / divisor
    @denominator = sign * denominator / divisor

    # Immutable from here on, so a Fraction can be shared freely.
    self.freeze()
  end

  # "3", "-7", "3/4" or "-6/8" -> Fraction.
  def self.parse(text: String) -> Fraction
    # At most one "/", and each side must be a plain integer (to_i() alone
    # would read "3x" as 3).
    parts = text.split("/")
    unless parts.length() == 1 || parts.length() == 2
      raise ArgumentError.new("not a number: #{text}")
    end
    parts.each() do |part|
      raise ArgumentError.new("not a number: #{text}") unless integer_text?(part)
    end
    Fraction.new(parts[0].to_i(), if parts.length() == 2 then parts[1].to_i() else 1 end)
  end

  # Accepts a Fraction or an Int and returns a Fraction, so the operators
  # below can take either.
  def self.lift(value: Fraction | Int) -> Fraction
    if value is Fraction then value else Fraction.new(value) end
  end

  def numerator() -> Int = @numerator
  def denominator() -> Int = @denominator

  # Arithmetic. Each operator builds a new Fraction (the constructor reduces
  # it). + and - use the common denominator a/b + c/d = (ad + cb) / bd.
  def +(other: Fraction | Int) -> Fraction
    o = Fraction.lift(other)
    Fraction.new(@numerator * o.denominator() + o.numerator() * @denominator,
                 @denominator * o.denominator())
  end

  def -(other: Fraction | Int) -> Fraction
    o = Fraction.lift(other)
    Fraction.new(@numerator * o.denominator() - o.numerator() * @denominator,
                 @denominator * o.denominator())
  end

  def *(other: Fraction | Int) -> Fraction
    o = Fraction.lift(other)
    Fraction.new(@numerator * o.numerator(), @denominator * o.denominator())
  end

  # Division multiplies by the reciprocal; dividing by zero raises.
  def /(other: Fraction | Int) -> Fraction
    o = Fraction.lift(other)
    raise ZeroDivisionError.new("division by the fraction 0") if o.zero?()
    Fraction.new(@numerator * o.denominator(), @denominator * o.numerator())
  end

  # Unary minus (there is no prefix operator method here, so it is a named
  # method).
  def negate() -> Fraction = Fraction.new(-@numerator, @denominator)

  # Comparable derives <, <=, >, >=, between? and clamp from this. Cross
  # multiplication is exact because both denominators are positive.
  def <=>(other: Fraction)
    (@numerator * other.denominator()) <=> (other.numerator() * @denominator)
  end

  # Equality by fields, valid because every Fraction is in lowest terms. The
  # `other is Fraction` check keeps `fraction == "x"` false instead of an
  # error.
  def ==(other)
    other is Fraction && @numerator == other.numerator() && @denominator == other.denominator()
  end

  def zero?() -> Bool = @numerator == 0

  # True for 3/1 (no fractional part).
  def whole?() -> Bool = @denominator == 1

  def to_f() -> Float = @numerator.to_f() / @denominator.to_f()

  def to_s() -> String
    if @denominator == 1 then "#{@numerator}" else "#{@numerator}/#{@denominator}" end
  end
end

# Absolute value.
def magnitude(n: Int) -> Int
  if n < 0 then -n else n end
end

# Euclid's algorithm as a self-recursive tail call. Both arguments are
# non-negative here; `%` also works on arbitrary-precision Ints, which the
# numerators and denominators become.
def gcd(a: Int, b: Int) -> Int
  return a if b == 0
  return gcd(b, a % b)
end

# True for "12" or "-12": an optional leading minus and then only digits.
def integer_text?(text: String) -> Bool
  body = if text.start_with?("-") then text.slice(1, text.length()) else text end
  return false if body.empty?()
  body.chars().all?() do |c| c >= "0" && c <= "9" end
end
