# The chart of accounts. Account is sealed, so a `case` over an Account
# must handle all five kinds -- see `normal_side` below.
require "./money"

sealed class Account
  attr_reader name: String

  def initialize(name: String)
    @name = name
    @@opened = (@@opened || 0) + 1
  end

  # How many accounts have been opened, across every subclass.
  def self.opened() -> Int = @@opened || 0

  def to_s() -> String = "#{@name} (#{self.kind()})"
  def kind() -> String = self.class().downcase()
end

class Asset < Account
end

class Liability < Account
end

class Equity < Account
end

class Income < Account
end

class Expense < Account
end

# Which side increases an account. Leaving out a kind is a compile error,
# because Account is sealed.
def normal_side(account: Account) -> Symbol
  case account
  when Asset, Expense then :debit
  when Liability, Equity, Income then :credit
  end
end
