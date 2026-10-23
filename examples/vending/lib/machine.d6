# The transition function. `step(state, event)` returns [next_state, messages].
#
# Both `case`s below are exhaustive over a sealed hierarchy: the outer over
# State, the inner over Event. Object patterns (`HasCredit{cents: credit}`)
# read a public reader and bind the result; `^local` compares against an
# existing local instead of binding; an `if` guard can veto a match, which
# then falls through to the next `when`.
require "./model"

# The machine owns the stock and the service key; the CURRENT STATE is not
# stored here but passed in and returned by `step`, which keeps the machine
# a pure state-transition function plus its inventory.
class Machine
  # Coins the machine accepts, largest first (give_back relies on that order).
  COIN_VALUES = [100, 25, 10, 5]

  def initialize(stock: Hash, service_key: String)
    @stock = stock          # slot => [price in cents, count]
    @service_key = service_key
  end

  # "A1 x2 @ $0.75". `[price, count] = ...` destructures the stock entry.
  def stock_line(slot: String) -> String
    [price, count] = @stock[slot]
    "#{slot} x#{count} @ #{money(price)}"
  end

  def slots() -> Array = @stock.keys()

  def step(state: State, event: Event) -> Array
    # `^key` in a pattern below means "equal to the existing local `key`"
    # (rather than binding a new variable), so the key is copied into a local
    # first.
    key = @service_key

    case state
    # Out of service: only the right service key does anything.
    when OutOfService
      case event
      when Service{key: ^key}
        [Idle.new(), ["service mode off; ready"]]
      when Service{}
        [state, ["wrong service key"]]
      when Coin{}, Select{}, Refund{}, Restock{}
        [state, ["out of service"]]
      end
    # Idle (no credit). The guarded Coin arm comes first: a bad coin matches
    # its guard and stops there; a good coin fails the guard, falls through
    # to the next arm, and starts a credit.
    when Idle
      case event
      when Coin{cents: cents} if !COIN_VALUES.include?(cents)
        [state, ["rejected a #{cents}c coin"]]
      when Coin{cents: cents}
        [HasCredit.new(cents), ["credit #{money(cents)}"]]
      when Select{}
        [state, ["insert coins first"]]
      when Refund{}
        [state, ["nothing to refund"]]
      # Restock and Service behave the same in every working state.
      else
        self.maintenance(state, event)
      end
    # Holding credit: the pattern on the state itself binds the amount.
    when HasCredit{cents: credit}
      case event
      when Coin{cents: cents} if !COIN_VALUES.include?(cents)
        [state, ["rejected a #{cents}c coin", "credit #{money(credit)}"]]
      when Coin{cents: cents}
        [HasCredit.new(credit + cents), ["credit #{money(credit + cents)}"]]
      when Select{slot: slot}
        self.vend(credit, slot)
      when Refund{}
        [Idle.new(), self.give_back(credit)]
      else
        self.maintenance(state, event)
      end
    end
  end

  protected

  # Restocking and service mode work the same in any state that reaches here.
  def maintenance(state: State, event: Event) -> Array
    key = @service_key

    case event
    # Add stock. An unknown slot starts at count 0 (fetch's default); the
    # price is always updated to the one given. `[_, before]` ignores the old
    # price.
    when Restock{slot: slot, count: count, price: price}
      [_, before] = @stock.fetch(slot, [price, 0])
      @stock[slot] = [price, before + count]
      [state, ["restocked #{self.stock_line(slot)}"]]
    # Entering service mode must not keep the customer's money: any credit
    # is returned first.
    when Service{key: ^key}
      [OutOfService.new(), ["service mode on"] + self.refund_if_credit(state)]
    when Service{}
      [state, ["wrong service key"]]
    else
      [state, ["ignored"]]
    end
  end

  # Change for the current credit, or no messages if there is none.
  def refund_if_credit(state: State) -> Array
    case state
    when HasCredit{cents: credit} then self.give_back(credit)
    else []
    end
  end

  # Tries to sell `slot` with `credit`. Every failure keeps the credit and
  # stays in HasCredit, so the customer can fix the problem (another coin, a
  # different slot) without losing their money.
  def vend(credit: Int, slot: String) -> Array
    unless @stock.include_key?(slot)
      return [HasCredit.new(credit), ["no such slot #{slot}"]]
    end

    [price, count] = @stock[slot]

    if count == 0
      [HasCredit.new(credit), ["#{slot} is sold out"]]
    elsif credit < price
      [HasCredit.new(credit), ["#{slot} costs #{money(price)}, credit is #{money(credit)}"]]
    # Success: remove one item, return to Idle, and give change if any is
    # owed.
    else
      @stock[slot] = [price, count - 1]
      change = credit - price
      messages = ["dispensed #{slot} for #{money(price)}"]
      messages += self.give_back(change) unless change == 0
      [Idle.new(), messages]
    end
  end

  # Greedy change, largest coin first; the coin set is canonical, so greedy is optimal.
  # Produces one message, e.g. "returned $0.40: 25c 10c 5c".
  def give_back(amount: Int) -> Array
    coins = []
    remaining = amount

    COIN_VALUES.each() do |coin|
      while remaining >= coin
        coins.push(coin)
        remaining -= coin
      end
    end
    ["returned #{money(amount)}: #{coins.map() do |c| "#{c}c" end.join(" ")}"]
  end
end

# 125 -> "$1.25". `rjust(2, "0")` keeps single-digit cents as "05".
def money(cents: Int) -> String
  "$#{cents / 100}.#{(cents % 100).to_s().rjust(2, "0")}"
end
