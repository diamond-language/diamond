# The machine's vocabulary. Both hierarchies are sealed, so a `case` over a
# State or an Event must name every kind (or have an `else`): adding a fifth
# state or a seventh event breaks the build at every `case` that forgot it.

sealed class Event
end

# Events: what a customer or technician can do to the machine.

# A coin is inserted (any value; the machine decides whether it accepts it).
class Coin < Event
  attr_reader cents: Int
  def initialize(cents: Int)
    @cents = cents
  end
end

# A slot is chosen, e.g. "A1".
class Select < Event
  attr_reader slot: String
  def initialize(slot: String)
    @slot = slot
  end
end

# Cancel and take back any credit.
class Refund < Event
end

# A technician adds `count` items to a slot at `price` cents.
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

# States: where the machine is between events.
sealed class State
end

# Ready, no money inserted.
class Idle < State
end

# Money inserted and waiting for a selection; `cents` is the credit.
class HasCredit < State
  attr_reader cents: Int
  def initialize(cents: Int)
    @cents = cents
  end
end

# Locked for maintenance; only the service key gets through.
class OutOfService < State
end
