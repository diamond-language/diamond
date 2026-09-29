# The machine's vocabulary. Both hierarchies are sealed, so a `case` over a
# State or an Event must name every kind (or have an `else`): adding a fifth
# state or a seventh event breaks the build at every `case` that forgot it.

sealed class Event
end

class Coin < Event
  attr_reader cents: Int
  def initialize(cents: Int)
    @cents = cents
  end
end

class Select < Event
  attr_reader slot: String
  def initialize(slot: String)
    @slot = slot
  end
end

class Refund < Event
end

class Restock < Event
  attr_reader slot: String
  attr_reader count: Int
  attr_reader price: Int
  def initialize(slot: String, count: Int, price: Int)
    @slot = slot
    @count = count
    @price = price
  end
end

# Toggles service mode; only the right key is honoured.
class Service < Event
  attr_reader key: String
  def initialize(key: String)
    @key = key
  end
end

sealed class State
end

class Idle < State
end

class HasCredit < State
  attr_reader cents: Int
  def initialize(cents: Int)
    @cents = cents
  end
end

class OutOfService < State
end
