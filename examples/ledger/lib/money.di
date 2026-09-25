# Money is an immutable value: an Int count of cents plus a currency code.
# Arithmetic goes through overloaded operators, ordering through <=> and
# Comparable, and every instance is frozen as soon as it is built.

class CurrencyMismatch < StandardError
  def initialize(left: String, right: String)
    super("cannot combine #{left} with #{right}")
  end
end

class Money
  include Comparable

  attr_reader currency: String

  def initialize(cents: Int, currency: String = "USD")
    @cents = cents
    @currency = currency
    self.freeze()
  end

  # "12.34" or "-0.05" -> Money. Exactly two decimal places are optional.
  def self.parse(text: String, currency: String = "USD") -> Money
    negative = text.start_with?("-")
    digits = if negative then text.slice(1, text.length()) else text end
    [whole, *fraction] = digits.split(".")
    if fraction.length() > 1 || (fraction.length() == 1 && fraction[0].length() > 2)
      raise ArgumentError.new("not an amount: #{text}")
    end
    cents = whole.to_i() * 100
    if fraction.length() == 1 then cents += fraction[0].ljust(2, "0").to_i() end
    Money.new(if negative then -cents else cents end, currency)
  end

  def self.zero(currency: String = "USD") -> Money = Money.new(0, currency)

  def +(other: Money) -> Money
    self.check_currency(other)
    Money.new(@cents + other.cents(), @currency)
  end

  def -(other: Money) -> Money
    self.check_currency(other)
    Money.new(@cents - other.cents(), @currency)
  end

  # Scaling by a rate rounds to the nearest cent, halves away from zero.
  def *(factor: Int | Float) -> Money
    Money.new((@cents * factor.to_f()).round(), @currency)
  end

  def negate() -> Money = Money.new(-@cents, @currency)

  def <=>(other: Money)
    self.check_currency(other)
    @cents <=> other.cents()
  end

  # Comparable supplies ==, but only for two Money values; this version
  # also answers false for anything else instead of raising.
  def ==(other)
    other is Money && @currency == other.currency() && @cents == other.cents()
  end

  def zero?() -> Bool = @cents == 0
  def negative?() -> Bool = @cents < 0

  # Splits into parts proportional to `ratios` without losing a cent: the
  # remainder goes one cent at a time to the earliest parts.
  def allocate(ratios: Array[Int]) -> Array[Money]
    total = ratios.sum()
    shares = ratios.map() do |ratio| @cents * ratio / total end
    remainder = @cents - shares.sum()
    parts = []
    shares.each_with_index() do |share, index|
      extra = if index < remainder then 1 else 0 end
      parts.push(Money.new(share + extra, @currency))
    end
    parts
  end

  def to_s() -> String
    symbol = case @currency
             when "USD" then "$"
             when "EUR" then "€"
             when "GBP" then "£"
             else "#{@currency} "
             end
    magnitude = abs(@cents)
    body = "#{symbol}#{self.group(magnitude / 100)}.#{"%02d".format(magnitude % 100)}"
    if @cents < 0 then "-#{body}" else body end
  end

  protected

  # Visible to other Money instances (so + and <=> can read it), but not to
  # outside code, which sees only formatted amounts.
  def cents() -> Int = @cents

  private

  def check_currency(other: Money)
    unless other.currency() == @currency
      raise CurrencyMismatch.new(@currency, other.currency())
    end
  end

  # 1234567 -> "1,234,567"
  def group(n: Int) -> String
    digits = n.to_s()
    groups = []
    while digits.length() > 3
      groups.push(digits.slice(digits.length() - 3, 3))
      digits = digits.slice(0, digits.length() - 3)
    end
    groups.push(digits)
    groups.reverse().join(",")
  end
end

def usd(text: String) -> Money = Money.parse(text, "USD")
